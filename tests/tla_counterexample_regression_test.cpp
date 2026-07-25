// C++ regression scenario seeded from a TLA+ counterexample (HSE-89).
//
// spec/tla/proofs/CidxResultProof.tla proves TrustedOutcomeInvariant --
// "a result marked proved-under-assumptions always carries a nonempty
// assumption set" -- inductively for CidxResult.tla, for every constant
// instantiation, not only the one finite instance TLC checks. Weakening
// ProveUnderAssumptions to stop recording that assumption set (so it stays
// empty) makes both TLC and TLAPS reject the model; the counterexample TLC
// produces for that seed is exported, byte-for-byte, at
// spec/tla/counterexamples/golden/trusted-outcome-violation.json by
// spec/tla/tools/export-counterexample.sh --demo.
//
// The C++ result protocol (query/result_protocol.hpp) does not have a
// literal "assumptions" field or a "proved-under-assumptions" enumerator --
// Status::Conditional plus a required "unknown"/"missing_evidence"
// diagnostic is its conceptual analog for a result that depends on
// unresolved assumptions (see generated_result_protocol.hpp's
// kDiagnosticStatusRules). This test is not a formal conformance replay
// (spec/tla/conformance/ owns that, over CidxBehavior/CidxStorageLifecycle);
// it is a regression scenario carrying the *same shape* as the exported
// counterexample -- an "assumptions empty" result -- into the real C++
// validation path, so the production code that would encode this class of
// claim is proven to reject it too.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "query/result_protocol.hpp"

namespace {

cidx::protocol::ResultEnvelope base_envelope() {
  using namespace cidx::protocol;
  ResultEnvelope envelope;
  envelope.operation = "query";
  envelope.identity = {.workspace = "workspace://demo",
                       .index = "semantic-index://demo",
                       .fact_sets = {"symbols", "files"},
                       .freshness = "current",
                       .source_revision = "git:abc123",
                       .source_fingerprint = "sha256:source"};
  envelope.producer = {.package = "cidx",
                       .version = "0.53.0",
                       .backend = "cpp",
                       .schema_version = 1};
  return envelope;
}

} // namespace

TEST_CASE("seeded counterexample regression: an assumptions-empty "
          "conditional result is rejected, matching TrustedOutcomeInvariant") {
  using namespace cidx::protocol;

  // Mirrors state 4 of counterexamples/golden/trusted-outcome-violation.json:
  // status = "proved-under-assumptions", assumptions = {} (empty). No
  // diagnostic records why the result is conditional -- the real protocol's
  // required_any rule for Status::Conditional has nothing to satisfy it.
  ResultEnvelope conditional_without_evidence = base_envelope();
  conditional_without_evidence.status = Status::Conditional;
  conditional_without_evidence.completeness = {.state = "unknown",
                                               .truncated = false,
                                               .stale = false,
                                               .budget = std::nullopt};
  conditional_without_evidence.diagnostics.clear();
  CHECK_FALSE(conditional_without_evidence.valid());

  // The invariant's "healthy" branch: assumptions = {EvidenceId} (nonempty)
  // corresponds to carrying the required diagnostic that explains what the
  // result is conditional on.
  ResultEnvelope conditional_with_evidence = conditional_without_evidence;
  conditional_with_evidence.diagnostics.push_back(
      {.code = "missing_evidence",
       .severity = "error",
       .message = "conditional on unresolved assumptions",
       .next_action = std::nullopt});
  CHECK(conditional_with_evidence.valid());

  // Forbidden codes for Status::Conditional (kDiagnosticStatusRules) must
  // still reject the envelope even once a required code is present --
  // otherwise a partially-fixed regression could pass by accident.
  ResultEnvelope conditional_with_forbidden_code = conditional_with_evidence;
  conditional_with_forbidden_code.diagnostics.push_back(
      {.code = "policy_refuted",
       .severity = "error",
       .message = "also refuted",
       .next_action = std::nullopt});
  CHECK_FALSE(conditional_with_forbidden_code.valid());
}
