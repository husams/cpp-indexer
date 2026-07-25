#include "application/services.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "application/analysis_service.hpp"
#include "ast/clang_version.hpp"
#include "ast/index_engine.hpp"
#include "astgraph/astgraph.hpp"
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

std::string operation_name(const std::optional<Operation> &operation) {
  if (!operation) {
    return "application";
  }
  if (const CommandMetadata *entry = metadata(*operation); entry != nullptr) {
    return std::string(entry->group) + "." + std::string(entry->name);
  }
  return "application";
}

bool belongs_to_root(const std::string &path, const std::string &root) {
  return path == root || (path.starts_with(root) && path.size() > root.size() &&
                          path[root.size()] == '/');
}

std::vector<std::string> split_configuration(std::string_view value) {
  std::istringstream input{std::string(value)};
  std::vector<std::string> tokens;
  std::string token;
  while (input >> token) {
    tokens.push_back(std::move(token));
  }
  return tokens;
}

void apply_configuration_override(diff::ParseConfig &config,
                                  const std::optional<std::string> &override) {
  if (!override) {
    return;
  }
  config.args = split_configuration(*override);
  config.classes = diff::classify_options(config.args);
  config.config_hash = *override;
}

void log_parse_failure(Logger *logger, const std::string &path,
                       const ast::IndexOneOutcome &outcome) {
  if (logger == nullptr || logger->file_path().empty() ||
      outcome.failed_flags.empty()) {
    return;
  }
  std::string flags;
  for (std::size_t index = 0; index < outcome.failed_flags.size(); ++index) {
    if (index != 0) {
      flags += ' ';
    }
    flags += outcome.failed_flags[index];
  }
  logger->error("cidx.clang",
                path + ": failed parse flags: " + flags +
                    "; clang: " + std::to_string(clang_version_major()));
  std::size_t shown = 0;
  for (const Diagnostic &diagnostic : outcome.diagnostics) {
    if (diagnostic.severity < 3 || shown >= 25) {
      continue;
    }
    ++shown;
    logger->info("cidx.clang", path + ": diag " +
                                   diagnostic.file_path.value_or("") + ":" +
                                   std::to_string(diagnostic.line.value_or(0)) +
                                   ": " + diagnostic.spelling);
  }
}

} // namespace

protocol::ResultEnvelope
StorageApplicationOperations::execute(const IndexRequest &request,
                                      ApplicationContext &context) {
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
  Storage &db = storage_;
  std::optional<std::string> source_root;
  if (request.source) {
    const std::optional<Component> component =
        db.get_component_by_name(*request.source);
    if (!component) {
      return service_error("index", context, "unknown_source",
                           "no component named " + *request.source);
    }
    source_root = db.component_abs_base(*component);
  }
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
  std::int64_t already = 0;
  std::int64_t deferred = 0;
  json_out::Array file_records;
  const bool rebuild = request.action == IndexAction::rebuild;
  if (rebuild) {
    for (const auto &[file, path] : db.list_files()) {
      if (source_root && !belongs_to_root(path, *source_root)) {
        continue;
      }
      db.set_file_indexed(file.id, false);
      if (!files::is_header(path)) {
        targets.emplace_back(file, path);
      }
    }
  } else if (request.files.empty()) {
    for (const auto &[file, path] : db.list_files()) {
      if (source_root && !belongs_to_root(path, *source_root)) {
        continue;
      }
      if (files::index_status(file, path) == files::IndexStatus::kOk) {
        ++already;
        file_records.push_back(json_out::Value::obj(
            {{"path", json_out::Value::of(path)},
             {"status", json_out::Value::of(std::string("already"))}}));
      } else if (!files::is_header(path)) {
        targets.emplace_back(file, path);
      } else {
        ++deferred;
        file_records.push_back(json_out::Value::obj(
            {{"path", json_out::Value::of(path)},
             {"status", json_out::Value::of(std::string("deferred"))}}));
      }
    }
  }

  std::int64_t indexed = 0;
  std::int64_t failed = 0;
  std::int64_t unknown = 0;
  std::int64_t warnings = 0;
  std::int64_t errors = 0;
  std::vector<protocol::Diagnostic> diagnostics;

  if (!request.files.empty() && !rebuild) {
    for (const std::string &file_arg : request.files) {
      const std::string path = files::resolve_file_arg(file_arg, source_root);
      const std::optional<File> file = db.get_file(path);
      if (!file) {
        ++unknown;
        file_records.push_back(json_out::Value::obj(
            {{"path", json_out::Value::of(path)},
             {"status", json_out::Value::of(std::string("unknown"))}}));
        if (diagnostics.size() < context.policy().max_diagnostics) {
          diagnostics.push_back(protocol::Diagnostic{
              .code = "invalid_input",
              .severity = "error",
              .message = "not in index database: " + path});
        }
        continue;
      }
      if (files::index_status(*file, path) == files::IndexStatus::kOk) {
        ++already;
        file_records.push_back(json_out::Value::obj(
            {{"path", json_out::Value::of(path)},
             {"status", json_out::Value::of(std::string("already"))}}));
        continue;
      }
      targets.emplace_back(*file, path);
    }
  }

  bool truncated = false;
  if (context.policy().max_work_items != 0 &&
      targets.size() > context.policy().max_work_items) {
    targets.resize(context.policy().max_work_items);
    truncated = true;
  }

  for (std::size_t position = 0; position < targets.size(); ++position) {
    const auto &[file, path] = targets[position];
    if (context.cancellation().cancelled()) {
      break;
    }
    context.publish(
        protocol::ProgressEvent{.sequence = static_cast<int64_t>(position),
                                .operation = "index",
                                .message = path,
                                .completed = static_cast<int64_t>(position),
                                .total = static_cast<int64_t>(targets.size())});
    if (context.cancellation().cancelled()) {
      break;
    }
    const ast::IndexOneOutcome outcome =
        ast::run_index_one(db, file, path, request.graph);
    std::vector<Diagnostic> persisted_diagnostics = outcome.diagnostics;
    if ((outcome.parse_failed || outcome.source_changed) &&
        persisted_diagnostics.empty()) {
      persisted_diagnostics.push_back(Diagnostic{
          .severity = 4,
          .spelling = outcome.error.empty() ? "indexing failed for " + path
                                            : outcome.error,
          .file_path = path});
    }
    db.replace_diagnostics(file.id, persisted_diagnostics);
    for (const Diagnostic &diagnostic : persisted_diagnostics) {
      if (diagnostic.severity >= 3) {
        ++errors;
      } else if (diagnostic.severity == 2) {
        ++warnings;
      }
    }
    if (outcome.parse_failed) {
      log_parse_failure(context.logger(), path, outcome);
    }
    if (outcome.parse_failed || outcome.source_changed) {
      db.set_file_indexed(file.id, false);
      ++failed;
      if (diagnostics.size() < context.policy().max_diagnostics) {
        diagnostics.push_back(protocol::Diagnostic{
            .code = "backend_error",
            .severity = "error",
            .message = outcome.error.empty() ? "indexing failed for " + path
                                             : outcome.error});
      }
      file_records.push_back(json_out::Value::obj(
          {{"path", json_out::Value::of(path)},
           {"status", json_out::Value::of(std::string("failed"))},
           {"error", json_out::Value::of(outcome.error.empty()
                                             ? "indexing failed for " + path
                                             : outcome.error)}}));
    } else {
      db.mark_file_indexed(file.id, std::nullopt, outcome.source_md5);
      ++indexed;
      file_records.push_back(json_out::Value::obj(
          {{"path", json_out::Value::of(path)},
           {"status", json_out::Value::of(std::string("indexed"))},
           {"stored", json_out::Value::of(outcome.stored)},
           {"headers_indexed", json_out::Value::of(outcome.headers.indexed)},
           {"headers_symbols", json_out::Value::of(outcome.headers.symbols)},
           {"headers_already", json_out::Value::of(outcome.headers.already)},
           {"headers_system", json_out::Value::of(outcome.headers.system)},
           {"headers_unowned", json_out::Value::of(outcome.headers.unowned)}}));
    }
  }
  protocol::ResultEnvelope result = service_result("index", context, "index");
  const std::int64_t skipped =
      static_cast<std::int64_t>(targets.size()) - indexed - failed + already;
  const bool cancelled = context.cancellation().cancelled();
  const bool all_current =
      std::ranges::all_of(db.list_files(), [](const auto &entry) {
        return files::index_status(entry.first, entry.second) ==
               files::IndexStatus::kOk;
      });
  const bool incomplete =
      truncated || cancelled || failed != 0 || unknown != 0 || !all_current;
  result.result = json_out::Value::obj(
      {{"indexed", json_out::Value::of(indexed)},
       {"failed", json_out::Value::of(failed)},
       {"skipped", json_out::Value::of(skipped)},
       {"already", json_out::Value::of(already)},
       {"unknown", json_out::Value::of(unknown)},
       {"deferred", json_out::Value::of(deferred)},
       {"warnings", json_out::Value::of(warnings)},
       {"errors", json_out::Value::of(errors)},
       {"files", json_out::Value::arr(std::move(file_records))}});
  if (cancelled) {
    result.status = protocol::Status::Error;
    result.completeness.state = "unknown";
    result.diagnostics.push_back(
        protocol::Diagnostic{.code = "timeout",
                             .severity = "error",
                             .message = "operation was cancelled"});
  } else if (failed != 0 || unknown != 0) {
    result.status = protocol::Status::Error;
    result.completeness.state = "unknown";
    result.diagnostics = std::move(diagnostics);
    if (result.diagnostics.empty()) {
      result.diagnostics.push_back(protocol::Diagnostic{
          .code = failed != 0 ? "backend_error" : "invalid_input",
          .severity = "error",
          .message = "index operation failed"});
    }
  } else if (incomplete) {
    result.status = protocol::Status::Partial;
    result.completeness.state = "partial";
    if (truncated) {
      result.diagnostics.push_back(
          protocol::Diagnostic{.code = "truncated_budget",
                               .severity = "warning",
                               .message = "index work budget reached before "
                                          "all pending files were processed"});
    }
  }
  result.completeness.truncated = truncated;
  if (truncated) {
    result.completeness.budget =
        static_cast<int64_t>(context.policy().max_work_items);
  }
  if (!incomplete) {
    db.stamp_index_identity();
  }
  const IndexIdentity identity = db.index_identity();
  result.identity.index = identity.freshness;
  if (result.status == protocol::Status::Error) {
    result.identity.freshness = "unverifiable";
  } else if (identity.freshness == "stale" &&
             result.status == protocol::Status::Complete) {
    result.status = protocol::Status::Unknown;
    result.completeness.state = "unknown";
    result.completeness.stale = true;
    result.completeness.truncated = false;
    result.identity.freshness = "stale";
    result.diagnostics.clear();
    result.diagnostics.push_back(protocol::Diagnostic{
        .code = "stale_input",
        .severity = "error",
        .message = "pending or changed files remain in the selected index"});
  } else if (identity.freshness == "stale") {
    // Preserve partial work and its evidence when a pending row keeps the
    // selected index stale; the stale identity is exposed in identity.index.
    result.identity.freshness = "unverifiable";
  } else {
    result.identity.freshness = identity.freshness;
  }
  return result;
}

protocol::ResultEnvelope
StorageApplicationOperations::execute(const AnalysisRequest &request,
                                      ApplicationContext &context) {
  try {
    protocol::ResultEnvelope result =
        service_result("analysis", context, "souffle-analysis");
    const std::string path =
        index_path_.empty() && context.workspace() != nullptr
            ? context.workspace()->index_path()
            : index_path_;
    result.result = cli::run_analysis_application(request, path).result;
    return result;
  } catch (const std::exception &error) {
    return service_error("analysis", context, "backend_error", error.what());
  }
}

protocol::ResultEnvelope
StorageApplicationOperations::execute(const AstInspectionRequest &request,
                                      ApplicationContext &context) {
  if (request.action != AstInspectionAction::dump) {
    return service_error(
        "ast", context, "invalid_input",
        "AST locals and conditions inspection is not implemented yet");
  }
  const std::optional<File> file =
      storage_.get_file(files::resolve_file_arg(request.source));
  if (!file) {
    return service_error("ast", context, "unknown_source",
                         "source is not registered in the selected index");
  }
  if (!file->compile_options) {
    return service_error("ast", context, "backend_error",
                         "source has no translation-unit configuration");
  }

  std::error_code ec;
  const std::filesystem::path temp_dir =
      std::filesystem::temp_directory_path(ec) / "cidx-application-ast";
  if (ec) {
    return service_error("ast", context, "backend_error",
                         "cannot resolve temporary AST directory");
  }
  std::filesystem::create_directories(temp_dir, ec);
  if (ec) {
    return service_error("ast", context, "backend_error",
                         "cannot create temporary AST directory");
  }
  const std::filesystem::path output =
      temp_dir / (std::to_string(file->id) + ".db");
  try {
    const astgraph::DumpStats stats = astgraph::dump_tu(
        files::resolve_file_arg(request.source), *file->compile_options,
        file->driver, output.string(), astgraph::Options{});
    std::filesystem::remove(output, ec);
    std::filesystem::remove(temp_dir, ec);
    protocol::ResultEnvelope result =
        service_result("ast", context, "astgraph");
    result.result = json_out::Value::obj(
        {{"action", json_out::Value::of(static_cast<int>(request.action))},
         {"source", json_out::Value::of(request.source)},
         {"cursor_nodes", json_out::Value::of(stats.cursor_nodes)},
         {"type_nodes", json_out::Value::of(stats.type_nodes)},
         {"edges", json_out::Value::of(stats.edges)},
         {"symbols", json_out::Value::of(stats.symbols)},
         {"files", json_out::Value::of(stats.files)}});
    return result;
  } catch (const std::exception &error) {
    std::filesystem::remove(output, ec);
    std::filesystem::remove(temp_dir, ec);
    return service_error("ast", context, "backend_error", error.what());
  }
}

protocol::ResultEnvelope
StorageApplicationOperations::execute(const DiffRequest &request,
                                      ApplicationContext &context) {
  try {
    if (request.scope == DiffScope::index) {
      const std::string left =
          request.left_index_identity.value_or(request.left);
      const std::string right =
          request.right_index_identity.value_or(request.right);
      protocol::ResultEnvelope result =
          service_result("diff.index", context, "diff");
      result.result =
          json_out::Value::obj({{"left", json_out::Value::of(left)},
                                {"right", json_out::Value::of(right)},
                                {"equal", json_out::Value::of(left == right)}});
      return result;
    }
    if (request.scope == DiffScope::source) {
      const std::string left =
          request.left_source_revision.value_or(request.left);
      const std::string right =
          request.right_source_revision.value_or(request.right);
      protocol::ResultEnvelope result =
          service_result("diff.source", context, "diff");
      result.result =
          json_out::Value::obj({{"left", json_out::Value::of(left)},
                                {"right", json_out::Value::of(right)},
                                {"equal", json_out::Value::of(left == right)}});
      return result;
    }

    const std::string left_db = request.left_index.value_or("");
    if (left_db.empty()) {
      return service_error("diff", context, "invalid_input",
                           "file and symbol diffs require an index path");
    }
    diff::ParseConfig left_config =
        diff::resolve_parse_config(diff::SideSpec{.side = "left",
                                                  .file = request.left,
                                                  .db = left_db,
                                                  .tu = std::nullopt});
    diff::ParseConfig right_config = diff::resolve_parse_config(
        diff::SideSpec{.side = "right",
                       .file = request.right,
                       .db = request.right_index.value_or(left_db),
                       .tu = std::nullopt});
    apply_configuration_override(left_config, request.left_configuration);
    apply_configuration_override(right_config, request.right_configuration);
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
StorageApplicationOperations::execute(const IncludeRequest &request,
                                      ApplicationContext &context) {
  hygiene::AnalysisOptions options;
  options.scope_paths = request.paths;
  options.want_duplicates = request.duplicates || !request.unused;
  options.want_unused = request.unused || !request.duplicates;
  const hygiene::AnalysisResult analysis = hygiene::analyze(storage_, options);
  protocol::ResultEnvelope result =
      service_result("include", context, "include-hygiene");
  result.result = json_out::Value::obj(
      {{"candidates", json_out::Value::of(static_cast<std::int64_t>(
                          analysis.candidates.size()))},
       {"uncovered", json_out::Value::of(static_cast<std::int64_t>(
                         analysis.uncovered_scope.size()))}});
  if (request.action == IncludeAction::plan) {
    if (context.workspace() == nullptr) {
      return service_error("include.plan", context, "invalid_input",
                           "include plans require a workspace");
    }
    const hygiene::CleanupPlan plan = hygiene::build_plan(
        storage_, analysis, context.workspace()->index_path());
    result.result = json_out::Value::obj(
        {{"candidates", json_out::Value::of(static_cast<std::int64_t>(
                            analysis.candidates.size()))},
         {"uncovered", json_out::Value::of(static_cast<std::int64_t>(
                           analysis.uncovered_scope.size()))},
         {"plan", json_out::Value::of(hygiene::serialize(plan))}});
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
        hygiene::execute(storage_, plan,
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
DefaultApplicationServices::index(const IndexRequest &request,
                                  ApplicationContext &context) const {
  return context.operations().index == nullptr
             ? service_error("index", context, "backend_error",
                             "index operation is not installed")
             : context.operations().index->execute(request, context);
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
    try {
      protocol::ResultEnvelope result =
          service_result("analysis.list", context, "souffle-analysis");
      result.result = cli::run_analysis_application(request, {}).result;
      return result;
    } catch (const std::exception &error) {
      return service_error("analysis.list", context, "backend_error",
                           error.what());
    }
  }
  return context.operations().analysis == nullptr
             ? service_error("analysis", context, "backend_error",
                             "analysis operation is not installed")
             : context.operations().analysis->execute(request, context);
}

protocol::ResultEnvelope
DefaultApplicationServices::workspace(const WorkspaceRequest &request,
                                      ApplicationContext &context) const {
  (void)request;
  return service_error("workspace", context, "invalid_input",
                       "workspace operation is not implemented");
}

protocol::ResultEnvelope
DefaultApplicationServices::ast(const AstInspectionRequest &request,
                                ApplicationContext &context) const {
  return context.operations().ast == nullptr
             ? service_error("ast", context, "backend_error",
                             "AST operation is not installed")
             : context.operations().ast->execute(request, context);
}

protocol::ResultEnvelope
DefaultApplicationServices::diff(const DiffRequest &request,
                                 ApplicationContext &context) const {
  return context.operations().diff == nullptr
             ? service_error("diff", context, "backend_error",
                             "diff operation is not installed")
             : context.operations().diff->execute(request, context);
}

protocol::ResultEnvelope
DefaultApplicationServices::include(const IncludeRequest &request,
                                    ApplicationContext &context) const {
  return context.operations().include == nullptr
             ? service_error("include", context, "backend_error",
                             "include operation is not installed")
             : context.operations().include->execute(request, context);
}

protocol::ResultEnvelope
DefaultApplicationServices::refactor(const RefactoringRequest &request,
                                     ApplicationContext &context) const {
  (void)request;
  return service_error("refactor", context, "invalid_input",
                       "refactoring operation is not implemented");
}

protocol::ResultEnvelope
DefaultApplicationServices::proof(const ProofRequest &request,
                                  ApplicationContext &context) const {
  (void)request;
  return service_error("proof", context, "invalid_input",
                       "proof operation is not implemented");
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
  if (const auto *include = std::get_if<IncludeRequest>(&request);
      include != nullptr && include->action == IncludeAction::plan &&
      include->output &&
      !context.permits(capability_bit(Capability::artifacts))) {
    return failure(operation, "policy_refuted",
                   "include plan output requires the artifacts capability");
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
