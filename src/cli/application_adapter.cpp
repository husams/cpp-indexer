#include "cli/application_adapter.hpp"

#include <sys/stat.h>

#include <cerrno>
#include <charconv>
#include <cstring>
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

void ensure_directory(const std::string &path) {
  std::string current;
  for (std::size_t i = 0; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') {
      if (!current.empty() && ::mkdir(current.c_str(), 0777) != 0 &&
          errno != EEXIST) {
        throw CidxError("cache directory: " + current + ": " +
                        std::strerror(errno));
      }
    }
    if (i < path.size()) {
      current += path[i];
    }
  }
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
  std::size_t first_argument = 1;
  if (argv.size() > 1 && argv[1] == "rebuild") {
    request.action = application::IndexAction::rebuild;
    first_argument = 2;
  } else if (argv.size() > 1 && argv[1] == "status") {
    request.action = application::IndexAction::status;
    first_argument = 2;
  } else if (argv.size() > 1 && argv[1] == "explain") {
    request.action = application::IndexAction::explain;
    first_argument = 2;
  } else if (argv.size() > 1 && argv[1] == "update") {
    first_argument = 2;
  }
  for (std::size_t i = first_argument; i < argv.size(); ++i) {
    if (argv[i] == "--no-graph") {
      request.graph = false;
    } else if (argv[i] == "--no-autoderive-labels") {
      request.autoderive_labels = false;
    } else if (argv[i] == "--source") {
      request.source = require_value(argv, i, "--source");
    } else if (argv[i] == "--db") {
      request.index = pathutil::abspath(
          pathutil::expanduser(require_value(argv, i, "--db")));
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

std::optional<application::CommandRequest>
parse_typed_workspace(const std::vector<std::string> &argv) {
  if (argv.size() < 2 || argv.front() != "workspace") {
    return std::nullopt;
  }
  application::WorkspaceRequest request;
  if (argv[1] == "list") {
    request.action = application::WorkspaceAction::list;
  } else if (argv[1] == "show") {
    request.action = application::WorkspaceAction::show;
  } else if (argv[1] == "select") {
    request.action = application::WorkspaceAction::select;
  } else if (argv[1] == "refresh") {
    request.action = application::WorkspaceAction::refresh;
  } else {
    usage_error("unknown workspace action " + argv[1]);
  }
  for (std::size_t i = 2; i < argv.size(); ++i) {
    if (argv[i] == "--workspace") {
      request.workspace = require_value(argv, i, "--workspace");
    } else if (argv[i] == "--repository") {
      request.repository = require_value(argv, i, "--repository");
    } else if (argv[i] == "--revision") {
      request.revision = require_value(argv, i, "--revision");
    } else if (argv[i] == "--db") {
      request.index = pathutil::abspath(
          pathutil::expanduser(require_value(argv, i, "--db")));
    } else {
      usage_error("unrecognized workspace option " + argv[i]);
    }
  }
  return request;
}

std::optional<application::CommandRequest>
parse_typed_ast(const std::vector<std::string> &argv) {
  if (argv.size() < 3 || argv.front() != "ast") {
    return std::nullopt;
  }
  application::AstInspectionRequest request;
  if (argv[1] == "dump") {
    request.action = application::AstInspectionAction::dump;
  } else if (argv[1] == "locals") {
    request.action = application::AstInspectionAction::locals;
  } else if (argv[1] == "conditions") {
    request.action = application::AstInspectionAction::conditions;
  } else {
    usage_error("unknown AST action " + argv[1]);
  }
  request.source = argv[2];
  for (std::size_t i = 3; i < argv.size(); ++i) {
    if (argv[i] == "--json") {
      request.json = true;
    } else if (argv[i] == "--db") {
      request.index = pathutil::abspath(
          pathutil::expanduser(require_value(argv, i, "--db")));
    } else {
      usage_error("unrecognized AST option " + argv[i]);
    }
  }
  return request;
}

std::optional<application::CommandRequest>
parse_typed_diff(const std::vector<std::string> &argv) {
  if (argv.size() < 4 || argv.front() != "diff") {
    return std::nullopt;
  }
  application::DiffRequest request;
  if (argv[1] == "file") {
    request.scope = application::DiffScope::file;
  } else if (argv[1] == "symbol") {
    request.scope = application::DiffScope::symbol;
  } else if (argv[1] == "source") {
    request.scope = application::DiffScope::source;
  } else if (argv[1] == "configuration") {
    request.scope = application::DiffScope::configuration;
  } else if (argv[1] == "index") {
    request.scope = application::DiffScope::index;
  } else {
    usage_error("unknown diff scope " + argv[1]);
  }
  request.left = argv[2];
  request.right = argv[3];
  for (std::size_t i = 4; i < argv.size(); ++i) {
    if (argv[i] == "--json") {
      request.json = true;
    } else if (argv[i] == "--db" || argv[i] == "--left-db") {
      request.left_index = pathutil::abspath(
          pathutil::expanduser(require_value(argv, i, argv[i])));
    } else if (argv[i] == "--right-db") {
      request.right_index = pathutil::abspath(
          pathutil::expanduser(require_value(argv, i, "--right-db")));
    } else if (argv[i] == "--selector" || argv[i] == "--symbol") {
      request.selector = require_value(argv, i, argv[i]);
    } else if (argv[i] == "--left-source-revision") {
      request.left_source_revision = require_value(argv, i, argv[i]);
    } else if (argv[i] == "--right-source-revision") {
      request.right_source_revision = require_value(argv, i, argv[i]);
    } else if (argv[i] == "--left-configuration") {
      request.left_configuration = require_value(argv, i, argv[i]);
    } else if (argv[i] == "--right-configuration") {
      request.right_configuration = require_value(argv, i, argv[i]);
    } else if (argv[i] == "--left-index-identity") {
      request.left_index_identity = require_value(argv, i, argv[i]);
    } else if (argv[i] == "--right-index-identity") {
      request.right_index_identity = require_value(argv, i, argv[i]);
    } else {
      usage_error("unrecognized diff option " + argv[i]);
    }
  }
  return request;
}

std::optional<application::CommandRequest>
parse_typed_include(const std::vector<std::string> &argv) {
  if (argv.size() < 2 || argv.front() != "include") {
    return std::nullopt;
  }
  application::IncludeRequest request;
  if (argv[1] == "graph") {
    request.action = application::IncludeAction::graph;
  } else if (argv[1] == "check") {
    request.action = application::IncludeAction::check;
  } else if (argv[1] == "plan") {
    request.action = application::IncludeAction::plan;
  } else if (argv[1] == "apply") {
    request.action = application::IncludeAction::apply;
  } else {
    usage_error("unknown include action " + argv[1]);
  }
  for (std::size_t i = 2; i < argv.size(); ++i) {
    if (argv[i] == "--db") {
      request.index = pathutil::abspath(
          pathutil::expanduser(require_value(argv, i, "--db")));
    } else if (argv[i] == "--output") {
      request.output = require_value(argv, i, "--output");
    } else if (argv[i] == "--plan") {
      request.plan = require_value(argv, i, "--plan");
    } else if (argv[i] == "--json") {
      request.json = true;
    } else if (argv[i] == "--reverse") {
      request.reverse = true;
    } else if (argv[i] == "--transitive") {
      request.transitive = true;
    } else if (argv[i] == "--cycles") {
      request.cycles = true;
    } else if (argv[i] == "--system") {
      request.include_system = true;
    } else if (argv[i] == "--duplicates") {
      request.duplicates = true;
    } else if (argv[i] == "--unused") {
      request.unused = true;
    } else if (argv[i] == "--dry-run") {
      request.dry_run = true;
    } else if (argv[i] == "--only") {
      request.only.push_back(require_value(argv, i, "--only"));
    } else if (argv[i].starts_with('-')) {
      usage_error("unrecognized include option " + argv[i]);
    } else {
      request.paths.push_back(argv[i]);
    }
  }
  return request;
}

std::optional<application::CommandRequest>
parse_typed_refactor(const std::vector<std::string> &argv) {
  if (argv.size() < 2 || argv.front() != "refactor") {
    return std::nullopt;
  }
  application::RefactoringRequest request;
  if (argv[1] == "check") {
    request.action = application::RefactoringAction::check;
  } else if (argv[1] == "plan") {
    request.action = application::RefactoringAction::plan;
  } else if (argv[1] == "apply") {
    request.action = application::RefactoringAction::apply;
  } else {
    usage_error("unknown refactoring action " + argv[1]);
  }
  for (std::size_t i = 2; i < argv.size(); ++i) {
    if (argv[i] == "--db") {
      request.index = pathutil::abspath(
          pathutil::expanduser(require_value(argv, i, "--db")));
    } else if (argv[i] == "--plan") {
      request.plan = require_value(argv, i, "--plan");
    } else if (argv[i] == "--dry-run") {
      request.dry_run = true;
    } else if (argv[i] == "--only") {
      request.only.push_back(require_value(argv, i, "--only"));
    } else if (argv[i].starts_with('-')) {
      usage_error("unrecognized refactoring option " + argv[i]);
    } else {
      request.paths.push_back(argv[i]);
    }
  }
  return request;
}

std::optional<application::CommandRequest>
parse_typed_proof(const std::vector<std::string> &argv) {
  if (argv.size() < 2 || argv.front() != "proof") {
    return std::nullopt;
  }
  application::ProofRequest request;
  if (argv[1] == "prepare") {
    request.action = application::ProofAction::prepare;
  } else if (argv[1] == "execute") {
    request.action = application::ProofAction::execute;
  } else if (argv[1] == "status") {
    request.action = application::ProofAction::status;
  } else if (argv[1] == "explain") {
    request.action = application::ProofAction::explain;
  } else {
    usage_error("unknown proof action " + argv[1]);
  }
  for (std::size_t i = 2; i < argv.size(); ++i) {
    if (argv[i] == "--target") {
      request.target = require_value(argv, i, "--target");
    } else if (argv[i] == "--policy") {
      request.policy = require_value(argv, i, "--policy");
    } else if (argv[i] == "--budget") {
      request.budget = require_value(argv, i, "--budget");
    } else if (argv[i] == "--db") {
      request.index = pathutil::abspath(
          pathutil::expanduser(require_value(argv, i, "--db")));
    } else {
      usage_error("unrecognized proof option " + argv[i]);
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
  if (const auto request = parse_typed_analysis(argv)) {
    return request;
  }
  if (const auto request = parse_typed_workspace(argv)) {
    return request;
  }
  if (const auto request = parse_typed_ast(argv)) {
    return request;
  }
  if (const auto request = parse_typed_diff(argv)) {
    return request;
  }
  if (const auto request = parse_typed_include(argv)) {
    return request;
  }
  if (const auto request = parse_typed_refactor(argv)) {
    return request;
  }
  return parse_typed_proof(argv);
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
  if (const auto request = parse_typed_workspace(argv)) {
    return ApplicationParseResult{.value = *request};
  }
  if (const auto request = parse_typed_ast(argv)) {
    return ApplicationParseResult{.value = *request};
  }
  if (const auto request = parse_typed_diff(argv)) {
    return ApplicationParseResult{.value = *request};
  }
  if (const auto request = parse_typed_include(argv)) {
    return ApplicationParseResult{.value = *request};
  }
  if (const auto request = parse_typed_refactor(argv)) {
    return ApplicationParseResult{.value = *request};
  }
  if (const auto request = parse_typed_proof(argv)) {
    return ApplicationParseResult{.value = *request};
  }
  return ApplicationParseResult{.value = CompatibilityRequest{.argv = argv}};
}

int run_application_request(const application::CommandRequest &request,
                            Context &ctx) {
  std::visit(
      [&ctx](const auto &typed) {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (requires { typed.index; }) {
          if (typed.index) {
            ctx.index_path = *typed.index;
          }
        } else if constexpr (std::is_same_v<T, application::DiffRequest>) {
          if (typed.left_index) {
            ctx.index_path = *typed.left_index;
          }
        }
      },
      request);

  const auto operation = application::operation_of(request);
  const application::CommandMetadata *entry =
      operation ? application::metadata(*operation) : nullptr;
  if (entry == nullptr) {
    return 2;
  }
  const bool read_only =
      entry->mutability == application::Mutability::read_only;
  const application::ApplicationPolicy policy{
      .access = read_only ? application::AccessMode::read_only
                          : application::AccessMode::read_write,
      .capabilities = entry->required_capabilities,
      .allow_schema_migration = false,
  };

  if (!entry->requires_index) {
    application::ApplicationContext application_context(policy);
    const application::DefaultApplicationServices services;
    const application::ApplicationService service(services);
    const protocol::ResultEnvelope result =
        service.execute(request, application_context);
    if (ctx.out != nullptr) {
      *ctx.out << json_out::dumps_indent2(result.result) << "\n";
    }
    return result.exit_code();
  }

  const std::string default_index = pathutil::join(ctx.cache_dir, "index.db");
  if (!read_only && ctx.index_path == default_index) {
    ensure_directory(ctx.cache_dir);
    Logger::root().set_file(pathutil::join(ctx.cache_dir, "cidx.log"));
    ctx.logger = &Logger::root();
  }

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
  application::StorageApplicationOperations operations(db, ctx.index_path);
  application::ApplicationOperationPorts operation_ports{
      .index = &operations,
      .analysis = &operations,
      .ast = &operations,
      .diff = &operations,
      .include = &operations,
  };
  application::ApplicationContext application_context(
      workspace, policy, read_ports, write_ports, operation_ports, ctx.logger);
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
