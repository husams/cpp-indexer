#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "application/registry.hpp"
#include "application/services.hpp"
#include "cli/application_adapter.hpp"
#include "query/exec.hpp"
#include "storage/storage.hpp"

namespace {

cidx::protocol::ResultEnvelope complete(std::string operation) {
  cidx::protocol::ResultEnvelope result;
  result.operation = std::move(operation);
  result.identity.workspace = "test";
  result.identity.index = "test";
  result.identity.fact_sets = {"application"};
  result.identity.freshness = "current";
  result.producer.backend = "test";
  return result;
}

struct RecordingServices final : cidx::application::ApplicationServices {
  cidx::protocol::ResultEnvelope
  index(const cidx::application::IndexRequest &,
        cidx::application::ApplicationContext &) const override {
    calls.push_back("index");
    return complete("index");
  }
  cidx::protocol::ResultEnvelope
  query(const cidx::application::QueryRequest &,
        cidx::application::ApplicationContext &) const override {
    calls.push_back("query");
    return complete("query");
  }
  cidx::protocol::ResultEnvelope
  analysis(const cidx::application::AnalysisRequest &,
           cidx::application::ApplicationContext &) const override {
    calls.push_back("analysis");
    return complete("analysis");
  }
  cidx::protocol::ResultEnvelope
  workspace(const cidx::application::WorkspaceRequest &,
            cidx::application::ApplicationContext &) const override {
    calls.push_back("workspace");
    return complete("workspace");
  }
  cidx::protocol::ResultEnvelope
  ast(const cidx::application::AstInspectionRequest &,
      cidx::application::ApplicationContext &) const override {
    calls.push_back("ast");
    return complete("ast");
  }
  cidx::protocol::ResultEnvelope
  diff(const cidx::application::DiffRequest &,
       cidx::application::ApplicationContext &) const override {
    calls.push_back("diff");
    return complete("diff");
  }
  cidx::protocol::ResultEnvelope
  include(const cidx::application::IncludeRequest &,
          cidx::application::ApplicationContext &) const override {
    calls.push_back("include");
    return complete("include");
  }
  cidx::protocol::ResultEnvelope
  refactor(const cidx::application::RefactoringRequest &,
           cidx::application::ApplicationContext &) const override {
    calls.push_back("refactor");
    return complete("refactor");
  }
  cidx::protocol::ResultEnvelope
  proof(const cidx::application::ProofRequest &,
        cidx::application::ApplicationContext &) const override {
    calls.push_back("proof");
    return complete("proof");
  }

  mutable std::vector<std::string> calls;
};

struct RecordingProgress final : cidx::application::ProgressSink {
  void publish(const cidx::protocol::ProgressEvent &event) override {
    events.push_back(event);
  }

  std::vector<cidx::protocol::ProgressEvent> events;
};

} // namespace

TEST_CASE("application registry is complete and validated") {
  CHECK(cidx::application::registry_is_valid());
  CHECK(cidx::application::command_registry().size() == 31);
  CHECK(cidx::application::metadata(cidx::application::Operation::query) !=
        nullptr);
  CHECK(cidx::application::metadata(cidx::application::Operation::query)
            ->mutability == cidx::application::Mutability::read_only);
}

TEST_CASE(
    "production parser returns typed requests and isolates compatibility") {
  const auto query = cidx::cli::parse_request({"query", "nodes()", "--json"});
  REQUIRE(query.has_value());
  REQUIRE(std::holds_alternative<cidx::application::QueryRequest>(*query));
  const auto &typed = std::get<cidx::application::QueryRequest>(*query);
  CHECK(typed.expression == "nodes()");
  CHECK(typed.output == cidx::application::QueryOutput::json);

  const auto compatibility =
      cidx::cli::parse_application_request({"search", "nodes"});
  REQUIRE(std::holds_alternative<cidx::cli::CompatibilityRequest>(
      compatibility.value));
  CHECK(std::get<cidx::cli::CompatibilityRequest>(compatibility.value).argv ==
        std::vector<std::string>{"search", "nodes"});

  const auto ast = cidx::cli::parse_application_request(
      {"ast", "dump", "fixture.cpp", "--json"});
  REQUIRE(std::holds_alternative<cidx::application::CommandRequest>(ast.value));
  const auto &ast_request =
      std::get<cidx::application::CommandRequest>(ast.value);
  REQUIRE(std::holds_alternative<cidx::application::AstInspectionRequest>(
      ast_request));
  CHECK(std::get<cidx::application::AstInspectionRequest>(ast_request).json);

  const auto diff = cidx::cli::parse_application_request(
      {"diff", "source", "left", "right", "--left-source-revision", "a",
       "--right-source-revision", "b"});
  REQUIRE(
      std::holds_alternative<cidx::application::CommandRequest>(diff.value));
  const auto &diff_request =
      std::get<cidx::application::CommandRequest>(diff.value);
  REQUIRE(std::holds_alternative<cidx::application::DiffRequest>(diff_request));
  CHECK(std::get<cidx::application::DiffRequest>(diff_request)
            .left_source_revision == std::optional<std::string>{"a"});
}

TEST_CASE("typed services dispatch every HSE-68 request family") {
  RecordingServices services;
  cidx::application::ApplicationService service(services);
  cidx::application::ApplicationContext context;
  const std::vector<cidx::application::CommandRequest> requests = {
      cidx::application::IndexRequest{},
      cidx::application::QueryRequest{.expression = "nodes()"},
      cidx::application::AnalysisRequest{},
      cidx::application::WorkspaceRequest{},
      cidx::application::AstInspectionRequest{.source = "fixture.cpp"},
      cidx::application::DiffRequest{.left = "left.cpp", .right = "right.cpp"},
      cidx::application::IncludeRequest{},
      cidx::application::RefactoringRequest{},
      cidx::application::ProofRequest{},
  };
  for (const auto &request : requests) {
    CHECK(service.execute(request, context).status ==
          cidx::protocol::Status::Complete);
  }
  CHECK(services.calls == std::vector<std::string>{
                              "index", "query", "analysis", "workspace", "ast",
                              "diff", "include", "refactor", "proof"});
}

TEST_CASE("unknown actions and scopes fail explicitly") {
  RecordingServices services;
  cidx::application::ApplicationService service(services);
  cidx::application::ApplicationContext context;

  const auto invalid_index = service.execute(
      cidx::application::IndexRequest{
          .action = static_cast<cidx::application::IndexAction>(255)},
      context);
  REQUIRE(invalid_index.status == cidx::protocol::Status::Error);
  CHECK(invalid_index.diagnostics.front().code == "invalid_input");

  const auto invalid_scope = service.execute(
      cidx::application::DiffRequest{
          .scope = static_cast<cidx::application::DiffScope>(255)},
      context);
  REQUIRE(invalid_scope.status == cidx::protocol::Status::Error);
  CHECK(invalid_scope.diagnostics.front().code == "invalid_input");
  CHECK(services.calls.empty());
}

TEST_CASE("capability policy rejects writes before the service") {
  RecordingServices services;
  cidx::application::ApplicationService service(services);
  cidx::application::ApplicationContext context(
      {.access = cidx::application::AccessMode::read_only,
       .capabilities = cidx::application::capability_bit(
           cidx::application::Capability::index_read)});

  const auto result = service.execute(
      cidx::application::IncludeRequest{
          .action = cidx::application::IncludeAction::apply},
      context);
  REQUIRE(result.status == cidx::protocol::Status::Refuted);
  CHECK(result.diagnostics.front().code == "policy_refuted");
  CHECK(services.calls.empty());
}

TEST_CASE("cancellation is enforced at the application boundary") {
  RecordingServices services;
  cidx::application::ApplicationService service(services);
  cidx::application::ApplicationContext context;
  context.cancellation().cancel();
  const auto result = service.execute(
      cidx::application::QueryRequest{.expression = "nodes()"}, context);
  CHECK(result.status == cidx::protocol::Status::Error);
  CHECK(result.diagnostics.front().code == "timeout");
}

TEST_CASE("metadata derives capabilities and policy protects artifacts") {
  const auto *analysis = cidx::application::metadata(
      cidx::application::Operation::analysis_execute);
  REQUIRE(analysis != nullptr);
  CHECK(analysis->required_capabilities ==
        cidx::application::capability_bit(
            cidx::application::Capability::analysis));

  RecordingServices services;
  cidx::application::ApplicationService service(services);
  cidx::application::ApplicationContext context(
      {.access = cidx::application::AccessMode::read_only,
       .capabilities = cidx::application::capability_bit(
           cidx::application::Capability::include_read)});
  const auto result = service.execute(
      cidx::application::IncludeRequest{
          .action = cidx::application::IncludeAction::plan,
          .output = "/tmp/forbidden-plan"},
      context);
  CHECK(result.status == cidx::protocol::Status::Refuted);
  CHECK(services.calls.empty());
}

TEST_CASE("direct source and index diff compares effective identities") {
  cidx::Storage db(":memory:");
  cidx::application::StorageApplicationOperations operations(db);
  cidx::application::ApplicationOperationPorts operation_ports{.diff =
                                                                   &operations};
  cidx::application::ApplicationContext context(
      cidx::application::ApplicationPolicy{
          .capabilities = cidx::application::capability_bit(
              cidx::application::Capability::diff)},
      operation_ports);
  const cidx::application::DefaultApplicationServices services;
  const cidx::application::ApplicationService service(services);
  const auto source = service.execute(
      cidx::application::DiffRequest{.scope =
                                         cidx::application::DiffScope::source,
                                     .left = "rev-a",
                                     .right = "rev-b"},
      context);
  CHECK(cidx::json_out::dumps_indent2(source.result).find("false") !=
        std::string::npos);
  const auto index = service.execute(
      cidx::application::DiffRequest{.scope =
                                         cidx::application::DiffScope::index,
                                     .left = "index-a",
                                     .right = "index-b"},
      context);
  CHECK(cidx::json_out::dumps_indent2(index.result).find("false") !=
        std::string::npos);
}

TEST_CASE("direct AST service parses a registered translation unit") {
  const std::filesystem::path source =
      std::filesystem::temp_directory_path() / "cidx-application-ast.cpp";
  {
    std::ofstream output(source);
    REQUIRE(output.good());
    output << "int answer() { return 42; }\n";
  }
  cidx::Storage db(":memory:");
  db.add_component("app", source.parent_path().string());
  db.add_file_path(source.string(), std::nullopt, std::nullopt,
                   std::vector<std::string>{"-std=c++17"}, std::string("c++"));
  cidx::application::StorageApplicationOperations operations(db);
  cidx::application::ApplicationOperationPorts operation_ports{.ast =
                                                                   &operations};
  cidx::application::ApplicationContext context(
      cidx::application::ApplicationPolicy{
          .capabilities = cidx::application::capability_bit(
              cidx::application::Capability::ast)},
      operation_ports);
  const cidx::application::DefaultApplicationServices services;
  const cidx::application::ApplicationService service(services);
  const auto result = service.execute(
      cidx::application::AstInspectionRequest{
          .action = cidx::application::AstInspectionAction::dump,
          .source = source.string()},
      context);
  CHECK(result.status == cidx::protocol::Status::Complete);
  CHECK(cidx::json_out::dumps_indent2(result.result).find("cursor_nodes") !=
        std::string::npos);
  std::error_code ec;
  std::filesystem::remove(source, ec);
}

TEST_CASE("index service enforces work and diagnostic budgets stably") {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "cidx-application-budget";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  REQUIRE_FALSE(ec);
  const std::filesystem::path first = root / "first.cpp";
  const std::filesystem::path second = root / "second.cpp";
  {
    std::ofstream(first) << "int first() { return 1; }\n";
    std::ofstream(second) << "int second() { return 2; }\n";
  }

  cidx::Storage db(":memory:");
  db.add_component("app", root.string());
  for (const auto &source : {first, second}) {
    db.add_file_path(source.string(), std::nullopt, std::nullopt,
                     std::vector<std::string>{"-std=c++17"},
                     std::string("c++"));
  }
  cidx::StorageWorkspaceAdapter workspace_data(db);
  cidx::WorkspaceContext workspace =
      cidx::WorkspaceContext::borrow(workspace_data);
  cidx::application::StorageApplicationOperations operations(db);
  cidx::application::ApplicationOperationPorts operation_ports{.index =
                                                                   &operations};
  cidx::application::ApplicationContext context(
      workspace,
      cidx::application::ApplicationPolicy{.max_work_items = 1,
                                           .max_diagnostics = 0},
      {}, {}, operation_ports);
  RecordingProgress progress;
  context.set_progress_sink(&progress);
  const cidx::application::DefaultApplicationServices services;
  const cidx::application::ApplicationService service(services);
  const auto result =
      service.execute(cidx::application::IndexRequest{}, context);

  CHECK(result.status == cidx::protocol::Status::Complete);
  CHECK(result.completeness.truncated);
  CHECK(result.completeness.budget == 1);
  CHECK(result.diagnostics.empty());
  REQUIRE(progress.events.size() == 1);
  CHECK(progress.events.front().sequence == 0);
  CHECK(progress.events.front().completed == 0);
  CHECK(progress.events.front().total == 1);

  std::filesystem::remove_all(root, ec);
}

TEST_CASE("direct analysis service exports real fact relations") {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "cidx-application-analysis";
  std::error_code ec;
  std::filesystem::remove_all(directory, ec);
  std::filesystem::create_directories(directory, ec);
  REQUIRE_FALSE(ec);
  const std::string index = (directory / "index.db").string();
  cidx::Storage db(index);
  cidx::application::StorageApplicationOperations operations(db, index);
  cidx::application::ApplicationOperationPorts operation_ports{.analysis =
                                                                   &operations};
  cidx::application::ApplicationContext context(
      cidx::application::ApplicationPolicy{
          .capabilities = cidx::application::capability_bit(
                              cidx::application::Capability::analysis) |
                          cidx::application::capability_bit(
                              cidx::application::Capability::artifacts)},
      operation_ports);
  const cidx::application::DefaultApplicationServices services;
  const cidx::application::ApplicationService service(services);
  const auto result = service.execute(
      cidx::application::AnalysisRequest{
          .action = cidx::application::AnalysisAction::export_facts,
          .export_directory = (directory / "facts").string()},
      context);
  CHECK(result.status == cidx::protocol::Status::Complete);
  CHECK(std::filesystem::exists(directory / "facts" / "symbol.facts"));
  std::filesystem::remove_all(directory, ec);
}

TEST_CASE("default services execute real storage-backed operations") {
  cidx::Storage db(":memory:");
  cidx::StorageWorkspaceAdapter workspace_data(db);
  cidx::WorkspaceContext workspace =
      cidx::WorkspaceContext::borrow(workspace_data);
  cidx::query::SqliteQueryReadAdapter query_read(db);
  cidx::application::ApplicationReadPorts read_ports{
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
  cidx::application::ApplicationWritePorts write_ports{
      .workspace = &db.workspace_catalog_write(),
      .source = &db.source_write(),
      .symbols = &db.symbol_write(),
      .types = &db.type_write(),
      .facts = &db.fact_write(),
      .definitions = &db.definition_write(),
      .includes = &db.include_write(),
      .unit_of_work = &db.unit_of_work(),
  };
  cidx::application::StorageApplicationOperations operations(db);
  cidx::application::ApplicationOperationPorts operation_ports{
      .index = &operations,
      .analysis = &operations,
      .ast = &operations,
      .diff = &operations,
      .include = &operations,
  };
  cidx::application::ApplicationContext context(
      workspace, {}, read_ports, write_ports, operation_ports, nullptr);
  const cidx::application::DefaultApplicationServices services;
  const cidx::application::ApplicationService service(services);

  CHECK(service.execute(cidx::application::IndexRequest{}, context).status ==
        cidx::protocol::Status::Complete);
  CHECK(service.execute(cidx::application::IncludeRequest{}, context).status ==
        cidx::protocol::Status::Complete);
  CHECK(
      service
          .execute(cidx::application::QueryRequest{.expression =
                                                       "codebase() | nodes()"},
                   context)
          .status == cidx::protocol::Status::Complete);

  const auto analysis = service.execute(
      cidx::application::AnalysisRequest{
          .action = cidx::application::AnalysisAction::list},
      context);
  CHECK(cidx::json_out::dumps_indent2(analysis.result).find("callgraph") !=
        std::string::npos);

  const auto unknown_source = service.execute(
      cidx::application::AstInspectionRequest{.source = "missing.cpp"},
      context);
  CHECK(unknown_source.status == cidx::protocol::Status::Error);
  CHECK(unknown_source.diagnostics.front().code == "unknown_source");
}

int main(int argc, char **argv) {
  doctest::Context context(argc, argv);
  return context.run();
}
