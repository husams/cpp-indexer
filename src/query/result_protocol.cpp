#include "query/result_protocol.hpp"

#include <algorithm>
#include <stdexcept>

namespace cidx::protocol {

namespace {

using json_out::Array;
using json_out::Object;
using json_out::Value;

Value optional_string(const std::optional<std::string> &value) {
  return value ? Value::of(redact_text(*value)) : Value::null();
}

Value identity_json(const Identity &identity) {
  Array fact_sets;
  for (const auto &fact_set : identity.fact_sets) {
    fact_sets.push_back(Value::of(redact_text(fact_set)));
  }
  Object out{{"workspace", Value::of(redact_text(identity.workspace))},
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
  return Value::obj({{"state", Value::of(redact_text(completeness.state))},
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
    out.emplace_back("next_action", Value::of(redact_text(*diagnostic.next_action)));
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
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-';
  });
}

Value evidence_json(const EvidenceNode &node, std::size_t depth,
                    std::size_t &node_count) {
  if (depth > kMaxEvidenceDepth || ++node_count > kMaxEvidenceNodes) {
    throw std::invalid_argument("evidence tree exceeds protocol bounds");
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
    out.push_back(Value::obj(
        {{"kind", Value::of(redact_text(artifact.kind))},
         {"id", Value::of(redact_text(artifact.id))},
         {"schema_version", Value::of(artifact.schema_version)},
         {"catalog_version", Value::of(artifact.catalog_version)},
         {"catalog_hash", Value::of(redact_text(artifact.catalog_hash))}}));
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
      throw std::invalid_argument("result payload exceeds protocol property bound");
    }
    Object out;
    out.reserve(value.o.size());
    for (const auto &[key, item] : value.o) {
      out.emplace_back(redact_text(key), bounded_value(item, depth + 1));
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
    if (std::any_of(diagnostics.begin(), diagnostics.end(),
                    [&rule](const Diagnostic &diagnostic) {
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

int ResultEnvelope::exit_code() const { return protocol::exit_code(exit_class()); }

bool ResultEnvelope::valid() const {
  if (!valid_identifier(operation) ||
      static_cast<std::size_t>(status) >= generated::kStatusNames.size() ||
      operation.empty() || identity.workspace.empty() || identity.index.empty() ||
      identity.workspace == "unknown" || identity.workspace == "workspace:unknown" ||
      identity.index == "unknown" || identity.fact_sets.empty() ||
      identity.fact_sets.size() > generated::kMaxFactSets || producer.package.empty() ||
      producer.version.empty() || producer.backend.empty() ||
      producer.schema_version < 1) {
    return false;
  }
  if (!one_of(identity.freshness, generated::kFreshness) ||
      !one_of(completeness.state, generated::kCompletenessStates) ||
      std::ranges::any_of(identity.fact_sets, [this](const auto &fact_set) {
        return std::ranges::count(identity.fact_sets, fact_set) > 1;
      }) ||
      completeness.stale != (identity.freshness == "stale")) {
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
      (completeness.truncated && status != Status::Partial) ||
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
  if (status == Status::Error &&
      !std::any_of(generated::kExitReasonPrecedence.begin(),
                   generated::kExitReasonPrecedence.end(),
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
        !one_of(diagnostic.severity, generated::kDiagnosticSeverities)) {
      return false;
    }
  }
  for (const auto &artifact : artifacts) {
    if (!one_of(artifact.kind, generated::kArtifactKinds) ||
        artifact.schema_version < 1 || artifact.catalog_version < 1) {
      return false;
    }
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
  Object out{{"protocol", Value::of(std::string(kProtocol))},
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
    out.emplace_back("replay", Value::obj({
        {"command", Value::of(redact_text(replay->command))},
        {"arguments", Value::arr(std::move(args))}}));
  }
  if (resources) {
    out.emplace_back("resources", Value::obj({
        {"elapsed_ms", resources->elapsed_ms ? Value::of(*resources->elapsed_ms)
                                               : Value::null()},
        {"peak_bytes", resources->peak_bytes ? Value::of(*resources->peak_bytes)
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
      !one_of(event, generated::kEventKinds) ||
      (completed && *completed < 0) || (total && *total < 0)) {
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
  for (const std::string_view key : {"TOKEN=", "TOKEN:", "PASSWORD=",
                                     "PASSWORD:", "SECRET=", "SECRET:"}) {
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

std::vector<std::string> redact_arguments(const std::vector<std::string> &arguments,
                                          std::size_t max_bytes) {
  std::vector<std::string> out;
  out.reserve(arguments.size());
  for (const auto &argument : arguments) {
    out.push_back(redact_text(argument, max_bytes));
  }
  return out;
}

} // namespace cidx::protocol
