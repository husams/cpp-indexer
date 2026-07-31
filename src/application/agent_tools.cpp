#include "application/agent_tools.hpp"

#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

#include "catalogs/generated_catalog.hpp"
#include "query/cxq.hpp"
#include "query/exec.hpp"
#include "util/json_read.hpp"
#include "util/version.hpp"

namespace cidx::agent {
namespace {

constexpr std::array<std::string_view, 2> kTools = {"query", "explain"};

const json_out::Value *field(const json_out::Value &value,
                             std::string_view name) {
  if (value.t != json_out::Value::T::Obj) {
    return nullptr;
  }
  for (const auto &[key, child] : value.o) {
    if (key == name) {
      return &child;
    }
  }
  return nullptr;
}

std::string required_string(const json_out::Value &object,
                            std::string_view name) {
  const json_out::Value *value = field(object, name);
  if (value == nullptr || value->t != json_out::Value::T::Str ||
      value->s.empty()) {
    throw std::invalid_argument("E_PROTOCOL_SCHEMA: '" + std::string(name) +
                                "' must be a non-empty string");
  }
  return value->s;
}

int64_t required_integer(const json_out::Value &object, std::string_view name) {
  const json_out::Value *value = field(object, name);
  if (value == nullptr || value->t != json_out::Value::T::Int) {
    throw std::invalid_argument("E_PROTOCOL_SCHEMA: '" + std::string(name) +
                                "' must be an integer");
  }
  return value->i;
}

protocol::ResultEnvelope envelope_base(const IndexIdentity &index,
                                       std::string operation) {
  protocol::ResultEnvelope result;
  result.operation = std::move(operation);
  result.identity.workspace = index.workspace;
  result.identity.index =
      "semantic-index/schema/" + std::to_string(index.schema_version);
  result.identity.fact_sets = {"symbols"};
  result.identity.freshness = index.freshness;
  result.identity.source_revision = index.source_revision;
  result.identity.source_fingerprint = index.source_fingerprint;
  result.producer.package = "cidx";
  result.producer.version = std::string(version::kFullProductVersion);
  result.producer.backend = "cpp";
  result.producer.schema_version = protocol::kProtocolVersion;
  result.evidence.push_back(
      protocol::EvidenceNode{.id = "queryplan",
                             .evidence_class = "derived",
                             .trust = "producer-verified",
                             .summary = "bounded QueryPlan execution",
                             .source = std::nullopt,
                             .children = {}});
  result.artifacts.push_back(
      protocol::ArtifactRef{.kind = "semantic-index",
                            .id = result.identity.index,
                            .schema_version = index.schema_version,
                            .catalog_version = catalog::kCatalogVersion,
                            .catalog_hash = std::string(catalog::kCatalogHash),
                            .generation = std::nullopt});
  return result;
}

protocol::ResultEnvelope failure(const IndexIdentity &index,
                                 std::string operation, std::string code,
                                 std::string message) {
  protocol::ResultEnvelope result = envelope_base(index, std::move(operation));
  result.status = code == "policy_refuted" ? protocol::Status::Refuted
                                           : protocol::Status::Error;
  result.completeness.state = "unknown";
  result.completeness.stale = index.freshness == "stale";
  result.result = json_out::Value::obj({});
  result.diagnostics.push_back(
      protocol::Diagnostic{.code = std::move(code),
                           .severity = "error",
                           .message = std::move(message),
                           .next_action = std::nullopt});
  return result;
}

void require_row_evidence(protocol::ResultEnvelope &result) {
  if (result.completeness.truncated ||
      result.result.t != json_out::Value::T::Obj) {
    return;
  }
  const json_out::Value *rows = field(result.result, "rows");
  if (rows == nullptr || rows->t != json_out::Value::T::Arr) {
    return;
  }
  for (const auto &row : rows->a) {
    const json_out::Value *file = field(row, "file");
    const json_out::Value *line = field(row, "line");
    if (file == nullptr || file->t == json_out::Value::T::Null ||
        line == nullptr || line->t == json_out::Value::T::Null) {
      result.status = protocol::Status::Unknown;
      result.completeness.state = "unknown";
      result.diagnostics.push_back(protocol::Diagnostic{
          .code = "missing_evidence",
          .severity = "warning",
          .message = "result row has no resolvable file/line provenance",
          .next_action = "select file and line evidence or inspect the index"});
      return;
    }
  }
}

void apply_freshness(protocol::ResultEnvelope &result,
                     const IndexIdentity &index) {
  if (index.freshness == "stale") {
    result.status = protocol::Status::Unknown;
    result.completeness.state = "unknown";
    result.completeness.stale = true;
    result.diagnostics.push_back(protocol::Diagnostic{
        .code = "stale_input",
        .severity = "error",
        .message = "index contents are stale for the workspace",
        .next_action =
            "re-index the affected sources before relying on this result"});
  } else if (index.freshness != "current") {
    result.status = protocol::Status::Unknown;
    result.completeness.state = "unknown";
    result.diagnostics.push_back(protocol::Diagnostic{
        .code = "unknown",
        .severity = "warning",
        .message = "index freshness could not be verified",
        .next_action =
            "stamp or re-index the workspace before relying on this result"});
  } else {
    result.status = protocol::Status::Complete;
    result.completeness.state = "complete";
  }
}

IndexIdentity index_identity(application::ApplicationContext &context) {
  if (context.read_ports().query != nullptr) {
    return context.read_ports().query->index_identity();
  }
  IndexIdentity fallback;
  fallback.schema_version = 1;
  fallback.workspace = "workspace:agent";
  fallback.freshness = "unverifiable";
  return fallback;
}

} // namespace

std::string_view tool_name(Tool tool) noexcept {
  switch (tool) {
  case Tool::query:
    return kTools[0];
  case Tool::explain:
    return kTools[1];
  }
  return {};
}

std::span<const std::string_view> tool_catalog() noexcept { return kTools; }

Request ToolService::decode_request(const std::string &json) {
  const json_out::Value root = json_read::parse(json);
  if (root.t != json_out::Value::T::Obj) {
    throw std::invalid_argument("E_PROTOCOL_SCHEMA: request must be an object");
  }
  const int64_t version = required_integer(root, "version");
  if (version != kProtocolVersion) {
    throw std::invalid_argument(
        "E_PROTOCOL_VERSION: unsupported agent protocol version " +
        std::to_string(version));
  }
  const std::string name = required_string(root, "tool");
  Request request;
  request.version = static_cast<int>(version);
  if (name == "query") {
    request.tool = Tool::query;
  } else if (name == "explain") {
    request.tool = Tool::explain;
  } else {
    throw std::invalid_argument("E_TOOL: unsupported agent tool '" + name +
                                "'");
  }
  request.query.expression = required_string(root, "cxq");
  request.query.output = application::QueryOutput::json;
  request.query.explain = request.tool == Tool::explain;
  if (const json_out::Value *budget = field(root, "budget");
      budget != nullptr) {
    if (budget->t != json_out::Value::T::Obj) {
      throw std::invalid_argument(
          "E_PROTOCOL_SCHEMA: 'budget' must be an object");
    }
    request.budget.max_results = required_integer(*budget, "max_results");
  }
  if (request.budget.max_results < 1 ||
      request.budget.max_results > kMaxResults) {
    throw std::invalid_argument("E_BUDGET: max_results must be between 1 and " +
                                std::to_string(kMaxResults));
  }
  request.query.max_results = request.budget.max_results;
  return request;
}

protocol::ResultEnvelope
ToolService::invoke(const Request &request,
                    application::ApplicationContext &context) {
  const IndexIdentity index = index_identity(context);
  const std::string operation(tool_name(request.tool));
  if (request.version != kProtocolVersion) {
    return failure(index, operation, "invalid_input",
                   "E_PROTOCOL_VERSION: unsupported agent protocol version " +
                       std::to_string(request.version));
  }
  if (tool_name(request.tool).empty()) {
    return failure(index, "agent", "invalid_input",
                   "E_TOOL: unsupported agent tool");
  }
  if (request.budget.max_results < 1 ||
      request.budget.max_results > kMaxResults) {
    return failure(index, operation, "invalid_input",
                   "E_BUDGET: max_results must be between 1 and " +
                       std::to_string(kMaxResults));
  }
  if (!context.permits(
          application::capability_bit(application::Capability::index_read))) {
    return failure(index, operation, "policy_refuted",
                   "agent tools require the index_read capability");
  }
  if (context.read_ports().query == nullptr) {
    return failure(index, operation, "backend_error",
                   "query read port is not installed");
  }
  try {
    const query::Plan plan = query::parse_cxq(request.query.expression);
    query::Executor executor(*context.read_ports().query);
    if (request.tool == Tool::query) {
      query::Result executed =
          executor.run(plan, std::nullopt, request.budget.max_results);
      protocol::ResultEnvelope result = executed.to_envelope();
      result.operation = operation;
      require_row_evidence(result);
      return result;
    }
    protocol::ResultEnvelope result = envelope_base(index, operation);
    result.result = executor.explain(plan);
    apply_freshness(result, index);
    return result;
  } catch (const std::exception &error) {
    return failure(index, operation, "invalid_input", error.what());
  }
}

json_out::Value
ToolService::encode_response(const Request &request,
                             const protocol::ResultEnvelope &response) {
  return json_out::Value::obj(
      {{"protocol", json_out::Value::of(std::string(kProtocol))},
       {"version", json_out::Value::of(kProtocolVersion)},
       {"tool", json_out::Value::of(std::string(tool_name(request.tool)))},
       {"response", response.to_json()},
       {"truncated", json_out::Value::of(response.completeness.truncated)},
       {"budget",
        json_out::Value::obj(
            {{"max_results", json_out::Value::of(request.budget.max_results)},
             {"exhausted",
              json_out::Value::of(response.completeness.budget.has_value())},
             {"exhausted_at",
              response.completeness.budget
                  ? json_out::Value::of(*response.completeness.budget)
                  : json_out::Value::null()}})}});
}

json_out::Value
ToolService::invoke_json(const std::string &json,
                         application::ApplicationContext &context) {
  try {
    const Request request = decode_request(json);
    const protocol::ResultEnvelope response = invoke(request, context);
    return encode_response(request, response);
  } catch (const std::exception &error) {
    Request request;
    request.tool = Tool::query;
    request.query.expression = "";
    const protocol::ResultEnvelope response = failure(
        index_identity(context), "query", "invalid_input", error.what());
    return encode_response(request, response);
  }
}

} // namespace cidx::agent
