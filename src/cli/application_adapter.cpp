#include "cli/application_adapter.hpp"

#include <charconv>
#include <string>
#include <type_traits>

#include "application/services.hpp"
#include "query/exec.hpp"
#include "storage/storage.hpp"
#include "util/errors.hpp"
#include "util/pathutil.hpp"

namespace cidx::cli {
namespace {

[[noreturn]] void usage_error(const std::string &message) {
  throw UsageError("cidx: error: " + message + "\n", 2);
}

std::string require_value(const std::vector<std::string> &argv,
                          std::size_t &index, const std::string &option) {
  if (index + 1 >= argv.size()) {
    usage_error(option + " requires a value");
  }
  return argv[++index];
}

std::optional<application::CommandRequest>
parse_typed_query(const std::vector<std::string> &argv) {
  if (argv.empty() || argv.front() != "query") {
    return std::nullopt;
  }
  if (argv.size() == 1 || argv[1] == "-h" || argv[1] == "--help" ||
      argv[1] == "--version") {
    return std::nullopt;
  }
  application::QueryRequest request;
  request.expression = argv[1];
  for (std::size_t i = 2; i < argv.size(); ++i) {
    if (argv[i] == "--json") {
      request.output = application::QueryOutput::json;
    } else if (argv[i] == "--explain") {
      request.explain = true;
    } else if (argv[i] == "--db") {
      request.index = pathutil::abspath(
          pathutil::expanduser(require_value(argv, i, "--db")));
    } else {
      usage_error("unrecognized query option " + argv[i]);
    }
  }
  return request;
}

std::optional<application::CommandRequest>
parse_typed_index(const std::vector<std::string> &argv) {
  if (argv.empty() || argv.front() != "index") {
    return std::nullopt;
  }
  if (argv.size() == 1 || argv[1] == "-h" || argv[1] == "--help" ||
      argv[1] == "--version") {
    return std::nullopt;
  }
  application::IndexRequest request;
  for (std::size_t i = 1; i < argv.size(); ++i) {
    if (argv[i] == "--no-graph") {
      request.graph = false;
    } else if (argv[i] == "--no-autoderive-labels") {
      request.autoderive_labels = false;
    } else if (argv[i] == "--source") {
      request.source = require_value(argv, i, "--source");
    } else if (argv[i].starts_with('-')) {
      usage_error("unrecognized index option " + argv[i]);
    } else {
      request.files.push_back(argv[i]);
    }
  }
  return request;
}

std::optional<application::CommandRequest>
parse_typed_analysis(const std::vector<std::string> &argv) {
  if (argv.empty() || argv.front() != "analyze") {
    return std::nullopt;
  }
  if (argv.size() == 1 || argv[1] == "-h" || argv[1] == "--help" ||
      argv[1] == "--version") {
    return std::nullopt;
  }
  application::AnalysisRequest request;
  for (std::size_t i = 1; i < argv.size(); ++i) {
    if (argv[i] == "--list") {
      request.action = application::AnalysisAction::list;
    } else if (argv[i] == "--rule") {
      request.action = application::AnalysisAction::execute;
      request.rule = require_value(argv, i, "--rule");
    } else if (argv[i] == "--rules-file") {
      request.action = application::AnalysisAction::execute;
      request.rules_file = require_value(argv, i, "--rules-file");
    } else if (argv[i] == "--export-facts") {
      request.action = application::AnalysisAction::export_facts;
      request.export_directory = require_value(argv, i, "--export-facts");
    } else if (argv[i] == "--db") {
      request.index = pathutil::abspath(
          pathutil::expanduser(require_value(argv, i, "--db")));
    } else if (argv[i] == "--jobs") {
      const std::string value = require_value(argv, i, "--jobs");
      int jobs = 0;
      const auto [end, error] =
          std::from_chars(value.data(), value.data() + value.size(), jobs);
      if (error != std::errc{} || end != value.data() + value.size() ||
          jobs < 1) {
        usage_error("--jobs must be a positive integer");
      }
      request.jobs = jobs;
    } else {
      usage_error("unrecognized analysis option " + argv[i]);
    }
  }
  return request;
}

} // namespace

std::optional<application::CommandRequest>
parse_request(const std::vector<std::string> &argv) {
  if (const auto request = parse_typed_query(argv)) {
    return request;
  }
  if (const auto request = parse_typed_index(argv)) {
    return request;
  }
  return parse_typed_analysis(argv);
}

ApplicationParseResult
parse_application_request(const std::vector<std::string> &argv) {
  if (const auto request = parse_typed_query(argv)) {
    return ApplicationParseResult{.value = *request};
  }
  if (const auto request = parse_typed_index(argv)) {
    return ApplicationParseResult{.value = *request};
  }
  if (const auto request = parse_typed_analysis(argv)) {
    return ApplicationParseResult{.value = *request};
  }
  return ApplicationParseResult{.value = CompatibilityRequest{.argv = argv}};
}

int run_application_request(const application::CommandRequest &request,
                            Context &ctx) {
  if (const auto *analysis =
          std::get_if<application::AnalysisRequest>(&request);
      analysis != nullptr &&
      analysis->action == application::AnalysisAction::list) {
    application::ApplicationContext application_context;
    const application::DefaultApplicationServices services;
    const application::ApplicationService service(services);
    const protocol::ResultEnvelope result =
        service.execute(request, application_context);
    if (ctx.out != nullptr) {
      *ctx.out << json_out::dumps_indent2(result.result) << "\n";
    }
    return result.exit_code();
  }
  const auto operation = application::operation_of(request);
  const bool read_only = operation &&
                         application::metadata(*operation) != nullptr &&
                         application::metadata(*operation)->mutability ==
                             application::Mutability::read_only;
  Storage db(ctx.index_path, read_only ? Storage::OpenMode::read_only
                                       : Storage::OpenMode::read_write);
  StorageWorkspaceAdapter workspace_data(db);
  WorkspaceContext workspace =
      WorkspaceContext::borrow(workspace_data,
                               read_only ? WorkspaceReadWriteMode::read_only
                                         : WorkspaceReadWriteMode::read_write,
                               ctx.index_path);
  query::SqliteQueryReadAdapter query_read(db);

  application::ApplicationReadPorts read_ports{
      .workspace = &db.workspace_catalog_read(),
      .source = &db.source_read(),
      .symbols = &db.symbol_read(),
      .types = &db.type_read(),
      .facts = &db.fact_read(),
      .definitions = &db.definition_read(),
      .includes = &db.include_read(),
      .schema = &db.schema_read(),
      .query = &query_read,
  };
  application::ApplicationWritePorts write_ports;
  if (!read_only) {
    write_ports = application::ApplicationWritePorts{
        .workspace = &db.workspace_catalog_write(),
        .source = &db.source_write(),
        .symbols = &db.symbol_write(),
        .types = &db.type_write(),
        .facts = &db.fact_write(),
        .definitions = &db.definition_write(),
        .includes = &db.include_write(),
        .unit_of_work = &db.unit_of_work(),
    };
  }
  application::ApplicationPolicy policy{
      .access = read_only ? application::AccessMode::read_only
                          : application::AccessMode::read_write,
      .capabilities = read_only ? application::capability_bit(
                                      application::Capability::index_read)
                                : application::all_capabilities(),
      .allow_schema_migration = false,
  };
  application::ApplicationContext application_context(
      workspace, policy, read_ports, write_ports, ctx.logger, &db);
  const application::DefaultApplicationServices services;
  const application::ApplicationService service(services);
  const protocol::ResultEnvelope result =
      service.execute(request, application_context);
  if (result.status == protocol::Status::Error ||
      result.status == protocol::Status::Refuted) {
    if (ctx.err != nullptr && !result.diagnostics.empty()) {
      *ctx.err << "error: " << result.diagnostics.front().message << "\n";
    }
    return result.exit_code();
  }
  if (ctx.out != nullptr) {
    *ctx.out << json_out::dumps_indent2(result.result) << "\n";
  }
  return result.exit_code();
}

} // namespace cidx::cli
