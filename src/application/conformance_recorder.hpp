// Instrumentation that maps real CIDX application-service actions and
// observations onto the vocabulary spec/tla/conformance/operation-map.json,
// observation-map.json, and sidecar-operation-map.json already use for
// abstract TLA+ states/actions (HSE-89). This is purely additive: it wraps
// an existing ApplicationServices implementation and never changes what it
// does; it only records what happened, in the shared vocabulary.
#pragma once

#include <string>
#include <vector>

#include "application/services.hpp"

namespace cidx::application {

struct ConformanceObservation {
  std::string operation;
  std::string status;
  std::string completenessState;
  std::string freshness;
  bool truncated = false;
  std::vector<std::string> artifactKinds;
  std::vector<std::string> diagnosticCodes;
};

// Wraps a real ApplicationServices implementation, delegates every call
// unchanged, and records one ConformanceObservation per call translated from
// the returned protocol::ResultEnvelope. `conformant()` checks the recorded
// observations against the one protected invariant expressible from the
// envelope alone without a live storage/query read port: CidxProtected's
// NoPartialPublication -- an index-generation-publication observation may
// never report a complete, current-freshness result without at least one
// published artifact. See spec/tla/ASSURANCE.md for what this instrumentation
// does and does not prove, and tests/conformance_recorder_test.cpp for the
// seeded-defect regression this is meant to reject.
class ConformanceRecorder final : public ApplicationServices {
public:
  explicit ConformanceRecorder(const ApplicationServices &delegate)
      : delegate_(delegate) {}

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

private:
  protocol::ResultEnvelope record(std::string operation,
                                  protocol::ResultEnvelope envelope) const;

  const ApplicationServices &delegate_;
  mutable std::vector<ConformanceObservation> observations_;
};

} // namespace cidx::application
