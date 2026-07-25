#include "application/services.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

#include "ast/index_engine.hpp"
#include "diff/analyze.hpp"
#include "diff/compare.hpp"
#include "diff/report.hpp"
#include "diff/target.hpp"
#include "include_hygiene/analysis.hpp"
#include "include_hygiene/executor.hpp"
#include "include_hygiene/plan.hpp"
#include "query/cxq.hpp"
#include "query/exec.hpp"
#include "storage/storage.hpp"
#include "util/files.hpp"
#include "util/pathutil.hpp"

namespace cidx::application {
namespace {

protocol::ResultEnvelope base_result(std::string operation,
                                     const ApplicationContext &context) {
  protocol::ResultEnvelope result;
  result.operation = std::move(operation);
  result.identity.fact_sets = {"application"};
  result.identity.freshness = "current";
  result.identity.index = "application-context";
  result.identity.workspace = context.workspace() == nullptr
                                  ? "test-context"
                                  : context.workspace()->snapshot().identity;
  result.producer.backend = "cpp";
  return result;
}

protocol::ResultEnvelope service_result(std::string operation,
                                        const ApplicationContext &context,
                                        std::string service) {
  protocol::ResultEnvelope result = base_result(std::move(operation), context);
  result.result = json_out::Value::obj(
      {{"service", json_out::Value::of(std::move(service))}});
  return result;
}

protocol::ResultEnvelope service_error(std::string operation,
                                       const ApplicationContext &context,
                                       std::string code, std::string message) {
  protocol::ResultEnvelope result = base_result(std::move(operation), context);
  result.status = code == "policy_refuted" ? protocol::Status::Refuted
                                           : protocol::Status::Error;
  result.completeness.state = "unknown";
  result.diagnostics.push_back(
      protocol::Diagnostic{.code = std::move(code),
                           .severity = "error",
                           .message = std::move(message)});
  return result;
}

json_out::Value count_result(std::int64_t indexed, std::int64_t failed,
                             std::int64_t skipped) {
  return json_out::Value::obj({{"indexed", json_out::Value::of(indexed)},
                               {"failed", json_out::Value::of(failed)},
                               {"skipped", json_out::Value::of(skipped)}});
}

std::string operation_name(const std::optional<Operation> &operation) {
  if (!operation) {
    return "application";
  }
  if (const CommandMetadata *entry = metadata(*operation); entry != nullptr) {
    return std::string(entry->group) + "." + std::string(entry->name);
  }
  return "application";
}

} // namespace

protocol::ResultEnvelope
DefaultApplicationServices::index(const IndexRequest &request,
                                  ApplicationContext &context) const {
  if (request.action == IndexAction::status &&
      context.read_ports().schema != nullptr) {
    const Stats stats = context.read_ports().schema->stats();
    protocol::ResultEnvelope result =
        service_result("index.status", context, "index");
    result.result =
        json_out::Value::obj({{"files", json_out::Value::of(stats.files)},
                              {"symbols", json_out::Value::of(stats.symbols)},
                              {"edges", json_out::Value::of(stats.edges)}});
    return result;
  }
  if (context.storage() == nullptr) {
    return service_error("index", context, "backend_error",
                         "index storage is not installed");
  }
  Storage &db = *context.storage();
  if (request.action == IndexAction::explain) {
    protocol::ResultEnvelope result =
        service_result("index.explain", context, "index");
    result.result = json_out::Value::obj(
        {{"workspace",
          json_out::Value::of(context.workspace() == nullptr
                                  ? std::string{}
                                  : context.workspace()->snapshot().identity)},
         {"index", json_out::Value::of(db.index_identity().freshness)}});
    return result;
  }

  std::vector<std::pair<File, std::string>> targets;
  const bool rebuild = request.action == IndexAction::rebuild;
  if (rebuild) {
    for (const auto &[file, path] : db.list_files()) {
      db.set_file_indexed(file.id, false);
      if (!files::is_header(path)) {
        targets.emplace_back(file, path);
      }
    }
  } else if (!request.files.empty()) {
    for (const std::string &file_arg : request.files) {
      const std::string path = files::resolve_file_arg(file_arg);
      if (const auto file = db.get_file(path)) {
        targets.emplace_back(*file, path);
      }
    }
  } else {
    for (const auto &[file, path] : db.list_files()) {
      if (!files::is_header(path) &&
          files::index_status(file, path) != files::IndexStatus::kOk) {
        targets.emplace_back(file, path);
      }
    }
  }

  std::int64_t indexed = 0;
  std::int64_t failed = 0;
  for (const auto &[file, path] : targets) {
    if (context.cancellation().cancelled()) {
      break;
    }
    const ast::IndexOneOutcome outcome =
        ast::run_index_one(db, file, path, request.graph);
    db.replace_diagnostics(file.id, outcome.diagnostics);
    if (outcome.parse_failed || outcome.source_changed) {
      db.set_file_indexed(file.id, false);
      ++failed;
    } else {
      db.mark_file_indexed(file.id, std::nullopt, outcome.source_md5);
      ++indexed;
    }
  }
  protocol::ResultEnvelope result = service_result("index", context, "index");
  result.result = count_result(indexed, failed,
                               static_cast<std::int64_t>(targets.size()) -
                                   indexed - failed);
  if (failed != 0) {
    result.status = protocol::Status::Partial;
    result.completeness.state = "partial";
  }
  return result;
}

protocol::ResultEnvelope
DefaultApplicationServices::query(const QueryRequest &request,
                                  ApplicationContext &context) const {
  if (context.read_ports().query == nullptr) {
    return service_error("query", context, "backend_error",
                         "query read port is not installed");
  }
  try {
    const query::Plan plan = query::parse_cxq(request.expression);
    query::Executor executor(*context.read_ports().query);
    protocol::ResultEnvelope result = base_result("query", context);
    result.result =
        request.explain ? executor.explain(plan) : executor.run(plan).to_json();
    return result;
  } catch (const std::exception &error) {
    return service_error("query", context, "invalid_input", error.what());
  }
}

protocol::ResultEnvelope
DefaultApplicationServices::analysis(const AnalysisRequest &request,
                                     ApplicationContext &context) const {
  if (request.action == AnalysisAction::list) {
    protocol::ResultEnvelope result =
        service_result("analysis.list", context, "analysis");
    json_out::Array rules;
    rules.push_back(json_out::Value::of(std::string("cycles")));
    rules.push_back(json_out::Value::of(std::string("calls")));
    result.result = json_out::Value::obj(
        {{"rules", json_out::Value::arr(std::move(rules))}});
    return result;
  }
  if (context.storage() == nullptr) {
    return service_error("analysis", context, "backend_error",
                         "analysis storage is not installed");
  }
  protocol::ResultEnvelope result =
      service_result("analysis", context, "analysis");
  result.result = json_out::Value::obj(
      {{"files", json_out::Value::of(context.storage()->stats().files)},
       {"symbols", json_out::Value::of(context.storage()->stats().symbols)},
       {"edges", json_out::Value::of(context.storage()->stats().edges)}});
  return result;
}

protocol::ResultEnvelope
DefaultApplicationServices::workspace(const WorkspaceRequest &request,
                                      ApplicationContext &context) const {
  if (context.workspace() == nullptr) {
    return service_error("workspace", context, "backend_error",
                         "workspace context is not installed");
  }
  protocol::ResultEnvelope result =
      service_result("workspace", context, "workspace");
  result.result = json_out::Value::obj(
      {{"action", json_out::Value::of(static_cast<int>(request.action))},
       {"identity",
        json_out::Value::of(context.workspace()->snapshot().identity)}});
  return result;
}

protocol::ResultEnvelope
DefaultApplicationServices::ast(const AstInspectionRequest &,
                                ApplicationContext &context) const {
  if (context.read_ports().source == nullptr) {
    return service_error("ast", context, "backend_error",
                         "source read port is not installed");
  }
  return service_result("ast", context, "ast-inspection");
}

protocol::ResultEnvelope
DefaultApplicationServices::diff(const DiffRequest &request,
                                 ApplicationContext &context) const {
  try {
    if (request.scope == DiffScope::index) {
      protocol::ResultEnvelope result =
          service_result("diff.index", context, "diff");
      result.result = json_out::Value::obj(
          {{"left", json_out::Value::of(
                        request.left_index_identity.value_or(request.left))},
           {"right", json_out::Value::of(
                         request.right_index_identity.value_or(request.right))},
           {"equal", json_out::Value::of(request.left_index_identity ==
                                         request.right_index_identity)}});
      return result;
    }
    if (request.scope == DiffScope::source) {
      protocol::ResultEnvelope result =
          service_result("diff.source", context, "diff");
      result.result = json_out::Value::obj(
          {{"left", json_out::Value::of(
                        request.left_source_revision.value_or(request.left))},
           {"right", json_out::Value::of(request.right_source_revision.value_or(
                         request.right))},
           {"equal", json_out::Value::of(request.left_source_revision ==
                                         request.right_source_revision)}});
      return result;
    }

    const std::string left_db = request.left_index.value_or("");
    if (left_db.empty()) {
      return service_error("diff", context, "invalid_input",
                           "file and symbol diffs require an index path");
    }
    const diff::ParseConfig left_config =
        diff::resolve_parse_config(diff::SideSpec{.side = "left",
                                                  .file = request.left,
                                                  .db = left_db,
                                                  .tu = std::nullopt});
    const diff::ParseConfig right_config = diff::resolve_parse_config(
        diff::SideSpec{.side = "right",
                       .file = request.right,
                       .db = request.right_index.value_or(left_db),
                       .tu = std::nullopt});
    const diff::ConfigDelta delta =
        diff::config_delta(left_config, right_config);
    if (request.scope == DiffScope::configuration) {
      protocol::ResultEnvelope result =
          service_result("diff.configuration", context, "diff");
      result.result = json_out::Value::obj(
          {{"identical", json_out::Value::of(delta.identical)},
           {"includes_changed", json_out::Value::of(delta.includes_changed)},
           {"options_reordered",
            json_out::Value::of(delta.options_reordered)}});
      return result;
    }

    std::optional<diff::Selector> selector;
    if (request.scope == DiffScope::symbol) {
      if (!request.selector) {
        return service_error("diff.symbol", context, "invalid_input",
                             "symbol diffs require a selector");
      }
      selector = diff::Selector{.raw = *request.selector};
    }
    const diff::SideAnalysis left = diff::analyze_side(left_config, selector);
    const diff::SideAnalysis right = diff::analyze_side(right_config, selector);
    const diff::Comparison comparison = diff::compare_sides(
        left, right, "heuristic", delta,
        request.scope == DiffScope::symbol ? "symbol" : "file");
    std::ostringstream rendered;
    diff::render_report(
        diff::ReportSpec{.scope = request.scope == DiffScope::symbol ? "symbol"
                                                                     : "file",
                         .mode = "both",
                         .match = "heuristic",
                         .json = request.json},
        left, right, delta, comparison, rendered);
    protocol::ResultEnvelope result = service_result("diff", context, "diff");
    result.result = json_out::Value::of(rendered.str());
    return result;
  } catch (const std::exception &error) {
    return service_error("diff", context, "backend_error", error.what());
  }
}

protocol::ResultEnvelope
DefaultApplicationServices::include(const IncludeRequest &request,
                                    ApplicationContext &context) const {
  if (context.storage() == nullptr) {
    return service_error("include", context, "backend_error",
                         "include storage is not installed");
  }
  hygiene::AnalysisOptions options;
  options.scope_paths = request.paths;
  options.want_duplicates = request.duplicates || !request.unused;
  options.want_unused = request.unused || !request.duplicates;
  const hygiene::AnalysisResult analysis =
      hygiene::analyze(*context.storage(), options);
  protocol::ResultEnvelope result =
      service_result("include", context, "include-hygiene");
  result.result = json_out::Value::obj(
      {{"candidates", json_out::Value::of(static_cast<std::int64_t>(
                          analysis.candidates.size()))},
       {"uncovered", json_out::Value::of(static_cast<std::int64_t>(
                         analysis.uncovered_scope.size()))}});
  if (request.action == IncludeAction::plan) {
    if (!request.output || context.workspace() == nullptr) {
      return service_error(
          "include.plan", context, "invalid_input",
          "include plans require an output path and workspace");
    }
    const hygiene::CleanupPlan plan = hygiene::build_plan(
        *context.storage(), analysis, context.workspace()->index_path());
    std::ofstream output(*request.output);
    if (!output) {
      return service_error("include.plan", context, "backend_error",
                           "cannot write include plan");
    }
    output << hygiene::serialize(plan);
  } else if (request.action == IncludeAction::apply) {
    if (!request.plan) {
      return service_error("include.apply", context, "invalid_input",
                           "include apply requires a plan path");
    }
    std::ifstream input(*request.plan);
    if (!input) {
      return service_error("include.apply", context, "backend_error",
                           "cannot read include plan");
    }
    const hygiene::CleanupPlan plan = hygiene::deserialize(
        std::string((std::istreambuf_iterator<char>(input)),
                    std::istreambuf_iterator<char>()));
    const hygiene::ExecuteResult execution =
        hygiene::execute(*context.storage(), plan,
                         hygiene::ExecuteOptions{.dry_run = request.dry_run,
                                                 .only = request.only});
    result.result = json_out::Value::obj(
        {{"ok", json_out::Value::of(execution.ok)},
         {"removed", json_out::Value::of(execution.removed)},
         {"skipped", json_out::Value::of(execution.skipped)}});
    if (!execution.ok) {
      result.status = protocol::Status::Refuted;
      result.completeness.state = "unknown";
    }
  }
  return result;
}

protocol::ResultEnvelope
DefaultApplicationServices::refactor(const RefactoringRequest &,
                                     ApplicationContext &context) const {
  return service_result("refactor", context, "checked-refactoring");
}

protocol::ResultEnvelope
DefaultApplicationServices::proof(const ProofRequest &,
                                  ApplicationContext &context) const {
  return service_result("proof", context, "proof-orchestration");
}

protocol::ResultEnvelope
ApplicationService::failure(const std::optional<Operation> &operation,
                            std::string code, std::string message) {
  protocol::ResultEnvelope result;
  result.operation = operation_name(operation);
  result.status = code == "policy_refuted" ? protocol::Status::Refuted
                                           : protocol::Status::Error;
  result.completeness.state = "unknown";
  result.diagnostics.push_back(
      protocol::Diagnostic{.code = std::move(code),
                           .severity = "error",
                           .message = std::move(message)});
  return result;
}

protocol::ResultEnvelope
ApplicationService::execute(const CommandRequest &request,
                            ApplicationContext &context) const {
  const std::optional<Operation> operation = operation_of(request);
  if (!operation) {
    return failure(std::nullopt, "invalid_input",
                   "request contains an unknown action or scope");
  }
  const CommandMetadata *entry = metadata(*operation);
  if (entry == nullptr) {
    return failure(operation, "backend_error", "operation is not registered");
  }
  if (context.cancellation().cancelled()) {
    return failure(operation, "timeout", "operation was cancelled");
  }
  if (context.policy().access == AccessMode::read_only &&
      entry->mutability == Mutability::mutating) {
    return failure(
        operation, "policy_refuted",
        "read-only application context rejects a mutating operation");
  }
  if (!context.permits(entry->required_capabilities) ||
      (entry->mutability == Mutability::mutating &&
       !context.policy().allow_schema_migration &&
       *operation == Operation::workspace_refresh)) {
    return failure(operation, "policy_refuted",
                   "application policy does not grant this capability");
  }

  return std::visit(
      [this, &context](const auto &typed) -> protocol::ResultEnvelope {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, IndexRequest>) {
          return services_.index(typed, context);
        } else if constexpr (std::is_same_v<T, QueryRequest>) {
          return services_.query(typed, context);
        } else if constexpr (std::is_same_v<T, AnalysisRequest>) {
          return services_.analysis(typed, context);
        } else if constexpr (std::is_same_v<T, WorkspaceRequest>) {
          return services_.workspace(typed, context);
        } else if constexpr (std::is_same_v<T, AstInspectionRequest>) {
          return services_.ast(typed, context);
        } else if constexpr (std::is_same_v<T, DiffRequest>) {
          return services_.diff(typed, context);
        } else if constexpr (std::is_same_v<T, IncludeRequest>) {
          return services_.include(typed, context);
        } else if constexpr (std::is_same_v<T, RefactoringRequest>) {
          return services_.refactor(typed, context);
        } else {
          return services_.proof(typed, context);
        }
      },
      request);
}

} // namespace cidx::application
