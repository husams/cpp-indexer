#include "extract/artifact.hpp"

// SqliteDb/SqliteStmt come transitively via extract/artifact.hpp ->
// storage/artifacts.hpp; no direct include of storage/sqlite.hpp here.
#include "util/hashing.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>

namespace cidx::extract {

ExtensionPublicationError::ExtensionPublicationError(const std::string &message)
    : std::runtime_error(message) {}

namespace {

bool valid_publication_namespace(std::string_view value) {
  if (value.empty() || value == "core" || value.starts_with("core.")) {
    return false;
  }
  if (std::isalpha(static_cast<unsigned char>(value.front())) == 0) {
    return false;
  }
  return std::ranges::all_of(value, [](char character) {
    return std::isalnum(static_cast<unsigned char>(character)) != 0 ||
           character == '_' || character == '-' || character == '.';
  });
}

ArtifactCompleteness completeness_rollup(const ExtractionPlan &plan,
                                         const ExecutionReport &report) {
  const bool every_rule_declared_complete =
      std::ranges::all_of(plan.rules, [](const ExtractionRule &rule) {
        return rule.completeness == DeclaredCompleteness::complete;
      });
  const bool any_budget_exhausted = std::ranges::any_of(
      report.rule_stats,
      [](const RuleExecutionStats &stats) { return stats.budget_exhausted; });
  if (every_rule_declared_complete && !any_budget_exhausted &&
      report.diagnostics.empty()) {
    return ArtifactCompleteness::complete;
  }
  return ArtifactCompleteness::partial;
}

void insert_meta(SqliteDb &db, std::string_view key, std::string_view value) {
  auto statement =
      db.prepare("INSERT INTO extension_meta(key, value) VALUES (?, ?)");
  statement.bind(1, key);
  statement.bind(2, value);
  statement.step_done();
}

bool provenance_matches(const ExtensionProvenance &provenance,
                        const ExecutionReport &report) {
  return provenance.plan_hash == report.plan_hash &&
         provenance.artifact_identity == report.artifact_identity;
}

// Verifies every fact in `sink` actually came from THIS `report`'s
// execution (same plan_hash and artifact_identity), rather than trusting
// the caller to have passed a matching report/sink pair. Guards against
// publishing a sink left over from a different plan or a different TU.
void verify_fact_provenance(const InMemoryExtensionFactSink &sink,
                            const ExecutionReport &report) {
  const bool all_match =
      std::ranges::all_of(sink.nodes(),
                          [&](const auto &fact) {
                            return provenance_matches(fact.provenance, report);
                          }) &&
      std::ranges::all_of(sink.relations(),
                          [&](const auto &fact) {
                            return provenance_matches(fact.provenance, report);
                          }) &&
      std::ranges::all_of(sink.attributes(),
                          [&](const auto &fact) {
                            return provenance_matches(fact.provenance, report);
                          }) &&
      std::ranges::all_of(sink.unknowns(), [&](const auto &fact) {
        return provenance_matches(fact.provenance, report);
      });
  if (!all_match) {
    throw ExtensionPublicationError(
        "fact provenance does not match this execution report -- sink and "
        "report must come from the same execute_plan() call");
  }
}

void insert_row(SqliteDb &db, std::string_view fact_kind,
                std::string_view namespace_name, std::string_view rule_id,
                std::string_view kind_name, std::string_view identity,
                std::string_view secondary,
                const ExtensionProvenance &provenance,
                const ExtensionEvidence &evidence) {
  auto statement = db.prepare(
      "INSERT OR IGNORE INTO extension(fact_kind, namespace, rule_id, "
      "kind_name, identity, secondary, completeness, producer_package, "
      "producer_version, evidence_file, evidence_line, evidence_col) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
  statement.bind(1, fact_kind);
  statement.bind(2, namespace_name);
  statement.bind(3, rule_id);
  statement.bind(4, kind_name);
  statement.bind(5, identity);
  statement.bind(6, secondary);
  statement.bind(7, std::string_view(to_string(provenance.completeness)));
  statement.bind(8, std::string_view(provenance.producer_package));
  statement.bind(9, static_cast<std::int64_t>(provenance.producer_version));
  statement.bind(10, std::string_view(evidence.file));
  statement.bind(11, evidence.line);
  statement.bind(12, evidence.col);
  statement.step_done();
}

} // namespace

ExtensionPublication
publish_extension_artifact(Storage &storage, const PublicationRequest &request,
                           const ExtractionPlan &plan,
                           const ExecutionReport &report,
                           InMemoryExtensionFactSink &sink) {
  if (!valid_publication_namespace(request.namespace_name)) {
    throw ExtensionPublicationError(
        "extension publication namespace is not allowed: " +
        request.namespace_name);
  }
  if (report.rule_stats.empty()) {
    throw ExtensionPublicationError(
        "cannot publish an execution report with no executed rules");
  }
  // Fail closed: only a PINNED execution (a real workspace/TU identity
  // supplied to execute_plan()'s ExecutionInput) may be published. An ad
  // hoc/test execution that left both empty produces facts with no
  // verifiable link to a workspace snapshot or TU descriptor, so it cannot
  // become a registered artifact.
  if (report.workspace_identity.empty() || report.tu_identity.empty()) {
    throw ExtensionPublicationError(
        "cannot publish an execution report with no pinned workspace/TU "
        "identity -- execute_plan() must be called with a non-empty "
        "ExecutionInput to publish its result");
  }
  verify_fact_provenance(sink, report);
  sink.canonicalize();

  // Conforms to the artifact contract storage/artifacts.cpp's
  // supported_contract()/required_relations() already declare for
  // kind.starts_with("extension:"): artifact_schema "cidx-extension/v1",
  // producer_version/engine_version prefixes, and exactly one required
  // relation named "extension". A single relation with a fact_kind
  // discriminator column is still fully queryable (filter by fact_kind) and
  // keeps this new artifact type consistent with the platform's existing
  // reader validation instead of inventing a parallel per-family-table shape.
  ArtifactSpec spec;
  spec.logical_id = "extension:" + request.namespace_name;
  spec.kind = spec.logical_id;
  spec.artifact_schema = "cidx-extension/v1";
  spec.producer_version = "cidx-extension 1";
  spec.engine_version = "cidx 1";
  // Derived from the report (already verified non-empty above), not an
  // independently-supplied request field -- this closes the gap where a
  // caller could publish under a workspace/TU identity that disagrees with
  // what execute_plan() actually pinned via ExecutionInput.
  spec.workspace_identity = report.workspace_identity;
  spec.tu_identity = report.tu_identity;
  // The identity of THIS execution (plan + pinned workspace/TU/source
  // input) -- fixes the review finding that an artifact's identity depended
  // only on the plan, never on what it actually ran against.
  spec.configuration_identity = report.artifact_identity;
  spec.input_fact_set_identity = report.plan_hash;
  spec.completeness = completeness_rollup(plan, report);
  const bool any_budget_exhausted =
      std::ranges::any_of(report.rule_stats, [](const RuleExecutionStats &s) {
        return s.budget_exhausted;
      });
  spec.truncation = any_budget_exhausted ? ArtifactTruncation::truncated
                                         : ArtifactTruncation::none;
  spec.trust = ArtifactTrust::producer_verified;
  // ArtifactSpec::evidence is validated against a fixed platform vocabulary
  // (source/derived/inferred/runtime/assumption/proof); extension facts are
  // computed FROM source via a matcher, not raw source text, so "derived"
  // is the correct tier here. The distinct "extension-analysis" evidence
  // class from HSE-65's Python package SDK is a package-result concept, not
  // this artifact-metadata field; ExtensionProvenance already carries the
  // per-fact extension/core distinction independent of this field.
  spec.evidence = "derived";
  // cidx::sha256_hex() returns "sha256:<64 hex chars>"; strip the
  // algorithm-tag prefix before using it as an attachment name fragment --
  // valid_attachment_name() forbids ':' (mirrors AnalysisPublisher::publish
  // in src/analysis/runner.cpp).
  const std::string logical_hash = cidx::sha256_hex(spec.logical_id);
  constexpr std::string_view kShaPrefix = "sha256:";
  const std::size_t hash_offset =
      logical_hash.starts_with(kShaPrefix) ? kShaPrefix.size() : 0;
  spec.attachment_name = "extract_" + logical_hash.substr(hash_offset, 16);
  spec.exposed_relations = {"extension"};

  ArtifactStore artifacts(storage, request.artifact_root);
  const ArtifactRecord record = artifacts.publish(spec, [&](SqliteDb &db) {
    db.exec(
        "CREATE TABLE extension(fact_kind TEXT NOT NULL, "
        "namespace TEXT NOT NULL, rule_id TEXT NOT NULL, "
        "kind_name TEXT NOT NULL, identity TEXT NOT NULL, "
        "secondary TEXT NOT NULL, completeness TEXT NOT NULL, "
        "producer_package TEXT NOT NULL, producer_version INTEGER NOT NULL, "
        "evidence_file TEXT NOT NULL, evidence_line INTEGER NOT NULL, "
        "evidence_col INTEGER NOT NULL, "
        "PRIMARY KEY(fact_kind, rule_id, kind_name, identity, secondary))");
    db.exec("CREATE TABLE extension_meta(key TEXT PRIMARY KEY, value TEXT "
            "NOT NULL)");

    insert_meta(db, "namespace", request.namespace_name);
    insert_meta(db, "plan_id", plan.plan_id);
    insert_meta(db, "plan_hash", report.plan_hash);
    insert_meta(db, "artifact_identity", report.artifact_identity);
    insert_meta(db, "rule_count", std::to_string(report.rule_stats.size()));
    insert_meta(db, "diagnostic_count",
                std::to_string(report.diagnostics.size()));

    for (const auto &fact : sink.nodes()) {
      insert_row(db, "node", fact.namespace_name, fact.provenance.rule_id,
                 fact.node_kind, fact.identity, "", fact.provenance,
                 fact.evidence);
    }
    for (const auto &fact : sink.relations()) {
      insert_row(db, "relation", fact.namespace_name, fact.provenance.rule_id,
                 fact.relation_kind, fact.from_identity, fact.to_identity,
                 fact.provenance, fact.evidence);
    }
    for (const auto &fact : sink.attributes()) {
      insert_row(db, "attribute", fact.namespace_name, fact.provenance.rule_id,
                 fact.attribute_name, fact.identity, fact.value,
                 fact.provenance, fact.evidence);
    }
    for (const auto &fact : sink.unknowns()) {
      insert_row(db, "unknown", fact.namespace_name, fact.provenance.rule_id,
                 fact.reason_code, fact.identity, "", fact.provenance,
                 fact.evidence);
    }
  });

  return ExtensionPublication{.logical_id = record.spec.logical_id,
                              .content_hash = record.content_hash,
                              .relative_path = record.relative_path,
                              .artifact_identity = report.artifact_identity,
                              .exposed_relations =
                                  record.spec.exposed_relations};
}

} // namespace cidx::extract
