// Publishes an ExtractionPlan execution's extension facts as a
// content-addressed artifact registered with the persistence layer's
// artifact store (storage/artifacts.hpp), the same mechanism HSE-66's
// AnalysisPublisher uses. Conforms to the "extension:"-kind artifact
// contract storage/artifacts.cpp's supported_contract()/
// required_relations() already declare: artifact_schema
// "cidx-extension/v1" and exactly one required relation, "extension" (a
// fact_kind-discriminated table covering all four fact families) plus a
// small extension_meta table. It is queryable through the existing
// cidx::analysis::ExtensionFactProvider (src/analysis/facts.hpp) without any
// new provider code -- that provider already reads an arbitrary artifact's
// non-meta tables as relations.
#pragma once

#include "extract/engine.hpp"
#include "extract/extension_facts.hpp"
#include "extract/plan_ir.hpp"
// storage/artifacts.hpp already pulls in <filesystem> and storage/sqlite.hpp
// transitively; this header does not name either directly (mirrors
// analysis/runner.hpp's treatment of the same dependency).
#include "storage/artifacts.hpp"

#include <stdexcept>
#include <string>

namespace cidx {
class Storage;
}

namespace cidx::extract {

class ExtensionPublicationError final : public std::runtime_error {
public:
  explicit ExtensionPublicationError(const std::string &message);
};

// Deliberately does NOT carry its own workspace_identity/tu_identity: an
// independently-supplied identity here could disagree with what the
// execution actually ran against (and did in the reviewed version of this
// file). publish_extension_artifact() reads those identities from
// `ExecutionReport` instead, so there is exactly one path from
// execute_plan()'s ExecutionInput to the published artifact's metadata.
struct PublicationRequest {
  std::filesystem::path artifact_root;
  // A package-qualified namespace (e.g. "banking.audit"); becomes part of
  // the artifact's logical_id. Must not be "core" or "core.*".
  std::string namespace_name;
};

struct ExtensionPublication {
  std::string logical_id;
  std::string content_hash;
  std::string relative_path;
  // == ExecutionReport::artifact_identity: the identity of THIS execution
  // (plan + pinned workspace/TU/source input), stamped as the artifact's
  // configuration_identity.
  std::string artifact_identity;
  std::vector<std::string> exposed_relations;
};

// `report` and `sink` must come from the same execute_plan() call. This is
// verified, not assumed: every fact in `sink` must carry the same
// provenance.plan_hash/artifact_identity as `report`, and `report`'s pinned
// workspace_identity/tu_identity (populated from the ExecutionInput passed
// to execute_plan()) must both be non-empty -- an ad hoc/test execution
// that leaves them empty cannot be published, only pinned executions can.
// Canonicalizes `sink` itself (non-const) so the published bytes are always
// deterministic regardless of whether the caller already did so. Throws
// ExtensionPublicationError if the namespace is invalid, the identities are
// unpinned, the report has no successfully executed rules, or any fact's
// provenance does not match this report.
[[nodiscard]] ExtensionPublication
publish_extension_artifact(Storage &storage, const PublicationRequest &request,
                           const ExtractionPlan &plan,
                           const ExecutionReport &report,
                           InMemoryExtensionFactSink &sink);

} // namespace cidx::extract
