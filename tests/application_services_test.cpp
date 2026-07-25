#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

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
  cidx::application::ApplicationContext context(workspace, {}, read_ports,
                                                write_ports, nullptr, &db);
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
}

int main(int argc, char **argv) {
  doctest::Context context(argc, argv);
  return context.run();
}
