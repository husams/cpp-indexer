#include "query/result_protocol.hpp"

#include <algorithm>
#include <stdexcept>

namespace cidx::protocol {

namespace {

using json_out::Array;
using json_out::Object;
using json_out::Value;

bool valid_utf8(std::string_view value) {
  for (std::size_t index = 0; index < value.size();) {
    const auto lead = static_cast<unsigned char>(value[index]);
    std::size_t continuation_count = 0;
    unsigned char second_min = 0x80;
    unsigned char second_max = 0xbf;
    if (lead <= 0x7f) {
      ++index;
      continue;
    }
    if (lead >= 0xc2 && lead <= 0xdf) {
      continuation_count = 1;
    } else if (lead == 0xe0) {
      continuation_count = 2;
      second_min = 0xa0;
    } else if (lead >= 0xe1 && lead <= 0xef) {
      continuation_count = 2;
      if (lead == 0xed) {
        second_max = 0x9f;
      }
    } else if (lead == 0xf0) {
      continuation_count = 3;
      second_min = 0x90;
    } else if (lead >= 0xf1 && lead <= 0xf3) {
      continuation_count = 3;
    } else if (lead == 0xf4) {
      continuation_count = 3;
      second_max = 0x8f;
    } else {
      return false;
    }
    if (index + continuation_count >= value.size()) {
      return false;
    }
    const auto second = static_cast<unsigned char>(value[index + 1]);
    if (second < second_min || second > second_max) {
      return false;
    }
    for (std::size_t offset = 2; offset <= continuation_count; ++offset) {
      const auto byte = static_cast<unsigned char>(value[index + offset]);
      if (byte < 0x80 || byte > 0xbf) {
        return false;
      }
    }
    index += continuation_count + 1;
  }
  return true;
}

bool valid_text(std::string_view value) {
  return value.size() <= kMaxTextBytes && valid_utf8(value);
}

bool placeholder_identity(std::string_view value) {
  return std::ranges::find(generated::kPlaceholderIdentities, value) !=
         generated::kPlaceholderIdentities.end();
}

bool code_list_contains(std::string_view list, std::string_view code) {
  std::size_t start = 0;
  while (start <= list.size()) {
    const std::size_t end = list.find('|', start);
    if (list.substr(start, end == std::string_view::npos
                               ? std::string_view::npos
                               : end - start) == code) {
      return true;
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return false;
}

Value optional_string(const std::optional<std::string> &value) {
  return value ? Value::of(redact_text(*value)) : Value::null();
}

Value identity_json(const Identity &identity) {
  Array fact_sets;
  for (const auto &fact_set : identity.fact_sets) {
    fact_sets.push_back(Value::of(redact_text(fact_set)));
  }
  Object out{
      {"workspace", Value::of(redact_text(identity.workspace))},
      {"index", Value::of(redact_text(identity.index))},
      {"fact_sets", Value::arr(std::move(fact_sets))},
      {"freshness", Value::of(redact_text(identity.freshness))},
      {"source_revision", optional_string(identity.source_revision)},
      {"source_fingerprint", optional_string(identity.source_fingerprint)}};
  return Value::obj(std::move(out));
}

Value producer_json(const Producer &producer) {
  return Value::obj({{"package", Value::of(redact_text(producer.package))},
                     {"version", Value::of(redact_text(producer.version))},
                     {"backend", Value::of(redact_text(producer.backend))},
                     {"schema_version", Value::of(producer.schema_version)}});
}

Value completeness_json(const Completeness &completeness) {
  return Value::obj(
      {{"state", Value::of(redact_text(completeness.state))},
       {"truncated", Value::of(completeness.truncated)},
       {"stale", Value::of(completeness.stale)},
       {"budget", completeness.budget ? Value::of(*completeness.budget)
                                      : Value::null()}});
}

Value diagnostic_json(const Diagnostic &diagnostic) {
  Object out{{"code", Value::of(redact_text(diagnostic.code))},
             {"severity", Value::of(redact_text(diagnostic.severity))},
             {"message", Value::of(redact_text(diagnostic.message))}};
  if (diagnostic.next_action) {
    out.emplace_back("next_action",
                     Value::of(redact_text(*diagnostic.next_action)));
  }
  return Value::obj(std::move(out));
}

template <std::size_t N>
bool one_of(std::string_view value,
            const std::array<std::string_view, N> &allowed) {
  return std::ranges::find(allowed, value) != allowed.end();
}

bool valid_identifier(std::string_view value) {
  if (value.empty() || value.front() < 'a' || value.front() > 'z') {
    return false;
  }
  return std::ranges::all_of(value, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '_' || c == '-';
  });
}

Value evidence_json(const EvidenceNode &node, std::size_t depth,
                    std::size_t &node_count) {
  if (depth > kMaxEvidenceDepth || ++node_count > kMaxEvidenceNodes) {
    throw std::invalid_argument("evidence tree exceeds protocol bounds");
  }
  if (!valid_text(node.id) || !valid_text(node.evidence_class) ||
      !valid_text(node.trust) || !valid_text(node.summary) ||
      (node.source && !valid_text(*node.source))) {
    throw std::invalid_argument("evidence text exceeds protocol bounds");
  }
  if (!one_of(node.evidence_class, generated::kEvidenceClasses) ||
      !one_of(node.trust, generated::kTrustLevels)) {
    throw std::invalid_argument("invalid evidence domain");
  }
  Object out{{"id", Value::of(redact_text(node.id))},
             {"class", Value::of(redact_text(node.evidence_class))},
             {"trust", Value::of(redact_text(node.trust))},
             {"summary", Value::of(redact_text(node.summary))}};
  if (node.source) {
    out.emplace_back("source", Value::of(redact_text(*node.source)));
  }
  if (!node.children.empty()) {
    Array children;
    for (const auto &child : node.children) {
      children.push_back(evidence_json(child, depth + 1, node_count));
    }
    out.emplace_back("children", Value::arr(std::move(children)));
  }
  return Value::obj(std::move(out));
}

Value artifacts_json(const std::vector<ArtifactRef> &artifacts) {
  Array out;
  for (const auto &artifact : artifacts) {
    if (!one_of(artifact.kind, generated::kArtifactKinds)) {
      throw std::invalid_argument("invalid artifact kind");
    }
    if (artifact.schema_version < 1 || artifact.catalog_version < 1) {
      throw std::invalid_argument("artifact versions must be positive");
    }
    Object artifact_json{
        {"kind", Value::of(redact_text(artifact.kind))},
        {"id", Value::of(redact_text(artifact.id))},
        {"schema_version", Value::of(artifact.schema_version)},
        {"catalog_version", Value::of(artifact.catalog_version)},
        {"catalog_hash", Value::of(redact_text(artifact.catalog_hash))}};
    if (artifact.generation) {
      artifact_json.emplace_back("generation",
                                 Value::of(redact_text(*artifact.generation)));
    }
    out.push_back(Value::obj(std::move(artifact_json)));
  }
  return Value::arr(std::move(out));
}

Value bounded_value(const Value &value, std::size_t depth) {
  if (depth > generated::kMaxResultDepth) {
    throw std::invalid_argument("result payload exceeds protocol depth");
  }
  switch (value.t) {
  case Value::T::Null:
  case Value::T::Bool:
  case Value::T::Int:
    return value;
  case Value::T::Str:
    if (!valid_utf8(value.s)) {
      throw std::invalid_argument("result payload contains invalid UTF-8");
    }
    return Value::of(redact_text(value.s));
  case Value::T::Arr: {
    if (value.a.size() > generated::kMaxResultItems) {
      throw std::invalid_argument("result payload exceeds protocol item bound");
    }
    Array out;
    out.reserve(value.a.size());
    for (const auto &item : value.a) {
      out.push_back(bounded_value(item, depth + 1));
    }
    return Value::arr(std::move(out));
  }
  case Value::T::Obj: {
    if (value.o.size() > generated::kMaxResultProperties) {
      throw std::invalid_argument(
          "result payload exceeds protocol property bound");
    }
    Object out;
    out.reserve(value.o.size());
    for (const auto &[key, item] : value.o) {
      if (!valid_text(key)) {
        throw std::invalid_argument(
            "result property key exceeds protocol bounds");
      }
      out.emplace_back(key, bounded_value(item, depth + 1));
    }
    return Value::obj(std::move(out));
  }
  }
  throw std::invalid_argument("unsupported result payload type");
}

} // namespace

std::string_view status_name(Status status) {
  const auto index = static_cast<std::size_t>(status);
  if (index >= generated::kStatusNames.size()) {
    throw std::invalid_argument("invalid result status");
  }
  return generated::kStatusNames[index];
}

std::string_view exit_class_name(ExitClass exit_class) {
  const auto index = static_cast<std::size_t>(exit_class);
  if (index >= generated::kExitClassNames.size()) {
    throw std::invalid_argument("invalid exit class");
  }
  return generated::kExitClassNames[index];
}

int exit_code(ExitClass exit_class) {
  const auto index = static_cast<std::size_t>(exit_class);
  if (index >= generated::kExitCodes.size()) {
    throw std::invalid_argument("invalid exit class");
  }
  return generated::kExitCodes[index];
}

ExitClass ResultEnvelope::exit_class() const {
  for (const auto &rule : generated::kExitReasonPrecedence) {
    if (std::ranges::any_of(diagnostics, [&rule](const Diagnostic &diagnostic) {
          return diagnostic.code == rule.code;
        })) {
      return rule.exit_class;
    }
  }
  if (status == Status::Refuted) {
    return ExitClass::PolicyFailure;
  }
  if (status == Status::Error) {
    return ExitClass::InfrastructureFailure;
  }
  return status == Status::Unknown || status == Status::Conditional
             ? ExitClass::Unknown
             : ExitClass::Success;
}

int ResultEnvelope::exit_code() const {
  return protocol::exit_code(exit_class());
}

bool ResultEnvelope::valid() const {
  if (!valid_text(operation) || !valid_identifier(operation) ||
      static_cast<std::size_t>(status) >= generated::kStatusNames.size() ||
      operation.empty() || identity.workspace.empty() ||
      identity.index.empty() || placeholder_identity(identity.workspace) ||
      placeholder_identity(identity.index) || identity.fact_sets.empty() ||
      identity.fact_sets.size() > generated::kMaxFactSets ||
      !valid_text(identity.workspace) || !valid_text(identity.index) ||
      !valid_text(identity.freshness) || !valid_text(producer.package) ||
      !valid_text(producer.version) || !valid_text(producer.backend) ||
      producer.package.empty() || producer.version.empty() ||
      producer.backend.empty() || producer.schema_version < 1 ||
      (identity.source_revision && !valid_text(*identity.source_revision)) ||
      (identity.source_fingerprint &&
       !valid_text(*identity.source_fingerprint)) ||
      !valid_text(completeness.state)) {
    return false;
  }
  if (!one_of(identity.freshness, generated::kFreshness) ||
      !one_of(completeness.state, generated::kCompletenessStates) ||
      std::ranges::any_of(identity.fact_sets,
                          [this](const auto &fact_set) {
                            return std::ranges::count(identity.fact_sets,
                                                      fact_set) > 1;
                          }) ||
      completeness.stale != (identity.freshness == "stale") ||
      std::ranges::any_of(identity.fact_sets, [](const auto &fact_set) {
        return !valid_text(fact_set);
      })) {
    return false;
  }
  const auto has_code = [this](std::string_view code) {
    return std::ranges::any_of(diagnostics, [code](const auto &diagnostic) {
      return diagnostic.code == code;
    });
  };
  if ((status == Status::Complete &&
       (completeness.state != "complete" || completeness.truncated ||
        completeness.stale || identity.freshness != "current")) ||
      (status == Status::Partial && completeness.state != "partial") ||
      (status == Status::Unknown && completeness.state != "unknown") ||
      (status == Status::Conditional && completeness.state != "unknown") ||
      (status == Status::Refuted && completeness.state != "unknown") ||
      (status == Status::Error && completeness.state != "unknown") ||
      (completeness.truncated &&
       std::ranges::find(generated::kTruncatedStatuses, status) ==
           generated::kTruncatedStatuses.end()) ||
      (identity.freshness == "stale" && status != Status::Unknown)) {
    return false;
  }
  if (status == Status::Unknown && !has_code("stale_input") &&
      !has_code("unknown") && !has_code("missing_evidence")) {
    return false;
  }
  if (status == Status::Conditional && !has_code("unknown") &&
      !has_code("missing_evidence")) {
    return false;
  }
  if (status == Status::Refuted && !has_code("policy_refuted")) {
    return false;
  }
  if (has_code("stale_input") &&
      (status != Status::Unknown || identity.freshness != "stale")) {
    return false;
  }
  if ((has_code("backend_error") || has_code("timeout")) &&
      status != Status::Error) {
    return false;
  }
  if (has_code("policy_refuted") && status != Status::Refuted) {
    return false;
  }
  if (status == Status::Error &&
      !std::ranges::any_of(
          generated::kExitReasonPrecedence,
          [&has_code](const auto &rule) { return has_code(rule.code); })) {
    return false;
  }
  if (diagnostics.size() > generated::kMaxDiagnostics ||
      evidence.size() > generated::kMaxEvidence ||
      artifacts.size() > generated::kMaxArtifacts ||
      (replay && replay->arguments.size() > generated::kMaxReplayArguments)) {
    return false;
  }
  if ((completeness.budget && *completeness.budget < 0) ||
      (resources && resources->elapsed_ms && *resources->elapsed_ms < 0) ||
      (resources && resources->peak_bytes && *resources->peak_bytes < 0)) {
    return false;
  }
  for (const auto &diagnostic : diagnostics) {
    if (!one_of(diagnostic.code, generated::kDiagnosticCodes) ||
        !one_of(diagnostic.severity, generated::kDiagnosticSeverities) ||
        !valid_text(diagnostic.code) || !valid_text(diagnostic.severity) ||
        !valid_text(diagnostic.message) ||
        (diagnostic.next_action && !valid_text(*diagnostic.next_action))) {
      return false;
    }
  }
  for (const auto &node : evidence) {
    if (!valid_text(node.id) || !valid_text(node.evidence_class) ||
        !valid_text(node.trust) || !valid_text(node.summary) ||
        (node.source && !valid_text(*node.source))) {
      return false;
    }
  }
  for (const auto &artifact : artifacts) {
    if (!one_of(artifact.kind, generated::kArtifactKinds) ||
        artifact.schema_version < 1 || artifact.catalog_version < 1 ||
        !valid_text(artifact.kind) || !valid_text(artifact.id) ||
        !valid_text(artifact.catalog_hash) ||
        (artifact.generation &&
         (artifact.generation->empty() || !valid_text(*artifact.generation)))) {
      return false;
    }
  }
  if (replay &&
      (!valid_text(replay->command) ||
       std::ranges::any_of(replay->arguments, [](const auto &argument) {
         return !valid_text(argument);
       }))) {
    return false;
  }
  for (const auto &rule : generated::kDiagnosticStatusRules) {
    if (rule.status != status) {
      continue;
    }
    if (!rule.required_any.empty() &&
        !std::ranges::any_of(diagnostics, [&rule](const auto &diagnostic) {
          return code_list_contains(rule.required_any, diagnostic.code);
        })) {
      return false;
    }
    if (std::ranges::any_of(diagnostics, [&rule](const auto &diagnostic) {
          return code_list_contains(rule.forbidden, diagnostic.code);
        })) {
      return false;
    }
    break;
  }
  if (result.t != json_out::Value::T::Obj) {
    return false;
  }
  std::size_t nodes = 0;
  try {
    (void)bounded_value(result, 0);
    for (const auto &node : evidence) {
      (void)evidence_json(node, 1, nodes);
    }
  } catch (const std::invalid_argument &) {
    return false;
  }
  return true;
}

json_out::Value ResultEnvelope::to_json() const {
  if (!valid()) {
    throw std::invalid_argument("invalid result envelope");
  }
  Array diagnostics_json;
  for (const auto &diagnostic : diagnostics) {
    diagnostics_json.push_back(diagnostic_json(diagnostic));
  }
  Array evidence_json_array;
  std::size_t evidence_nodes = 0;
  for (const auto &node : evidence) {
    evidence_json_array.push_back(evidence_json(node, 1, evidence_nodes));
  }
  Object out{
      {"protocol", Value::of(std::string(kProtocol))},
      {"operation", Value::of(redact_text(operation))},
      {"status", Value::of(std::string(status_name(status)))},
      {"exit_class", Value::of(std::string(exit_class_name(exit_class())))},
      {"exit_code", Value::of(exit_code())},
      {"result", bounded_value(result, 0)},
      {"identity", identity_json(identity)},
      {"producer", producer_json(producer)},
      {"completeness", completeness_json(completeness)},
      {"diagnostics", Value::arr(std::move(diagnostics_json))},
      {"evidence", Value::arr(std::move(evidence_json_array))},
      {"artifacts", artifacts_json(artifacts)}};
  if (replay) {
    Array args;
    for (const auto &argument : redact_arguments(replay->arguments)) {
      args.push_back(Value::of(argument));
    }
    out.emplace_back(
        "replay",
        Value::obj({{"command", Value::of(redact_text(replay->command))},
                    {"arguments", Value::arr(std::move(args))}}));
  }
  if (resources) {
    out.emplace_back(
        "resources",
        Value::obj({{"elapsed_ms", resources->elapsed_ms
                                       ? Value::of(*resources->elapsed_ms)
                                       : Value::null()},
                    {"peak_bytes", resources->peak_bytes
                                       ? Value::of(*resources->peak_bytes)
                                       : Value::null()}}));
  }
  return Value::obj(std::move(out));
}

json_out::Value ResultEnvelope::error_status_json() const {
  if (!valid() || status != Status::Error || diagnostics.empty()) {
    throw std::invalid_argument("error status requires an error envelope");
  }
  const auto &diagnostic = diagnostics.front();
  return Value::obj({
      {"status", Value::of(std::string(status_name(Status::Error)))},
      {"code", Value::of(diagnostic.code)},
      {"message", Value::of(redact_text(diagnostic.message))},
      {"exit_class", Value::of(std::string(exit_class_name(exit_class())))},
      {"exit_code", Value::of(exit_code())},
  });
}

std::string ResultEnvelope::human_text() const {
  std::string out = "status: " + std::string(status_name(status));
  if (completeness.truncated) {
    out += " (truncated)";
  }
  if (completeness.stale) {
    out += " (stale index)";
  }
  if (!diagnostics.empty()) {
    out += "\nreason: " + redact_text(diagnostics.front().message);
    if (diagnostics.front().next_action) {
      out += "\nnext: " + redact_text(*diagnostics.front().next_action);
    }
  }
  return redact_text(out, generated::kMaxHumanOutputBytes);
}

json_out::Value ProgressEvent::to_json() const {
  if (sequence < 0 || !valid_identifier(operation) ||
      !one_of(event, generated::kEventKinds) || (completed && *completed < 0) ||
      (total && *total < 0)) {
    throw std::invalid_argument("invalid progress event");
  }
  Object out{{"protocol", Value::of(std::string(kEventProtocol))},
             {"sequence", Value::of(sequence)},
             {"operation", Value::of(redact_text(operation))},
             {"event", Value::of(event)},
             {"message", Value::of(redact_text(message))}};
  if (completed) {
    out.emplace_back("completed", Value::of(*completed));
  }
  if (total) {
    out.emplace_back("total", Value::of(*total));
  }
  return Value::obj(std::move(out));
}

std::string redact_text(std::string_view value, std::size_t max_bytes) {
  std::string text(value);
  constexpr std::string_view marker = "<redacted:secret>";
  for (const std::string_view key :
       {"TOKEN=", "TOKEN:", "PASSWORD=", "PASSWORD:", "SECRET=", "SECRET:"}) {
    std::size_t pos = 0;
    while ((pos = text.find(key, pos)) != std::string::npos) {
      const std::size_t value_start = pos + key.size();
      std::size_t value_end = text.find_first_of(" \t\r\n,;", value_start);
      if (value_end == std::string::npos) {
        value_end = text.size();
      }
      text.replace(value_start, value_end - value_start, marker);
      pos = value_start + marker.size();
    }
  }
  if (text.size() <= max_bytes) {
    return text;
  }
  const std::string suffix = "...<redacted:size-limit>";
  if (max_bytes <= suffix.size()) {
    return suffix.substr(0, max_bytes);
  }
  std::size_t keep = max_bytes - suffix.size();
  while (keep > 0 && keep < text.size() &&
         (static_cast<unsigned char>(text[keep]) & 0xC0U) == 0x80U) {
    --keep;
  }
  text.resize(keep);
  text += suffix;
  return text;
}

std::vector<std::string>
redact_arguments(const std::vector<std::string> &arguments,
                 std::size_t max_bytes) {
  std::vector<std::string> out;
  out.reserve(arguments.size());
  for (const auto &argument : arguments) {
    out.push_back(redact_text(argument, max_bytes));
  }
  return out;
}

} // namespace cidx::protocol
