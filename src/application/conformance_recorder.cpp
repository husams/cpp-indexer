#include "application/conformance_recorder.hpp"

#include <algorithm>

namespace cidx::application {

protocol::ResultEnvelope
ConformanceRecorder::record(std::string operation,
                            protocol::ResultEnvelope envelope) const {
  ConformanceObservation observation{
      .operation = std::move(operation),
      .status = std::string(protocol::status_name(envelope.status)),
      .completenessState = envelope.completeness.state,
      .freshness = envelope.identity.freshness,
      .truncated = envelope.completeness.truncated,
      .artifactKinds = {},
      .diagnosticCodes = {},
  };
  observation.artifactKinds.reserve(envelope.artifacts.size());
  for (const auto &artifact : envelope.artifacts) {
    observation.artifactKinds.push_back(artifact.kind);
  }
  observation.diagnosticCodes.reserve(envelope.diagnostics.size());
  for (const auto &diagnostic : envelope.diagnostics) {
    observation.diagnosticCodes.push_back(diagnostic.code);
  }
  observations_.push_back(std::move(observation));
  return envelope;
}

protocol::ResultEnvelope
ConformanceRecorder::index(const IndexRequest &request,
                           ApplicationContext &context) const {
  return record("index.publish", delegate_.index(request, context));
}

protocol::ResultEnvelope
ConformanceRecorder::query(const QueryRequest &request,
                           ApplicationContext &context) const {
  return record("query.return", delegate_.query(request, context));
}

protocol::ResultEnvelope
ConformanceRecorder::analysis(const AnalysisRequest &request,
                              ApplicationContext &context) const {
  return record("sidecar.publish", delegate_.analysis(request, context));
}

protocol::ResultEnvelope
ConformanceRecorder::workspace(const WorkspaceRequest &request,
                               ApplicationContext &context) const {
  return record("workspace.import", delegate_.workspace(request, context));
}

protocol::ResultEnvelope
ConformanceRecorder::ast(const AstInspectionRequest &request,
                         ApplicationContext &context) const {
  return record("ast.inspect", delegate_.ast(request, context));
}

protocol::ResultEnvelope
ConformanceRecorder::diff(const DiffRequest &request,
                          ApplicationContext &context) const {
  return record("diff.compare", delegate_.diff(request, context));
}

protocol::ResultEnvelope
ConformanceRecorder::include(const IncludeRequest &request,
                             ApplicationContext &context) const {
  return record("include.plan", delegate_.include(request, context));
}

protocol::ResultEnvelope
ConformanceRecorder::refactor(const RefactoringRequest &request,
                              ApplicationContext &context) const {
  return record("refactor.plan", delegate_.refactor(request, context));
}

protocol::ResultEnvelope
ConformanceRecorder::proof(const ProofRequest &request,
                           ApplicationContext &context) const {
  return record("proof.execute", delegate_.proof(request, context));
}

bool ConformanceRecorder::conformant() const {
  return std::ranges::all_of(observations_, [](const auto &observation) {
    // CidxProtected.NoPartialPublication: publicationState = "current" =>
    // artifactState in {"published", "derived"}. An index-generation-
    // publication observation reporting a complete, current-freshness
    // result without any published artifact is exactly the shape that
    // predicate forbids.
    if (observation.operation != "index.publish") {
      return true;
    }
    const bool claims_published_current =
        observation.status == "complete" &&
        observation.completenessState == "complete" &&
        observation.freshness == "current";
    return !claims_published_current || !observation.artifactKinds.empty();
  });
}

} // namespace cidx::application
