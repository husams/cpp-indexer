// Tests for ConformanceRecorder (HSE-89): the C++ conformance gate for the
// index-generation-publication flow named alongside QueryPlan read-only
// execution and sidecar publication in the acceptance criteria. Every test
// wraps FakeServices with ConformanceRecorder::wrapping(), which reads the
// real, checked-in spec/tla/conformance/*.json schema files (not an invented
// parallel vocabulary) -- so a passing test here means the recorded trace
// was replayed against the declared schema itself.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <memory>

#include "application/conformance_recorder.hpp"
#include "storage/ports.hpp"
#include "storage/storage.hpp"
#include "workspace/context.hpp"

namespace {

using namespace cidx::application;
using cidx::protocol::ResultEnvelope;
using cidx::protocol::Status;

ResultEnvelope base_envelope(std::string operation, Status status,
                             std::string completeness_state,
                             std::string freshness) {
  ResultEnvelope envelope;
  envelope.operation = std::move(operation);
  envelope.status = status;
  envelope.identity.workspace = "test";
  envelope.identity.index = "test";
  envelope.identity.fact_sets = {"application"};
  envelope.identity.freshness = std::move(freshness);
  envelope.completeness.state = std::move(completeness_state);
  envelope.producer.backend = "test";
  return envelope;
}

// A service double, in the same style as application_services_test.cpp's
// RecordingServices, but with request-shaped response bodies so the
// recorder has something meaningful to observe.
struct FakeServices final : ApplicationServices {
  ResultEnvelope index_response;
  ResultEnvelope query_response =
      base_envelope("query", Status::Complete, "complete", "current");
  ResultEnvelope analysis_response;

  ResultEnvelope index(const IndexRequest &request,
                       ApplicationContext &context) const override {
    (void)request;
    (void)context;
    return index_response;
  }
  ResultEnvelope query(const QueryRequest &request,
                       ApplicationContext &context) const override {
    (void)request;
    (void)context;
    return query_response;
  }
  ResultEnvelope analysis(const AnalysisRequest &request,
                          ApplicationContext &context) const override {
    (void)request;
    (void)context;
    return analysis_response;
  }
  ResultEnvelope workspace(const WorkspaceRequest &request,
                           ApplicationContext &context) const override {
    (void)request;
    (void)context;
    return base_envelope("workspace", Status::Complete, "complete", "current");
  }
  ResultEnvelope ast(const AstInspectionRequest &request,
                     ApplicationContext &context) const override {
    (void)request;
    (void)context;
    return base_envelope("ast", Status::Complete, "complete", "current");
  }
  ResultEnvelope diff(const DiffRequest &request,
                      ApplicationContext &context) const override {
    (void)request;
    (void)context;
    return base_envelope("diff", Status::Complete, "complete", "current");
  }
  ResultEnvelope include(const IncludeRequest &request,
                         ApplicationContext &context) const override {
    (void)request;
    (void)context;
    return base_envelope("include", Status::Complete, "complete", "current");
  }
  ResultEnvelope refactor(const RefactoringRequest &request,
                          ApplicationContext &context) const override {
    (void)request;
    (void)context;
    return base_envelope("refactor", Status::Complete, "complete", "current");
  }
  ResultEnvelope proof(const ProofRequest &request,
                       ApplicationContext &context) const override {
    (void)request;
    (void)context;
    return base_envelope("proof", Status::Complete, "complete", "current");
  }
};

ApplicationContext test_context() { return ApplicationContext{}; }

QueryRequest read_only_query() {
  return QueryRequest{.expression = "symbols()",
                      .output = QueryOutput::human,
                      .explain = false,
                      .index = std::nullopt};
}

// The smallest write-port interface (a single factory method), used only to
// obtain a non-null pointer -- never actually invoked -- so a test can seed
// "a write port was wired into a supposedly read-only query context".
struct FakeUnitOfWorkFactory final : cidx::storage::UnitOfWorkFactory {
  std::unique_ptr<cidx::storage::UnitOfWork> begin() override {
    return nullptr;
  }
};

} // namespace

TEST_CASE("conformance recorder accepts a well-behaved index, query, and "
          "sidecar-publish trace") {
  FakeServices fake;
  fake.index_response =
      base_envelope("index", Status::Complete, "complete", "current");
  fake.index_response.artifacts.push_back({.kind = "semantic-index",
                                           .id = "generation-1",
                                           .schema_version = 1,
                                           .catalog_version = 1,
                                           .catalog_hash = "test-hash"});
  fake.analysis_response =
      base_envelope("analysis", Status::Complete, "complete", "current");
  fake.analysis_response.artifacts.push_back({.kind = "analysis",
                                              .id = "sidecar-1",
                                              .schema_version = 1,
                                              .catalog_version = 1,
                                              .catalog_hash = "test-hash"});

  ConformanceRecorder recorder = ConformanceRecorder::wrapping(fake);
  ApplicationContext context = test_context();
  recorder.index(IndexRequest{.action = IndexAction::update,
                              .files = {},
                              .source = std::nullopt,
                              .graph = true,
                              .autoderive_labels = true,
                              .json = false,
                              .index = std::nullopt},
                 context);
  recorder.query(read_only_query(), context);
  recorder.analysis(AnalysisRequest{.action = AnalysisAction::execute,
                                    .rule = std::nullopt,
                                    .rules_file = std::nullopt,
                                    .export_directory = std::nullopt,
                                    .index = std::nullopt,
                                    .jobs = 1},
                    context);

  REQUIRE(recorder.observations().size() == 3);
  CHECK(recorder.observations()[0].operation == "index.publish");
  CHECK(recorder.observations()[1].operation == "query.return");
  CHECK(recorder.observations()[2].operation == "sidecar.publish");
  CHECK(recorder.conformant());
}

TEST_CASE("conformance recorder rejects a seeded partial-publication defect") {
  FakeServices fake;
  // Seeded defect: claims a complete, current index generation but never
  // published an artifact for it -- the exact NoPartialPublication violation
  // seeded elsewhere against the abstract TLA+ model
  // (spec/tla/tools/check-regression.sh's "partial-publication" scenario).
  fake.index_response =
      base_envelope("index", Status::Complete, "complete", "current");
  REQUIRE(fake.index_response.artifacts.empty());

  ConformanceRecorder recorder = ConformanceRecorder::wrapping(fake);
  ApplicationContext context = test_context();
  recorder.index(IndexRequest{.action = IndexAction::update,
                              .files = {},
                              .source = std::nullopt,
                              .graph = true,
                              .autoderive_labels = true,
                              .json = false,
                              .index = std::nullopt},
                 context);

  CHECK_FALSE(recorder.conformant());
}

TEST_CASE("conformance recorder ignores index() calls that are not the "
          "update action") {
  FakeServices fake;
  fake.index_response =
      base_envelope("index", Status::Complete, "complete", "current");
  REQUIRE(fake.index_response.artifacts.empty());

  ConformanceRecorder recorder = ConformanceRecorder::wrapping(fake);
  ApplicationContext context = test_context();
  recorder.index(IndexRequest{.action = IndexAction::status,
                              .files = {},
                              .source = std::nullopt,
                              .graph = true,
                              .autoderive_labels = true,
                              .json = false,
                              .index = std::nullopt},
                 context);

  CHECK(recorder.observations().empty());
  CHECK(recorder.conformant());
}

TEST_CASE("conformance recorder rejects a seeded query-write defect") {
  FakeServices fake;
  fake.query_response =
      base_envelope("query", Status::Complete, "complete", "current");

  ConformanceRecorder recorder = ConformanceRecorder::wrapping(fake);
  FakeUnitOfWorkFactory write_capable_factory;
  ApplicationWritePorts write_ports;
  write_ports.unit_of_work = &write_capable_factory;
  // Seeded defect: a write port is wired into the context a query executes
  // under -- CidxProtected.ReadOnlyQueries(queryWrites) forbids this for any
  // call claiming query.return. A minimal real in-memory Storage/
  // WorkspaceContext is needed here because ApplicationContext only exposes
  // write ports through the constructor overload that takes one.
  cidx::Storage storage(":memory:");
  cidx::StorageWorkspaceAdapter workspace_data(storage);
  cidx::WorkspaceContext workspace = cidx::WorkspaceContext::borrow(
      workspace_data, cidx::WorkspaceReadWriteMode::read_only);
  ApplicationContext context(workspace, ApplicationPolicy{},
                             ApplicationReadPorts{}, write_ports);
  recorder.query(read_only_query(), context);

  REQUIRE(recorder.observations().size() == 1);
  CHECK_FALSE(recorder.conformant());
}

TEST_CASE("conformance recorder only labels analysis() a sidecar publish for "
          "execute/export_facts, not list") {
  FakeServices fake;
  fake.analysis_response =
      base_envelope("analysis", Status::Complete, "complete", "current");

  ConformanceRecorder recorder = ConformanceRecorder::wrapping(fake);
  ApplicationContext context = test_context();
  recorder.analysis(AnalysisRequest{.action = AnalysisAction::list,
                                    .rule = std::nullopt,
                                    .rules_file = std::nullopt,
                                    .export_directory = std::nullopt,
                                    .index = std::nullopt,
                                    .jobs = 1},
                    context);

  CHECK(recorder.observations().empty());
}

TEST_CASE("conformance recorder rejects a seeded illegal-sidecar-state "
          "defect (claims execute success, publishes no artifact)") {
  FakeServices fake;
  fake.analysis_response =
      base_envelope("analysis", Status::Complete, "complete", "current");
  REQUIRE(fake.analysis_response.artifacts.empty());

  ConformanceRecorder recorder = ConformanceRecorder::wrapping(fake);
  ApplicationContext context = test_context();
  recorder.analysis(AnalysisRequest{.action = AnalysisAction::execute,
                                    .rule = std::nullopt,
                                    .rules_file = std::nullopt,
                                    .export_directory = std::nullopt,
                                    .index = std::nullopt,
                                    .jobs = 1},
                    context);

  REQUIRE(recorder.observations().size() == 1);
  CHECK_FALSE(recorder.conformant());
}

TEST_CASE("conformance recorder does not claim the six out-of-scope "
          "ApplicationServices methods") {
  FakeServices fake;
  ConformanceRecorder recorder = ConformanceRecorder::wrapping(fake);
  ApplicationContext context = test_context();
  recorder.workspace(WorkspaceRequest{}, context);
  recorder.ast(AstInspectionRequest{}, context);
  recorder.diff(DiffRequest{}, context);
  recorder.include(IncludeRequest{}, context);
  recorder.refactor(RefactoringRequest{}, context);
  recorder.proof(ProofRequest{}, context);

  CHECK(recorder.observations().empty());
  CHECK(recorder.conformant());
}
