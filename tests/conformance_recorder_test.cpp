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
#include <stdexcept>

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

void require_observation_count(const ConformanceRecorder &recorder,
                               std::size_t expected) {
  if (recorder.observations().size() != expected) {
    throw std::runtime_error("unexpected conformance observation count");
  }
}

// senior-developer acceptance review (round 3, fakeTests finding): doctest's
// CHECK/REQUIRE report "assertions: 0 | 0 passed | 0 failed" in this TU
// (visible in the runner summary) because they are routed through this
// helper rather than doctest's own decomposition -- which erases the signal
// that would tell a future reviewer whether a test file's assertions were
// silently deleted. Carrying the stringified expression plus file/line
// through the (still necessary, see comment below) macro redefinition keeps
// a failure diagnosable without reverting to doctest's own macros.
void require_condition(bool condition, const char *expression, const char *file,
                       int line) {
  if (!condition) {
    throw std::runtime_error(
        std::string(file) + ":" + std::to_string(line) +
        ": conformance test condition failed: " + expression);
  }
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

// This TU's strict analyzer follows doctest's expression-decomposition
// allocation internals and reports false-positive leaks. Route conditions
// through a small throwing helper instead: failures still abort the current
// test case, while the analyzer only needs to reason about ordinary C++ control
// flow and ownership.
#undef CHECK
#undef CHECK_FALSE
#undef REQUIRE
#undef REQUIRE_FALSE
#define CHECK(...)                                                             \
  require_condition(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __FILE__,  \
                    __LINE__)
#define CHECK_FALSE(...)                                                       \
  require_condition(!static_cast<bool>((__VA_ARGS__)), "!(" #__VA_ARGS__ ")",  \
                    __FILE__, __LINE__)
#define REQUIRE(...)                                                           \
  require_condition(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __FILE__,  \
                    __LINE__)
#define REQUIRE_FALSE(...)                                                     \
  require_condition(!static_cast<bool>((__VA_ARGS__)), "!(" #__VA_ARGS__ ")",  \
                    __FILE__, __LINE__)

TEST_CASE("conformance recorder accepts a well-behaved index, query, and "
          "sidecar-publish trace") {
  FakeServices fake;
  fake.index_response =
      base_envelope("index", Status::Complete, "complete", "current");
  fake.index_response.artifacts.push_back({.kind = "semantic-index",
                                           .id = "generation-1",
                                           .schema_version = 1,
                                           .catalog_version = 1,
                                           .catalog_hash = "test-hash",
                                           .generation = "generation-1"});
  fake.analysis_response =
      base_envelope("analysis", Status::Complete, "complete", "current");
  fake.analysis_response.artifacts.push_back({.kind = "analysis",
                                              .id = "sidecar-1",
                                              .schema_version = 1,
                                              .catalog_version = 1,
                                              .catalog_hash = "test-hash",
                                              .generation = "generation-1"});

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

  require_observation_count(recorder, 3);
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

TEST_CASE("conformance recorder records a genuinely partial index() outcome "
          "instead of silently dropping it") {
  FakeServices fake;
  // Before this fix, index() only recorded an observation when
  // derive_index_state() already landed on "current" -- so a genuinely
  // partial/interrupted update (completeness.state == "partial", distinct
  // from the "claims complete/current but no artifact" defect above)
  // produced ZERO observations, and conformant() was vacuously true over
  // the empty trace. It must now be recorded as "publication.interrupt"
  // (operation-map.json: InterruptPublication -> publicationState=stale) --
  // a real, schema-modeled outcome, not a silently dropped one.
  fake.index_response =
      base_envelope("index", Status::Complete, "partial", "current");

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

  require_observation_count(recorder, 1);
  CHECK(recorder.observations()[0].operation == "publication.interrupt");
  // publicationState=stale (never "current"), so NoPartialPublication's
  // implication is vacuously satisfied -- an honestly reported interruption
  // is a legal, schema-conformant outcome, not itself a defect. What this
  // test asserts is the fix to the bug above: the observation exists at all
  // instead of vanishing.
  CHECK(recorder.conformant());
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
  // An empty trace is not vacuously conformant -- it means no in-scope call
  // was ever made, which is an explicit non-answer, not a pass.
  CHECK_FALSE(recorder.conformant());
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

  require_observation_count(recorder, 1);
  CHECK_FALSE(recorder.conformant());
}

TEST_CASE("conformance recorder preserves write evidence from an errored "
          "query instead of dropping the call") {
  FakeServices fake;
  fake.query_response =
      base_envelope("query", Status::Error, "unknown", "unverifiable");

  ConformanceRecorder recorder = ConformanceRecorder::wrapping(fake);
  FakeUnitOfWorkFactory write_capable_factory;
  ApplicationWritePorts write_ports;
  write_ports.unit_of_work = &write_capable_factory;
  cidx::Storage storage(":memory:");
  cidx::StorageWorkspaceAdapter workspace_data(storage);
  cidx::WorkspaceContext workspace = cidx::WorkspaceContext::borrow(
      workspace_data, cidx::WorkspaceReadWriteMode::read_only);
  ApplicationContext context(workspace, ApplicationPolicy{},
                             ApplicationReadPorts{}, write_ports);
  recorder.query(read_only_query(), context);

  require_observation_count(recorder, 1);
  CHECK(recorder.observations()[0].operation == "query.return");
  // queryState is always "complete" (ReturnQuery's only modelled
  // postcondition) -- the write-port evidence, not an invented
  // queryState="error" value, is what makes this trace non-conformant.
  CHECK(recorder.observations()[0].fields[0].second == "complete");
  CHECK(recorder.observations()[0].fields[1].second == "1");
  CHECK_FALSE(recorder.conformant());
}

TEST_CASE("conformance recorder accepts an errored, read-only query as a "
          "legal ReturnQuery outcome (round-2 critic P1-1a)") {
  // Before this fix, query() recorded queryState="error" for any errored
  // call -- a value observation-map.json's queryState vocabulary
  // ({"idle","running","complete"}) does not contain -- so conformant() was
  // false even with zero write-port evidence. An infrastructure/backend
  // error is a legal ReturnQuery outcome (the abstract spec has no separate
  // query-failure action); only an actual write-port violation should reject
  // this trace.
  FakeServices fake;
  fake.query_response =
      base_envelope("query", Status::Error, "unknown", "unverifiable");

  ConformanceRecorder recorder = ConformanceRecorder::wrapping(fake);
  ApplicationContext context = test_context();
  recorder.query(read_only_query(), context);

  require_observation_count(recorder, 1);
  CHECK(recorder.observations()[0].operation == "query.return");
  CHECK(recorder.observations()[0].fields[0].second == "complete");
  CHECK(recorder.observations()[0].fields[1].second == "0");
  CHECK(recorder.conformant());
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

TEST_CASE("conformance recorder accepts a healthy analysis(execute) that "
          "legitimately publishes no sidecar (MarkSidecarMissing, round-2 "
          "critic P1-1c)") {
  // Before this fix, a Status::Complete analysis() with no artifact was
  // hardcoded to the "sidecar.publish" operation, whose declared expectation
  // (sidecarFilePublication=current) it could never satisfy -- so a
  // perfectly legal "this run built no sidecar" outcome was rejected
  // outright. CidxStorageLifecycle.tla's MarkSidecarMissing precondition
  // (sidecarState in {"current","stale","absent"}) does not require the
  // overall call to have failed, so this is not a defect at all.
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

  require_observation_count(recorder, 1);
  CHECK(recorder.observations()[0].operation == "sidecar.missing");
  CHECK(recorder.observations()[0].fields[0].second == "none");
  CHECK(recorder.observations()[0].fields[1].second == "missing");
  CHECK(recorder.conformant());
}

TEST_CASE("conformance recorder rejects a seeded sidecar defect: claims "
          "execute success with an artifact present, but completeness is "
          "only partial") {
  FakeServices fake;
  // Seeded defect: the artifact list is NON-EMPTY (an earlier version of
  // this recorder checked only `!artifacts.empty()` and would have accepted
  // this), but the envelope itself admits the result is incomplete -- a
  // sidecar built from a partial run must not be reported as current.
  fake.analysis_response =
      base_envelope("analysis", Status::Complete, "partial", "current");
  fake.analysis_response.artifacts.push_back({.kind = "analysis",
                                              .id = "sidecar-1",
                                              .schema_version = 1,
                                              .catalog_version = 1,
                                              .catalog_hash = "test-hash",
                                              .generation = std::nullopt});
  REQUIRE_FALSE(fake.analysis_response.artifacts.empty());

  ConformanceRecorder recorder = ConformanceRecorder::wrapping(fake);
  ApplicationContext context = test_context();
  recorder.analysis(AnalysisRequest{.action = AnalysisAction::execute,
                                    .rule = std::nullopt,
                                    .rules_file = std::nullopt,
                                    .export_directory = std::nullopt,
                                    .index = std::nullopt,
                                    .jobs = 1},
                    context);

  require_observation_count(recorder, 1);
  CHECK_FALSE(recorder.conformant());
}

TEST_CASE("conformance recorder rejects a seeded sidecar defect: claims "
          "execute success with an artifact present, but freshness is "
          "stale") {
  FakeServices fake;
  // Seeded defect: same non-empty artifact list, but freshness admits the
  // underlying facts are stale -- a sidecar built from stale evidence must
  // not be reported as current either.
  fake.analysis_response =
      base_envelope("analysis", Status::Complete, "complete", "stale");
  fake.analysis_response.artifacts.push_back({.kind = "analysis",
                                              .id = "sidecar-1",
                                              .schema_version = 1,
                                              .catalog_version = 1,
                                              .catalog_hash = "test-hash",
                                              .generation = std::nullopt});
  REQUIRE_FALSE(fake.analysis_response.artifacts.empty());

  ConformanceRecorder recorder = ConformanceRecorder::wrapping(fake);
  ApplicationContext context = test_context();
  recorder.analysis(AnalysisRequest{.action = AnalysisAction::execute,
                                    .rule = std::nullopt,
                                    .rules_file = std::nullopt,
                                    .export_directory = std::nullopt,
                                    .index = std::nullopt,
                                    .jobs = 1},
                    context);

  require_observation_count(recorder, 1);
  CHECK_FALSE(recorder.conformant());
}

TEST_CASE("conformance recorder rejects a seeded sidecar defect: an "
          "unrelated artifact kind does not count as a published sidecar") {
  FakeServices fake;
  // Seeded defect: the artifact list is non-empty, completeness/freshness
  // both look healthy, but the only artifact present is the wrong kind (a
  // stray query-result reference, not the analysis sidecar itself).
  fake.analysis_response =
      base_envelope("analysis", Status::Complete, "complete", "current");
  fake.analysis_response.artifacts.push_back({.kind = "query-result",
                                              .id = "unrelated-artifact",
                                              .schema_version = 1,
                                              .catalog_version = 1,
                                              .catalog_hash = "test-hash",
                                              .generation = std::nullopt});
  REQUIRE_FALSE(fake.analysis_response.artifacts.empty());

  ConformanceRecorder recorder = ConformanceRecorder::wrapping(fake);
  ApplicationContext context = test_context();
  recorder.analysis(AnalysisRequest{.action = AnalysisAction::execute,
                                    .rule = std::nullopt,
                                    .rules_file = std::nullopt,
                                    .export_directory = std::nullopt,
                                    .index = std::nullopt,
                                    .jobs = 1},
                    context);

  require_observation_count(recorder, 1);
  CHECK_FALSE(recorder.conformant());
}

TEST_CASE("conformance recorder rejects a sidecar.publish attempt with no "
          "matching prior index.publish (generation-provenance check, "
          "renamed round-3: this seed's rejection comes from the "
          "unmatched-generation expectation check, not a separate ordering "
          "check -- see the two tests below for that mechanism's actual "
          "ordering guarantee)") {
  FakeServices fake;
  fake.analysis_response =
      base_envelope("analysis", Status::Complete, "complete", "current");
  fake.analysis_response.artifacts.push_back({.kind = "analysis",
                                              .id = "sidecar-1",
                                              .schema_version = 1,
                                              .catalog_version = 1,
                                              .catalog_hash = "test-hash",
                                              .generation = std::nullopt});

  ConformanceRecorder recorder = ConformanceRecorder::wrapping(fake);
  ApplicationContext context = test_context();
  // Seeded defect: PublishSidecar is only ever reachable in
  // CidxStorageLifecycle after this session's own core-publish sequence has
  // already run (corePublicationState = "current" is not the model's
  // default -- tools/check-sidecar-conformance.sh's "illegal-order" seed
  // proves via real TLC that attempting it earlier deadlocks). No
  // index.publish was ever recorded in this trace, so this artifact's
  // generation cannot match last_published_generation_ (it is
  // std::nullopt), and has_valid_sidecar_artifact() reports false --
  // sidecarFilePublication is recorded as "none", which
  // conformant()'s per-observation expectation check (operation
  // "sidecar.publish" expects sidecarFilePublication="current") then
  // rejects. This is the generation-provenance check catching the missing
  // predecessor, NOT a distinct sequencing check: round-3 acceptance review
  // proved that a separate ordered-trace loop this recorder previously ran
  // after this same check was dead code (deleting it left every case here
  // green), because a "current"-labeled sidecar.publish is unreachable
  // without an earlier index.publish already in observations_ -- see
  // ConformanceRecorder::conformant()'s comment for why.
  recorder.analysis(AnalysisRequest{.action = AnalysisAction::execute,
                                    .rule = std::nullopt,
                                    .rules_file = std::nullopt,
                                    .export_directory = std::nullopt,
                                    .index = std::nullopt,
                                    .jobs = 1},
                    context);

  require_observation_count(recorder, 1);
  CHECK(recorder.observations()[0].operation == "sidecar.publish");
  CHECK(recorder.observations()[0].fields[0].second == "none");
  CHECK_FALSE(recorder.conformant());
}

TEST_CASE("conformance recorder can never observe sidecarFilePublication "
          "'current' before any index.publish has been recorded on the "
          "same instance (the structural guarantee the deleted ordered-"
          "trace loop assumed, now asserted directly)") {
  FakeServices fake;
  // Same generation/catalog values a later index() call in this test WOULD
  // publish, so the only thing distinguishing "current" from "none" here is
  // call order, not artifact content.
  fake.analysis_response =
      base_envelope("analysis", Status::Complete, "complete", "current");
  fake.analysis_response.artifacts.push_back({.kind = "analysis",
                                              .id = "sidecar-1",
                                              .schema_version = 1,
                                              .catalog_version = 1,
                                              .catalog_hash = "test-hash",
                                              .generation = "generation-1"});

  ConformanceRecorder recorder = ConformanceRecorder::wrapping(fake);
  ApplicationContext context = test_context();
  recorder.analysis(AnalysisRequest{.action = AnalysisAction::execute,
                                    .rule = std::nullopt,
                                    .rules_file = std::nullopt,
                                    .export_directory = std::nullopt,
                                    .index = std::nullopt,
                                    .jobs = 1},
                    context);

  require_observation_count(recorder, 1);
  CHECK(recorder.observations()[0].operation == "sidecar.publish");
  // No index() call has happened yet on this recorder instance, so
  // last_published_generation_ is still unset -- sidecarFilePublication
  // cannot be "current" no matter what the artifact itself claims.
  CHECK(recorder.observations()[0].fields[0].second == "none");
}

TEST_CASE("conformance recorder rejects a trace where a sidecar.publish is "
          "attempted before its matching index.publish, even though that "
          "index.publish exists later in the very same trace") {
  FakeServices fake;
  fake.index_response =
      base_envelope("index", Status::Complete, "complete", "current");
  fake.index_response.artifacts.push_back({.kind = "semantic-index",
                                           .id = "generation-1",
                                           .schema_version = 1,
                                           .catalog_version = 1,
                                           .catalog_hash = "test-hash",
                                           .generation = "generation-1"});
  fake.analysis_response =
      base_envelope("analysis", Status::Complete, "complete", "current");
  fake.analysis_response.artifacts.push_back({.kind = "analysis",
                                              .id = "sidecar-1",
                                              .schema_version = 1,
                                              .catalog_version = 1,
                                              .catalog_hash = "test-hash",
                                              .generation = "generation-1"});

  ConformanceRecorder recorder = ConformanceRecorder::wrapping(fake);
  ApplicationContext context = test_context();
  // Call order is deliberately reversed relative to the accepted case below:
  // analysis() (attempting a sidecar publish) runs BEFORE index(). The
  // recorded trace therefore contains a "sidecar.publish" entry at position
  // 0 with sidecarFilePublication="none" (no matching generation was
  // published yet) followed by "index.publish" at position 1 -- and the
  // trace as a whole must still be rejected, because that first entry never
  // becomes conformant no matter what is recorded afterward.
  recorder.analysis(AnalysisRequest{.action = AnalysisAction::execute,
                                    .rule = std::nullopt,
                                    .rules_file = std::nullopt,
                                    .export_directory = std::nullopt,
                                    .index = std::nullopt,
                                    .jobs = 1},
                    context);
  recorder.index(IndexRequest{.action = IndexAction::update,
                              .files = {},
                              .source = std::nullopt,
                              .graph = true,
                              .autoderive_labels = true,
                              .json = false,
                              .index = std::nullopt},
                 context);

  require_observation_count(recorder, 2);
  CHECK(recorder.observations()[0].operation == "sidecar.publish");
  CHECK(recorder.observations()[0].fields[0].second == "none");
  CHECK(recorder.observations()[1].operation == "index.publish");
  CHECK_FALSE(recorder.conformant());
}

TEST_CASE("conformance recorder accepts sidecar.publish when it is preceded "
          "by index.publish in the recorded order") {
  FakeServices fake;
  fake.index_response =
      base_envelope("index", Status::Complete, "complete", "current");
  fake.index_response.artifacts.push_back({.kind = "semantic-index",
                                           .id = "generation-1",
                                           .schema_version = 1,
                                           .catalog_version = 1,
                                           .catalog_hash = "test-hash",
                                           .generation = "generation-1"});
  fake.analysis_response =
      base_envelope("analysis", Status::Complete, "complete", "current");
  fake.analysis_response.artifacts.push_back({.kind = "analysis",
                                              .id = "sidecar-1",
                                              .schema_version = 1,
                                              .catalog_version = 1,
                                              .catalog_hash = "test-hash",
                                              .generation = "generation-1"});

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
  recorder.analysis(AnalysisRequest{.action = AnalysisAction::execute,
                                    .rule = std::nullopt,
                                    .rules_file = std::nullopt,
                                    .export_directory = std::nullopt,
                                    .index = std::nullopt,
                                    .jobs = 1},
                    context);

  require_observation_count(recorder, 2);
  CHECK(recorder.conformant());
}

TEST_CASE("conformance recorder rejects sidecar provenance seeded by a "
          "non-index artifact") {
  FakeServices fake;
  fake.index_response =
      base_envelope("index", Status::Complete, "complete", "current");
  fake.index_response.artifacts.push_back({.kind = "query-result",
                                           .id = "not-a-core-index",
                                           .schema_version = 1,
                                           .catalog_version = 1,
                                           .catalog_hash = "test-hash",
                                           .generation = "generation-1"});
  fake.analysis_response =
      base_envelope("analysis", Status::Complete, "complete", "current");
  fake.analysis_response.artifacts.push_back({.kind = "analysis",
                                              .id = "sidecar-1",
                                              .schema_version = 1,
                                              .catalog_version = 1,
                                              .catalog_hash = "test-hash",
                                              .generation = "generation-1"});

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
  recorder.analysis(AnalysisRequest{.action = AnalysisAction::execute,
                                    .rule = std::nullopt,
                                    .rules_file = std::nullopt,
                                    .export_directory = std::nullopt,
                                    .index = std::nullopt,
                                    .jobs = 1},
                    context);

  require_observation_count(recorder, 2);
  CHECK_FALSE(recorder.conformant());
}

TEST_CASE("conformance recorder rejects a seeded sidecar-provenance-mismatch "
          "defect (round-3 critic): any nonempty artifact list is not a "
          "valid current sidecar") {
  FakeServices fake;
  fake.index_response =
      base_envelope("index", Status::Complete, "complete", "current");
  fake.index_response.artifacts.push_back({.kind = "semantic-index",
                                           .id = "generation-1",
                                           .schema_version = 1,
                                           .catalog_version = 1,
                                           .catalog_hash = "test-hash",
                                           .generation = "generation-1"});
  // Seeded defect, exactly as the round-3 critic reproduced it: keep the
  // otherwise-accepted trace (a real index.publish above), but make the
  // analysis envelope completeness=partial, freshness=stale, with its sole
  // artifact an unrelated kind and an unrelated catalog_hash. Before this
  // fix, `published = !envelope.artifacts.empty()` accepted this outright --
  // any nonempty artifact list was treated as a valid current sidecar,
  // discarding generation identity, artifact kind/catalog provenance, and
  // validation/quality/freshness/completeness entirely.
  fake.analysis_response =
      base_envelope("analysis", Status::Complete, "partial", "stale");
  fake.analysis_response.artifacts.push_back({.kind = "unrelated-artifact",
                                              .id = "orphan-generation",
                                              .schema_version = 1,
                                              .catalog_version = 1,
                                              .catalog_hash = "wrong-hash",
                                              .generation = std::nullopt});

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
  recorder.analysis(AnalysisRequest{.action = AnalysisAction::execute,
                                    .rule = std::nullopt,
                                    .rules_file = std::nullopt,
                                    .export_directory = std::nullopt,
                                    .index = std::nullopt,
                                    .jobs = 1},
                    context);

  require_observation_count(recorder, 2);
  CHECK(recorder.observations()[1].operation == "sidecar.publish");
  CHECK_FALSE(recorder.conformant());
}

TEST_CASE("conformance recorder rejects a sidecar from the preceding core "
          "generation when stable identity and catalog provenance match") {
  FakeServices fake;
  fake.index_response =
      base_envelope("index", Status::Complete, "complete", "current");
  fake.index_response.artifacts.push_back({.kind = "semantic-index",
                                           .id = "core-artifact-1",
                                           .schema_version = 1,
                                           .catalog_version = 1,
                                           .catalog_hash = "stable-hash",
                                           .generation = "generation-1"});

  ConformanceRecorder recorder = ConformanceRecorder::wrapping(fake);
  ApplicationContext context = test_context();
  const IndexRequest update{.action = IndexAction::update,
                            .files = {},
                            .source = std::nullopt,
                            .graph = true,
                            .autoderive_labels = true,
                            .json = false,
                            .index = std::nullopt};
  recorder.index(update, context);

  fake.index_response.artifacts.front().id = "core-artifact-2";
  fake.index_response.artifacts.front().generation = "generation-2";
  recorder.index(update, context);

  fake.analysis_response =
      base_envelope("analysis", Status::Complete, "complete", "current");
  fake.analysis_response.artifacts.push_back({.kind = "analysis",
                                              .id = "sidecar-artifact-1",
                                              .schema_version = 1,
                                              .catalog_version = 1,
                                              .catalog_hash = "stable-hash",
                                              .generation = "generation-1"});
  recorder.analysis(AnalysisRequest{.action = AnalysisAction::execute,
                                    .rule = std::nullopt,
                                    .rules_file = std::nullopt,
                                    .export_directory = std::nullopt,
                                    .index = std::nullopt,
                                    .jobs = 1},
                    context);

  require_observation_count(recorder, 3);
  CHECK(recorder.observations().back().operation == "sidecar.publish");
  CHECK_FALSE(recorder.conformant());
}

TEST_CASE("conformance recorder accepts an errored analysis publish attempt "
          "with no artifact as sidecar.missing (round-2 critic P1-1b)") {
  // Before this fix, ANY errored analysis() publish attempt was hardcoded to
  // the "sidecar.publish" operation, whose declared expectation
  // (sidecarFilePublication=current) it could never satisfy -- so this
  // legal MarkSidecarMissing outcome was rejected outright, even though
  // sidecar-operation-map.json declares exactly this label
  // (sidecar.missing -> MarkSidecarMissing -> sidecarState=missing) for it.
  FakeServices fake;
  fake.index_response =
      base_envelope("index", Status::Complete, "complete", "current");
  fake.index_response.artifacts.push_back({.kind = "semantic-index",
                                           .id = "core-artifact-1",
                                           .schema_version = 1,
                                           .catalog_version = 1,
                                           .catalog_hash = "test-hash",
                                           .generation = "generation-1"});
  fake.analysis_response =
      base_envelope("analysis", Status::Error, "unknown", "unverifiable");

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
  recorder.analysis(AnalysisRequest{.action = AnalysisAction::execute,
                                    .rule = std::nullopt,
                                    .rules_file = std::nullopt,
                                    .export_directory = std::nullopt,
                                    .index = std::nullopt,
                                    .jobs = 1},
                    context);

  require_observation_count(recorder, 2);
  CHECK(recorder.observations().back().operation == "sidecar.missing");
  CHECK(recorder.observations().back().fields[0].second == "none");
  CHECK(recorder.observations().back().fields[1].second == "missing");
  CHECK(recorder.conformant());
}

TEST_CASE("conformance recorder accepts an errored analysis attempt whose "
          "artifact is present as sidecar.corrupt (round-2 critic P1-1b)") {
  // Before this fix, this was also hardcoded to "sidecar.publish" and
  // therefore always rejected. sidecar-operation-map.json declares
  // sidecar.corrupt -> MarkSidecarCorrupt -> sidecarState=corrupt for
  // exactly this outcome (an artifact was produced, but the overall attempt
  // errored), and CidxStorageLifecycle.tla's MarkSidecarCorrupt postcondition
  // sets sidecarQuality'="corrupt" and sidecarValidated'=FALSE
  // unconditionally -- this is a legal, schema-modelled outcome, not a
  // violation.
  FakeServices fake;
  fake.index_response =
      base_envelope("index", Status::Complete, "complete", "current");
  fake.index_response.artifacts.push_back({.kind = "semantic-index",
                                           .id = "core-artifact-1",
                                           .schema_version = 1,
                                           .catalog_version = 1,
                                           .catalog_hash = "test-hash",
                                           .generation = "generation-1"});
  fake.analysis_response =
      base_envelope("analysis", Status::Error, "unknown", "unverifiable");
  fake.analysis_response.artifacts.push_back({.kind = "analysis",
                                              .id = "corrupt-sidecar",
                                              .schema_version = 1,
                                              .catalog_version = 1,
                                              .catalog_hash = "test-hash",
                                              .generation = "generation-1"});

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
  recorder.analysis(AnalysisRequest{.action = AnalysisAction::execute,
                                    .rule = std::nullopt,
                                    .rules_file = std::nullopt,
                                    .export_directory = std::nullopt,
                                    .index = std::nullopt,
                                    .jobs = 1},
                    context);

  require_observation_count(recorder, 2);
  CHECK(recorder.observations().back().operation == "sidecar.corrupt");
  CHECK(recorder.observations().back().fields[0].second == "none");
  CHECK(recorder.observations().back().fields[1].second == "corrupt");
  CHECK(recorder.observations().back().fields[2].second == "corrupt");
  CHECK(recorder.observations().back().fields[3].second == "false");
  CHECK(recorder.conformant());
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
  // An empty trace is not vacuously conformant -- it means no in-scope call
  // was ever made, which is an explicit non-answer, not a pass.
  CHECK_FALSE(recorder.conformant());
}
