#include "application/conformance_recorder.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "util/errors.hpp"

#ifndef CIDX_SPEC_TLA_CONFORMANCE_DIR
#define CIDX_SPEC_TLA_CONFORMANCE_DIR "spec/tla/conformance"
#endif

namespace cidx::application {

namespace {

std::string read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw CidxError("conformance schema: cannot open '" + path + "'");
  }
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::optional<std::string>
field_value(const ConformanceObservation &observation,
            const std::string &field) {
  for (const auto &[name, value] : observation.fields) {
    if (name == field) {
      return value;
    }
  }
  return std::nullopt;
}

// indexState/publicationState/artifactState are derived independently of one
// another -- indexState from completeness+freshness, artifactState from the
// artifact list -- so a defect that reports success without a published
// artifact produces an inconsistent pair the cross-check below can catch.
// This mirrors CidxProtected.NoPartialPublication exactly.
std::string derive_index_state(const protocol::ResultEnvelope &envelope) {
  if (envelope.status == protocol::Status::Error) {
    return "failed";
  }
  if (envelope.completeness.state == "complete" &&
      envelope.identity.freshness == "current") {
    return "current";
  }
  return "stale";
}

std::string derive_publication_state(const std::string &index_state) {
  if (index_state == "current") {
    return "current";
  }
  if (index_state == "failed") {
    return "none";
  }
  return "stale";
}

std::string derive_artifact_state(const protocol::ResultEnvelope &envelope) {
  return envelope.artifacts.empty() ? "none" : "published";
}

// sidecarQuality/sidecarValidated mirror PrepareSidecarArtifact's own
// `sidecarValidated' = (quality = "valid")`: a sidecar is only "valid" when
// the delegate reported both a complete result and current freshness --
// anything else (partial, stale, or otherwise unaccounted-for) is a real,
// schema-declared non-valid quality, not a silently-accepted default.
std::string derive_sidecar_quality(const protocol::ResultEnvelope &envelope) {
  if (envelope.completeness.state == "complete" &&
      envelope.identity.freshness == "current") {
    return "valid";
  }
  if (envelope.completeness.state == "partial") {
    return "partial";
  }
  return "corrupt";
}

// The one artifact PublishSidecar actually publishes has kind "analysis"
// (generated_result_protocol.hpp's kArtifactKinds) and a catalog_hash tied to
// the same generation the most recently recorded index.publish reported --
// not any nonempty artifact list. Checking `!artifacts.empty()` alone (as an
// earlier version of this recorder did) would accept a claimed publish
// backed by an unrelated artifact kind and an unrelated catalog, discarding
// generation identity/catalog provenance entirely -- exactly the shape a
// seeded defect would produce. identity.index stands in for
// CidxStorageLifecycle's `sidecarGeneration = currentGeneration`
// precondition: it is the one generation-identifying field ResultEnvelope
// actually carries end to end, alongside the artifact's own catalog_hash.
bool sidecar_artifact_matches_published_generation(
    const protocol::ResultEnvelope &envelope,
    const std::optional<ConformanceRecorder::PublishedGeneration>
        &published_generation) {
  if (envelope.artifacts.empty() || !published_generation.has_value()) {
    return false;
  }
  const protocol::ArtifactRef &artifact = envelope.artifacts.front();
  return artifact.kind == "analysis" && !artifact.catalog_hash.empty() &&
         artifact.catalog_hash == published_generation->catalog_hash &&
         envelope.identity.index == published_generation->identity_index;
}

bool has_valid_sidecar_artifact(
    const protocol::ResultEnvelope &envelope,
    const std::optional<ConformanceRecorder::PublishedGeneration>
        &published_generation) {
  return derive_sidecar_quality(envelope) == "valid" &&
         sidecar_artifact_matches_published_generation(envelope,
                                                       published_generation);
}

} // namespace

ConformanceRecorder
ConformanceRecorder::wrapping(const ApplicationServices &delegate) {
  const std::string dir = CIDX_SPEC_TLA_CONFORMANCE_DIR;
  ConformanceSchema index_query_schema =
      ConformanceSchema::parse(read_file(dir + "/operation-map.json"),
                               read_file(dir + "/observation-map.json"));
  ConformanceSchema sidecar_schema = ConformanceSchema::parse(
      read_file(dir + "/sidecar-operation-map.json"),
      read_file(dir + "/sidecar-observation-map.json"));
  return {delegate, std::move(index_query_schema), std::move(sidecar_schema)};
}

protocol::ResultEnvelope
ConformanceRecorder::index(const IndexRequest &request,
                           ApplicationContext &context) const {
  protocol::ResultEnvelope envelope = delegate_.index(request, context);
  if (request.action == IndexAction::update) {
    // Every update attempt is recorded, regardless of outcome -- silently
    // dropping the observation for a non-"current" result would mean the
    // recorded trace is only ever the subset of actions that already looked
    // legal, and conformant() replaying that filtered trace could never
    // catch what the filter itself dropped (a genuinely partial/failed
    // publish would simply vanish rather than being checked and rejected).
    // Each outcome gets the operation-map.json action that actually
    // describes it, so the schema-expectation check below stays meaningful
    // instead of being forced to accept every outcome as "index.publish".
    const std::string index_state = derive_index_state(envelope);
    const std::string publication_state = derive_publication_state(index_state);
    const std::string artifact_state = derive_artifact_state(envelope);
    if (index_state == "current") {
      observations_.push_back(ConformanceObservation{
          .operation = "index.publish",
          .fields = {{"indexState", index_state},
                     {"publicationState", publication_state},
                     {"artifactState", artifact_state}},
      });
      // Record the generation a later sidecar.publish must tie back to --
      // see PublishedGeneration's declaration for why identity.index +
      // catalog_hash are the two fields used. An index.publish with no
      // artifact at all (artifact_state == "none") still overwrites this
      // with an empty catalog_hash, so no subsequent sidecar can spuriously
      // match it.
      last_published_generation_ = PublishedGeneration{
          .catalog_hash = envelope.artifacts.empty()
                              ? std::string()
                              : envelope.artifacts.front().catalog_hash,
          .identity_index = envelope.identity.index,
      };
    } else if (index_state == "stale") {
      // operation-map.json: publication.interrupt -> InterruptPublication ->
      // publicationState=stale.
      observations_.push_back(ConformanceObservation{
          .operation = "publication.interrupt",
          .fields = {{"indexState", index_state},
                     {"publicationState", publication_state},
                     {"artifactState", artifact_state}},
      });
    } else {
      // operation-map.json: index.failure -> IndexFails ->
      // failureMode=index-failure.
      observations_.push_back(ConformanceObservation{
          .operation = "index.failure",
          .fields = {{"indexState", index_state},
                     {"publicationState", publication_state},
                     {"artifactState", artifact_state},
                     {"failureMode", "index-failure"}},
      });
    }
  }
  return envelope;
}

protocol::ResultEnvelope
ConformanceRecorder::query(const QueryRequest &request,
                           ApplicationContext &context) const {
  protocol::ResultEnvelope envelope = delegate_.query(request, context);
  if (envelope.status != protocol::Status::Error) {
    // CidxProtected.ReadOnlyQueries(queryWrites) == queryWrites = 0.
    // ResultEnvelope carries no write counter, so the observable proxy is
    // whether the context this call executed under has ANY write port
    // wired at all: DefaultApplicationServices::query() never references
    // write_ports(), so a correctly-scoped read-only context has none, and
    // a query executed with write capability present is exactly the seeded
    // "query write" defect this check exists to reject.
    const auto &write_ports = context.write_ports();
    const bool any_write_port =
        write_ports.workspace != nullptr || write_ports.source != nullptr ||
        write_ports.symbols != nullptr || write_ports.types != nullptr ||
        write_ports.facts != nullptr || write_ports.definitions != nullptr ||
        write_ports.includes != nullptr || write_ports.unit_of_work != nullptr;
    observations_.push_back(ConformanceObservation{
        .operation = "query.return",
        .fields = {{"queryState", "complete"},
                   {"queryWrites", any_write_port ? "1" : "0"}},
    });
  }
  return envelope;
}

protocol::ResultEnvelope
ConformanceRecorder::analysis(const AnalysisRequest &request,
                              ApplicationContext &context) const {
  protocol::ResultEnvelope envelope = delegate_.analysis(request, context);
  // Only a request whose action can plausibly produce a derived sidecar
  // artifact is a publish attempt at all -- `list` never is. The label is
  // derived from what was asked for and whether the call reported an
  // outright error; sidecarFilePublication/sidecarState are derived
  // independently from the artifact list, so a claimed publish with no
  // artifact is a real, catchable mismatch rather than a tautology.
  const bool publish_attempt =
      (request.action == AnalysisAction::execute ||
       request.action == AnalysisAction::export_facts) &&
      envelope.status != protocol::Status::Error;
  if (publish_attempt) {
    const std::string sidecar_quality = derive_sidecar_quality(envelope);
    const bool published =
        has_valid_sidecar_artifact(envelope, last_published_generation_);
    observations_.push_back(ConformanceObservation{
        .operation = "sidecar.publish",
        .fields = {{"sidecarFilePublication", published ? "current" : "none"},
                   {"sidecarState", published ? "current" : "missing"},
                   {"sidecarQuality", sidecar_quality},
                   {"sidecarValidated", published ? "true" : "false"}},
    });
  }
  return envelope;
}

protocol::ResultEnvelope
ConformanceRecorder::workspace(const WorkspaceRequest &request,
                               ApplicationContext &context) const {
  return delegate_.workspace(request, context);
}

protocol::ResultEnvelope
ConformanceRecorder::ast(const AstInspectionRequest &request,
                         ApplicationContext &context) const {
  return delegate_.ast(request, context);
}

protocol::ResultEnvelope
ConformanceRecorder::diff(const DiffRequest &request,
                          ApplicationContext &context) const {
  return delegate_.diff(request, context);
}

protocol::ResultEnvelope
ConformanceRecorder::include(const IncludeRequest &request,
                             ApplicationContext &context) const {
  return delegate_.include(request, context);
}

protocol::ResultEnvelope
ConformanceRecorder::refactor(const RefactoringRequest &request,
                              ApplicationContext &context) const {
  return delegate_.refactor(request, context);
}

protocol::ResultEnvelope
ConformanceRecorder::proof(const ProofRequest &request,
                           ApplicationContext &context) const {
  return delegate_.proof(request, context);
}

bool ConformanceRecorder::conformant() const {
  // An empty trace is not vacuously conformant: `std::ranges::all_of` over
  // an empty range is trivially true, and (before this check existed) that
  // silently rewarded a recording path that dropped every observation for
  // an outcome it didn't recognize. Every in-scope call now always records
  // something (see index()/query()/analysis() above), so an empty trace
  // here means no in-scope call was ever made at all -- an explicit,
  // checkable precondition, not a default pass.
  if (observations_.empty()) {
    return false;
  }

  // Per-observation schema/invariant checks (unordered): every recorded
  // field must be schema-allowed, every operation's declared expectation
  // must hold, and the two protected-invariant cross-checks apply.
  const bool per_observation_ok =
      std::ranges::all_of(observations_, [this](const auto &observation) {
        const bool is_sidecar = observation.operation.starts_with("sidecar.");
        const ConformanceSchema &schema =
            is_sidecar ? sidecar_schema_ : index_query_schema_;

        for (const auto &[field, value] : observation.fields) {
          if (!schema.is_allowed(field, value)) {
            return false;
          }
        }

        if (const auto expectation =
                schema.expectation_for(observation.operation)) {
          const auto actual = field_value(observation, expectation->field);
          if (!actual || *actual != expectation->value) {
            return false;
          }
        }

        if (observation.operation == "index.publish") {
          const auto publication_state =
              field_value(observation, "publicationState");
          if (publication_state && *publication_state == "current") {
            const auto index_state = field_value(observation, "indexState");
            const auto artifact_state =
                field_value(observation, "artifactState");
            if (!index_state || *index_state != "current") {
              return false;
            }
            if (!artifact_state || (*artifact_state != "published" &&
                                    *artifact_state != "derived")) {
              return false;
            }
          }
        }

        if (observation.operation == "query.return") {
          const auto query_writes = field_value(observation, "queryWrites");
          if (!query_writes || *query_writes != "0") {
            return false;
          }
        }

        return true;
      });
  if (!per_observation_ok) {
    return false;
  }

  // Ordered-trace check: CidxStorageConformance.tla's PublishSidecar action
  // requires corePublicationState = "current" -- reachable only after this
  // same session's StartOneTUUpdate -> ... -> PublishCoreGeneration sequence
  // has already run. tools/check-sidecar-conformance.sh's "illegal-order"
  // seed proves via real TLC replay that attempting PublishSidecar before
  // that sequence completes deadlocks (no legal step remains); this is the
  // same rule enforced here, directly against the recorded C++ call order,
  // rather than a per-observation check that ignores sequencing entirely.
  // (A literal per-trace TLC replay is not attempted here: CidxStorageLifecycle
  // models the core-then-sidecar sequence as 7 fine-grained actions with no
  // 1:1 correspondence to the 2 coarse-grained ApplicationServices calls
  // (index()/analysis()) this recorder observes, and tests/CMakeLists.txt's
  // "default" label is deliberately hermetic -- no Java/TLC dependency,
  // matching every other default-labeled test. check-sidecar-conformance.sh is
  // the TLC-backed verification of the underlying rule; this is its C++
  // enforcement.)
  bool index_published_so_far = false;
  for (const auto &observation : observations_) {
    if (observation.operation == "index.publish") {
      index_published_so_far = true;
    } else if (observation.operation == "sidecar.publish") {
      if (!index_published_so_far) {
        return false;
      }
    }
  }

  return true;
}

} // namespace cidx::application
