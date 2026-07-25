#include "cli/application_adapter.hpp"

#include <string>
#include <type_traits>
#include <utility>

#include "application/services.hpp"

namespace cidx::cli {
namespace {

protocol::ResultEnvelope result_for_exit(const std::string &operation, int rc) {
  protocol::ResultEnvelope result;
  result.operation = operation;
  if (rc == 0) {
    return result;
  }
  result.status = protocol::Status::Error;
  result.completeness.state = "unknown";
  result.diagnostics.push_back(
      protocol::Diagnostic{.code = rc == 2 ? "usage" : "backend_error",
                           .severity = "error",
                           .message = "legacy CLI adapter returned exit code " +
                                      std::to_string(rc)});
  return result;
}

ParsedArgs legacy_args(const application::CommandRequest &request) {
  ParsedArgs args;
  std::visit(
      [&args](const auto &typed) {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, application::IndexRequest>) {
          args.command = "index";
          args.files = typed.files;
          args.source = typed.source;
          args.no_graph = !typed.graph;
          args.no_autoderive_labels = !typed.autoderive_labels;
        } else if constexpr (std::is_same_v<T, application::QueryRequest>) {
          args.command = "query";
          args.query_text = typed.expression;
          args.query_json = typed.output == application::QueryOutput::json;
          args.query_explain = typed.explain;
          args.index_db = typed.index;
        } else if constexpr (std::is_same_v<T, application::AnalysisRequest>) {
          args.command = "analyze";
          args.analyze_rule = typed.rule;
          args.analyze_rules_file = typed.rules_file;
          args.analyze_export = typed.export_directory;
          args.index_db = typed.index;
          args.analyze_jobs = typed.jobs;
          args.analyze_list = typed.action == application::AnalysisAction::list;
        }
      },
      request);
  return args;
}

} // namespace

std::optional<application::CommandRequest>
try_build_application_request(const ParsedArgs &args) {
  if (args.command == "index") {
    return application::IndexRequest{
        .action = application::IndexAction::update,
        .files = args.files,
        .source = args.source,
        .graph = !args.no_graph,
        .autoderive_labels = !args.no_autoderive_labels,
    };
  }
  if (args.command == "query") {
    return application::QueryRequest{
        .expression = args.query_text,
        .output = args.query_json ? application::QueryOutput::json
                                  : application::QueryOutput::human,
        .explain = args.query_explain,
        .index = args.index_db,
    };
  }
  if (args.command == "analyze") {
    const application::AnalysisAction action =
        args.analyze_list     ? application::AnalysisAction::list
        : args.analyze_export ? application::AnalysisAction::export_facts
                              : application::AnalysisAction::execute;
    return application::AnalysisRequest{
        .action = action,
        .rule = args.analyze_rule,
        .rules_file = args.analyze_rules_file,
        .export_directory = args.analyze_export,
        .index = args.index_db,
        .jobs = args.analyze_jobs,
    };
  }
  return std::nullopt;
}

std::optional<application::CommandRequest>
parse_request(const std::vector<std::string> &argv) {
  return try_build_application_request(parse_args(argv));
}

int run_application_request(const application::CommandRequest &request,
                            Context &ctx) {
  application::ApplicationHandlers handlers;
  handlers.index = [&ctx](const application::IndexRequest &request,
                          application::ApplicationContext &) {
    const ParsedArgs args = legacy_args(request);
    return result_for_exit("index", cmd_index(args, ctx));
  };
  handlers.query = [&ctx](const application::QueryRequest &request,
                          application::ApplicationContext &) {
    const ParsedArgs args = legacy_args(request);
    return result_for_exit("query", cmd_query(args, ctx));
  };
  handlers.analysis = [&ctx](const application::AnalysisRequest &request,
                             application::ApplicationContext &) {
    const ParsedArgs args = legacy_args(request);
    return result_for_exit("analysis", cmd_analyze(args, ctx));
  };

  application::ApplicationContext application_context;
  application::ApplicationService service(std::move(handlers));
  const protocol::ResultEnvelope result =
      service.execute(request, application_context);
  if (result.status == protocol::Status::Error ||
      result.status == protocol::Status::Refuted) {
    return !result.diagnostics.empty() &&
                   result.diagnostics.front().code == "usage"
               ? 2
               : 1;
  }
  return result.exit_code();
}

} // namespace cidx::cli
