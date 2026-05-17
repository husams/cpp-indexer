# ADR-9: Cross-repo schema versioning — refuse on mismatch; monotonic integer SCHEMA_VERSION

Status: accepted
Date: 2026-05-17
Resolves: PRD Q4, AC-M4-2 (implicit), AC-M6-6, AC-M6-7

## Context

PRD Q4 asks how to reconcile cross-repo resolution (Phase 5) when repos were indexed at different times with different schema versions. AC-M6-6 requires a `SchemaVersion` node in the graph. AC-M6-7 requires cpp-mcp to refuse on mismatch rather than return potentially wrong results. The same principle applies to Phase 5: if Repo A was indexed under schema v3 and Repo B under v4, the assumptions about edge kinds / node attributes do not hold and silent reconciliation would produce wrong `EXTERNAL_REF` edges.

## Decision

A single monotonically-increasing integer `SCHEMA_VERSION` baked into the binary at compile time. Strict refusal on mismatch.

### Constant

```rust
// src/schema/version.rs
pub const SCHEMA_VERSION: u32 = 1;  // bump in the same PR that changes nodes.rs / edges.rs
pub const SCHEMA_VERSION_TAG: &str = const_format::concatcp!("cxg-schema-v", SCHEMA_VERSION);
```

CI gate: a `tests/schema_version_bump.rs` integration test computes a Blake3 of `src/schema/{nodes,edges}.rs` and compares against a `tests/schema-baseline.txt` file. If the source files change without `SCHEMA_VERSION` being bumped (and `schema-baseline.txt` updated), the test fails. This forces the bump to be deliberate.

### SchemaVersion node

- One `SchemaVersion` node per indexed graph, written by Phase 4 at the start of every run. Attributes: `version: u32`, `tag: String`, `indexer_commit: String` (git rev of cpp-indexer at build time, baked via `build.rs`), `libclang_version: String`, `wrote_at: ISO8601`.
- Sink-level uniqueness: `MERGE (n:SchemaVersion {version: $version}) SET n.wrote_at = $now`.

### Phase 5 enforcement

- At Phase 5 start, query every indexed REPO node's associated `SchemaVersion` (one-to-one through a `WRITTEN_WITH_SCHEMA` edge written at Phase 4).
- If any two REPOs have different `version` values → **refuse**: exit non-zero (`cxg-resolve-cross-repo`) or job state `failed` (cxg-daemon) with diagnostic listing each `(repo_name, schema_version, indexer_commit)` tuple. AC-M4-2 implicit, AC-M6-7.
- No "reconcile" or "upgrade in place" path. The remediation documented in runbook.md is: re-index the older repo with the newer cxg-indexer binary, then re-run Phase 5.

### cpp-mcp interaction

- cpp-mcp reads the live graph's `SchemaVersion` on startup AND opportunistically per query session. If the graph's version != the version embedded in cpp-mcp's vendored `schema.txt`, cpp-mcp returns an error to the caller (mapped to AC-M6-7). See ADR-1 for the cpp-mcp boundary.

### Bump policy

- Any change to `NodeKind` or `EdgeKind` variants, or to a required attribute, requires bumping `SCHEMA_VERSION` by 1 in the same PR.
- Optional additive attributes (new nullable column in `attrs_json`) **may** be done without a bump if marked "additive, optional" in the PR description and noted in CHANGELOG. CI gate above enforces only that structural changes (`nodes.rs`, `edges.rs`) are paired with a bump.

## Alternatives considered

- **Semantic versioning string (`"1.2.3"`)**: rejected. Adds three-axis comparison logic with no benefit; cross-repo resolution either works or it doesn't, and we want a hard refusal. Integer suffices.
- **Reconcile-on-the-fly (auto-promote older-schema edges to newer)**: rejected. Inference about how a deprecated edge kind maps to a new one is exactly the silent-wrong-answer failure mode AC-M6-7 prohibits.
- **No version at all; trust producers**: rejected. CHARTER and AC-M6-7 explicitly require detection.
- **Per-node schema versioning**: rejected. The granularity is per-graph; per-node would multiply storage cost with no use case.

## Consequences

Positive:
- Mismatches are loud and explicit; no risk of silently wrong `EXTERNAL_REF` materialisation.
- Single integer is trivial to inspect (`cxg-index --version`, `GET /v1/status`, sink query).
- Forces deliberate schema evolution PRs.

Negative:
- Operators must re-index all repos after every schema bump if they want unified Phase 5. Documented as a known cost in runbook.
- The CI bump-gate produces friction; mitigated by `additive, optional` exception.

Follow-ups:
- After v1, evaluate a `compatible_with: Vec<u32>` field on the indexer (e.g., schema v3 indexer can read v2 staging files) if re-indexing cost becomes a real pain point.

Revisit if: a real operational scenario emerges where a repo cannot be re-indexed (e.g., source code no longer available) and stale-version reconciliation becomes necessary.

## References

- requirements.md AC-M4-2 (implicit through cross_repo_candidate handling), AC-M6-6, AC-M6-7
- PRD v1.1 Q4
- engineering plan v1.1 §Open questions (cross-repo schema versioning)
- ADR-1 (cpp-mcp boundary; the consumer side)
- Cognee tags: `task:cpp-indexer role:architect`
