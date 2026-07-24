#include "query/result_protocol.hpp"

#include <algorithm>
#include <stdexcept>

namespace cidx::protocol {

namespace {

using json_out::Array;
using json_out::Object;
using json_out::Value;

Value optional_string(const std::optional<std::string> &value) {
  return value ? Value::of(*value) : Value::null();
}

Value identity_json(const Identity &identity) {
  Array fact_sets;
  for (const auto &fact_set : identity.fact_sets) {
    fact_sets.push_back(Value::of(fact_set));
  }
  Object out{{"workspace", Value::of(identity.workspace)},
             {"index", Value::of(identity.index)},
             {"fact_sets", Value::arr(std::move(fact_sets))},
             {"freshness", Value::of(identity.freshness)},
             {"source_revision", optional_string(identity.source_revision)},
             {"source_fingerprint", optional_string(identity.source_fingerprint)}};
  return Value::obj(std::move(out));
}

Value producer_json(const Producer &producer) {
  return Value::obj({{"package", Value::of(producer.package)},
                     {"version", Value::of(producer.version)},
                     {"backend", Value::of(producer.backend)},
                     {"schema_version", Value::of(producer.schema_version)}});
}

Value completeness_json(const Completeness &completeness) {
  return Value::obj({{"state", Value::of(completeness.state)},
                     {"truncated", Value::of(completeness.truncated)},
                     {"stale", Value::of(completeness.stale)},
                     {"budget", completeness.budget ? Value::of(*completeness.budget)
                                                       : Value::null()}});
}

Value diagnostic_json(const Diagnostic &diagnostic) {
  Object out{{"code", Value::of(diagnostic.code)},
             {"severity", Value::of(diagnostic.severity)},
             {"message", Value::of(redact_text(diagnostic.message))}};
  if (diagnostic.next_action) {
    out.emplace_back("next_action", Value::of(redact_text(*diagnostic.next_action)));
  }
  return Value::obj(std::move(out));
}

Value evidence_json(const EvidenceNode &node, std::size_t depth,
                    std::size_t &node_count) {
  if (depth > kMaxEvidenceDepth || ++node_count > kMaxEvidenceNodes) {
    throw std::invalid_argument("evidence tree exceeds protocol bounds");
  }
  Object out{{"id", Value::of(node.id)},
             {"class", Value::of(node.evidence_class)},
             {"trust", Value::of(node.trust)},
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
    out.push_back(Value::obj(
        {{"kind", Value::of(artifact.kind)},
         {"id", Value::of(redact_text(artifact.id))},
         {"schema_version", Value::of(artifact.schema_version)},
         {"catalog_version", Value::of(artifact.catalog_version)},
         {"catalog_hash", Value::of(artifact.catalog_hash)}}));
  }
  return Value::arr(std::move(out));
}

} // namespace

std::string_view status_name(Status status) {
  switch (status) {
  case Status::Complete:
    return "complete";
  case Status::Partial:
    return "partial";
  case Status::Unknown:
    return "unknown";
  case Status::Refuted:
    return "refuted";
  case Status::Conditional:
    return "conditional";
  case Status::Error:
    return "error";
  }
  return "error";
}

std::string_view exit_class_name(ExitClass exit_class) {
  switch (exit_class) {
  case ExitClass::Success:
    return "success";
  case ExitClass::Usage:
    return "usage";
  case ExitClass::InvalidOrStaleInput:
    return "invalid_or_stale_input";
  case ExitClass::PolicyFailure:
    return "policy_failure";
  case ExitClass::Unknown:
    return "unknown";
  case ExitClass::InfrastructureFailure:
    return "infrastructure_failure";
  }
  return "infrastructure_failure";
}

int exit_code(ExitClass exit_class) {
  switch (exit_class) {
  case ExitClass::Success:
    return 0;
  case ExitClass::Usage:
    return 2;
  case ExitClass::InvalidOrStaleInput:
    return 3;
  case ExitClass::PolicyFailure:
    return 4;
  case ExitClass::Unknown:
    return 5;
  case ExitClass::InfrastructureFailure:
    return 6;
  }
  return 6;
}

ExitClass ResultEnvelope::exit_class() const {
  if (status == Status::Refuted) {
    return ExitClass::PolicyFailure;
  }
  if (status == Status::Error) {
    for (const auto &diagnostic : diagnostics) {
      if (diagnostic.code == "usage") {
        return ExitClass::Usage;
      }
      if (diagnostic.code == "invalid_input" ||
          diagnostic.code == "stale_input") {
        return ExitClass::InvalidOrStaleInput;
      }
      if (diagnostic.code == "backend_error" ||
          diagnostic.code == "timeout") {
        return ExitClass::InfrastructureFailure;
      }
    }
    return ExitClass::InfrastructureFailure;
  }
  if (status == Status::Unknown) {
    return ExitClass::Unknown;
  }
  return ExitClass::Success;
}

int ResultEnvelope::exit_code() const { return protocol::exit_code(exit_class()); }

bool ResultEnvelope::valid() const {
  if (operation.empty() || identity.workspace.empty() || identity.index.empty() ||
      identity.fact_sets.empty() || producer.package.empty() ||
      producer.version.empty() || producer.backend.empty()) {
    return false;
  }
  if (completeness.truncated && completeness.state == "complete") {
    return false;
  }
  if (completeness.stale && identity.freshness != "stale") {
    return false;
  }
  std::size_t nodes = 0;
  try {
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
  Object out{{"protocol", Value::of(std::string("cidx.result/v1"))},
             {"operation", Value::of(operation)},
             {"status", Value::of(std::string(status_name(status)))},
             {"exit_class", Value::of(std::string(exit_class_name(exit_class())))},
             {"exit_code", Value::of(exit_code())},
             {"result", result},
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

std::string ResultEnvelope::human_text() const {
  std::string out = "status: " + std::string(status_name(status));
  if (completeness.truncated) {
    out += " (truncated)";
  }
  if (completeness.stale) {
    out += " (stale index)";
  }
  if (!diagnostics.empty()) {
    out += "\nreason: " + diagnostics.front().message;
    if (diagnostics.front().next_action) {
      out += "\nnext: " + *diagnostics.front().next_action;
    }
  }
  return out;
}

json_out::Value ProgressEvent::to_json() const {
  Object out{{"protocol", Value::of(std::string("cidx.event/v1"))},
             {"sequence", Value::of(sequence)},
             {"operation", Value::of(operation)},
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
  text.resize(max_bytes - suffix.size());
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
