#include "extract/artifact.hpp"

#include "extract/plan_identity.hpp"
// SqliteDb/SqliteStmt come transitively via extract/artifact.hpp ->
// storage/artifacts.hpp; no direct include of storage/sqlite.hpp here.
#include "catalogs/generated_catalog.hpp"
#include "util/hashing.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>

namespace cidx::extract {

// The DB schema version of the `extension`/`meta` table pair this file
// writes (i.e. the shape "cidx-extension/v1" describes), not the
// ExtractionPlan IR's own schema_version (plan_ir.hpp's
// kExtractionPlanSchemaVersion) -- an unrelated axis: a plan's schema can
// change independently of the published artifact's table layout.
constexpr int kExtensionArtifactSchemaVersion = 1;

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

// Writes the producer-owned `meta` key/value table the platform's
// cidx::analysis::ExtensionFactProvider (src/analysis/facts.cpp) actually
// reads from -- distinct from both the platform's own auto-populated
// `artifact_meta` envelope table (storage/artifacts.cpp's write_envelope())
// and this file's own `extension_meta` diagnostics table. Every other
// artifact producer (e.g. src/astgraph/astgraph.cpp's `meta` table) follows
// the same pattern: `artifact_meta` is the platform's generic envelope,
// `meta` is the producer's own fact-schema metadata that only ITS reader
// knows how to interpret. ExtensionFactProvider::snapshot() reads
// workspace_identity/tu_identity/applicability with `artifact_meta_value(...)
// .value_or(meta_value(...))` -- and because `value_or`'s argument is
// evaluated unconditionally in C++, that `meta_value()` call runs (and
// throws "no such table: meta") even when `artifact_meta` already has the
// answer. schema_version/catalog_hash are read from `meta` ONLY, with no
// `artifact_meta` fallback at all. Without this table, publishing succeeds
// but the artifact can never be read back through the required adapter.
void insert_platform_meta(SqliteDb &db, const ExecutionReport &report) {
  db.exec("CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL)");
  auto insert = [&db](std::string_view key, std::string_view value) {
    auto statement = db.prepare("INSERT INTO meta(key, value) VALUES (?, ?)");
    statement.bind(1, key);
    statement.bind(2, value);
    statement.step_done();
  };
  insert("workspace_identity", report.workspace_identity);
  insert("tu_identity", report.tu_identity);
  // Each execution runs against exactly one pinned translation unit (a
  // single clang::ASTContext) -- "translation-unit", not the platform's
  // "workspace" default, mirrors AstgraphFactProvider's own default for the
  // same reason.
  insert("applicability", "translation-unit");
  insert("schema_version", std::to_string(kExtensionArtifactSchemaVersion));
  insert("catalog_version", std::to_string(cidx::catalog::kCatalogVersion));
  insert("catalog_hash", cidx::catalog::kCatalogHash);
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
  if (!report.publication_state_is_intact()) {
    throw ExtensionPublicationError(
        "cannot publish an execution report whose publication state was "
        "mutated after execution");
  }
  if (!report.descriptor_backed()) {
    throw ExtensionPublicationError(
        "cannot publish an execution report that did not run from an HSE-61 "
        "TranslationUnitDescriptor through the HSE-63 pass registry");
  }
  if (std::ranges::any_of(report.rule_stats,
                          [](const RuleExecutionStats &stats) {
                            return stats.budget_exhausted;
                          })) {
    throw ExtensionPublicationError(
        "cannot publish a partial artifact from a budget-exhausted execution");
  }
  // Fail closed: only a PINNED execution from the descriptor-backed overload
  // may be published. An empty identity has no verifiable link to a workspace
  // snapshot or translation-unit configuration.
  if (report.workspace_identity.empty() || report.tu_identity.empty()) {
    throw ExtensionPublicationError(
        "cannot publish an execution report with no pinned workspace/TU "
        "identity -- execute_plan() must be called with a complete "
        "TranslationUnitDescriptor to publish its result");
  }
  // Fail closed against publishing `plan`'s metadata (plan_id, rule
  // producer_package/version, ...) alongside a DIFFERENT plan's execution
  // report/facts: verify_fact_provenance() below only checks that the sink
  // and report agree with EACH OTHER, which a caller could satisfy by
  // passing a `plan` argument that never actually produced either of them.
  // Recomputing plan_hash(plan) here and requiring it match report.plan_hash
  // ties the published metadata to the SAME plan execute_plan() actually
  // ran.
  if (plan_hash(plan) != report.plan_hash) {
    throw ExtensionPublicationError(
        "the supplied ExtractionPlan does not match the plan that produced "
        "this execution report -- plan_hash(plan) != report.plan_hash");
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
  spec.truncation = ArtifactTruncation::none;
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
        // `namespace` is part of the identity of a fact, not just metadata:
        // two rules publishing under DIFFERENT namespaces (e.g. two
        // consumer packages independently emitting the same relation
        // between the same endpoints) are distinct facts and must not
        // collide. Without `namespace` in the key, INSERT OR IGNORE
        // silently drops the second insert as a "duplicate" of the first.
        "PRIMARY KEY(fact_kind, namespace, rule_id, kind_name, identity, "
        "secondary))");
    db.exec("CREATE TABLE extension_meta(key TEXT PRIMARY KEY, value TEXT "
            "NOT NULL)");
    insert_platform_meta(db, report);

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
