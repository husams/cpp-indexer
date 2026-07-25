// Tests for ConformanceRecorder (HSE-89): the C++ conformance gate for the
// index-generation-publication flow named alongside QueryPlan read-only
// execution and sidecar publication in the acceptance criteria. A
// well-behaved service must be recorded as conformant; a seeded defect that
// claims a published, current index generation without any published
// artifact -- exactly what CidxProtected.NoPartialPublication forbids --
// must be rejected.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "application/conformance_recorder.hpp"

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

} // namespace

TEST_CASE("conformance recorder accepts a well-behaved index publication") {
  FakeServices fake;
  fake.index_response =
      base_envelope("index", Status::Complete, "complete", "current");
  fake.index_response.artifacts.push_back({.kind = "semantic-index",
                                           .id = "generation-1",
                                           .schema_version = 1,
                                           .catalog_version = 1,
                                           .catalog_hash = "test-hash"});

  ConformanceRecorder recorder(fake);
  ApplicationContext context = test_context();
  recorder.index(IndexRequest{}, context);
  recorder.query(QueryRequest{.expression = "symbols()",
                              .output = QueryOutput::human,
                              .explain = false,
                              .index = std::nullopt},
                 context);
  recorder.analysis(AnalysisRequest{}, context);

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

  ConformanceRecorder recorder(fake);
  ApplicationContext context = test_context();
  recorder.index(IndexRequest{}, context);

  CHECK_FALSE(recorder.conformant());
}

TEST_CASE("conformance recorder ignores non-publication operations when "
          "judging conformance") {
  FakeServices fake;
  fake.query_response =
      base_envelope("query", Status::Complete, "complete", "current");

  ConformanceRecorder recorder(fake);
  ApplicationContext context = test_context();
  recorder.query(QueryRequest{.expression = "symbols()",
                              .output = QueryOutput::human,
                              .explain = false,
                              .index = std::nullopt},
                 context);

  // A read-only query reporting complete/current carries no artifact and is
  // not held to the publication rule -- only index.publish observations are.
  CHECK(recorder.conformant());
}
