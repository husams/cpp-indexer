// Instrumentation that maps real CIDX application-service actions and
// observations onto the vocabulary spec/tla/conformance/operation-map.json,
// observation-map.json, and sidecar-operation-map.json already use for
// abstract TLA+ states/actions (HSE-89). This is purely additive: it wraps
// an existing ApplicationServices implementation and never changes what it
// does; it only records what happened, in the shared vocabulary, and
// replays those records against the checked-in schema files (not an
// invented parallel vocabulary -- see conformance_schema.hpp).
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "application/conformance_schema.hpp"
#include "application/services.hpp"

namespace cidx::application {

// A recorded call, in the vocabulary the schema files declare. `operation`
// is empty when the call is outside this recorder's declared scope (only
// index-generation publication, QueryPlan read-only execution, and sidecar
// publication are covered -- the other six ApplicationServices methods are
// delegated but never claimed as conformance-checked, since operation-map.json
// has no corresponding actions for them). `fields` holds only field=value
// pairs actually derived from the real ResultEnvelope/ApplicationContext.
struct ConformanceObservation {
  std::string operation;
  std::vector<std::pair<std::string, std::string>> fields;
};

// Wraps a real ApplicationServices implementation, delegates every call
// unchanged, and records one ConformanceObservation per in-scope call.
// `conformant()` replays the recorded trace against the declared schema:
// every recorded value must be one the schema allows for that field, every
// operation's schema-declared expectation must hold, and the two protected
// invariants this recorder can evaluate from ResultEnvelope/ApplicationContext
// data alone -- CidxProtected.NoPartialPublication (index-generation
// publication) and CidxProtected.ReadOnlyQueries (QueryPlan execution) --
// must hold. See spec/tla/ASSURANCE.md for what this instrumentation does
// and does not prove, and tests/conformance_recorder_test.cpp for the seeded
// regressions this is meant to reject.
class ConformanceRecorder final : public ApplicationServices {
public:
  ConformanceRecorder(const ApplicationServices &delegate,
                      ConformanceSchema index_query_schema,
                      ConformanceSchema sidecar_schema)
      : delegate_(delegate), index_query_schema_(std::move(index_query_schema)),
        sidecar_schema_(std::move(sidecar_schema)) {}

  // Reads the checked-in schema files from spec/tla/conformance/ via the
  // CIDX_SPEC_TLA_CONFORMANCE_DIR compile definition (CMakeLists.txt), the
  // same pattern src/ui/assets.cpp uses for CIDX_UI_ASSET_DIR.
  static ConformanceRecorder wrapping(const ApplicationServices &delegate);
  // delegate_ is a reference member (see below): binding it to a temporary
  // would leave a dangling reference the moment the full expression that
  // constructed the temporary ends. Deleting the rvalue overload makes that
  // a compile error instead of a silent use-after-free the first time this
  // is wired into a call site that constructs its ApplicationServices inline
  // (senior-developer acceptance-review finding, round 3).
  static ConformanceRecorder wrapping(ApplicationServices &&delegate) = delete;

  protocol::ResultEnvelope index(const IndexRequest &request,
                                 ApplicationContext &context) const override;
  protocol::ResultEnvelope query(const QueryRequest &request,
                                 ApplicationContext &context) const override;
  protocol::ResultEnvelope analysis(const AnalysisRequest &request,
                                    ApplicationContext &context) const override;
  protocol::ResultEnvelope
  workspace(const WorkspaceRequest &request,
            ApplicationContext &context) const override;
  protocol::ResultEnvelope ast(const AstInspectionRequest &request,
                               ApplicationContext &context) const override;
  protocol::ResultEnvelope diff(const DiffRequest &request,
                                ApplicationContext &context) const override;
  protocol::ResultEnvelope include(const IncludeRequest &request,
                                   ApplicationContext &context) const override;
  protocol::ResultEnvelope refactor(const RefactoringRequest &request,
                                    ApplicationContext &context) const override;
  protocol::ResultEnvelope proof(const ProofRequest &request,
                                 ApplicationContext &context) const override;

  [[nodiscard]] const std::vector<ConformanceObservation> &
  observations() const {
    return observations_;
  }

  [[nodiscard]] bool conformant() const;

  // The generation/catalog/index provenance a legitimate sidecar.publish must
  // tie back to. ArtifactRef::generation is the shared generation token;
  // artifact IDs cannot be used because core and sidecar IDs identify
  // different artifacts. Catalog hash and index identity are retained as
  // secondary checks. Public (not an implementation-hiding concern) so the
  // free helper functions in conformance_recorder.cpp's anonymous namespace
  // can take it by const-reference without needing friendship.
  struct PublishedGeneration {
    std::string generation;
    std::string catalog_hash;
    std::string identity_index;
  };

private:
  const ApplicationServices &delegate_;
  ConformanceSchema index_query_schema_;
  ConformanceSchema sidecar_schema_;
  mutable std::vector<ConformanceObservation> observations_;
  mutable std::optional<PublishedGeneration> last_published_generation_;
};

} // namespace cidx::application
