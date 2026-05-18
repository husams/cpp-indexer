---
story: S40, S41, S42, S43, S44, S45, S46
run_id: cpp-indexer-m8-v2
author: developer
date: 2026-05-18
---

# M8 Implementation Notes (cumulative through S44 + S45)

See per-story logs in `logs/developer-<slug>.md` for full detail.

## S44 — Neo4j native property writes + covering indexes

### Files changed

- `src/sink/neo4j.rs` — 4 new index constants; `ensure_indexes()` extended to 6 with error-suppression for `EquivalentSchemaRuleAlreadyExists`; `CQL_MERGE_NODES` +10 SET clauses; `CQL_MERGE_EDGES` +2 SET clauses; `opt_str_to_bolt`, `opt_bool_to_bolt`, `structured_list_to_json_bolt` helpers; `node_to_bolt` / `edge_to_bolt` extended; `pub fn graph_handle()` added; 14 new unit tests
- `tests/integration/neo4j_indexes.rs` — new gated integration test (4 `#[ignore]` tests)
- `Cargo.toml` — registered `neo4j_indexes` test

### Exit-gate results

```
cargo fmt --all -- --check            → OK
cargo clippy --all-targets --all-features -- -D warnings  → OK
cargo test --lib sink::neo4j          → 31 passed; 0 failed
cargo test --all-targets --features test-mock → all passed; 0 failed
CPP_INDEXER_LIVE_NEO4J=1 NEO4J_URI=bolt://192.168.1.200:7687 \
  cargo test --test neo4j_indexes -- --ignored --nocapture
  → 4 passed; 0 failed; finished in 8.73s
```

### Deviations from plan.md / ADR-14

1. **[sr-dev] ADR-14 `List<Map>` rejected by Neo4j Community**: ADR-14 assumes Community supports `List<Map>` node properties. In practice, Neo4j Community 2025.12.1 returns `Neo.ClientError.Statement.TypeError`. Mitigation: `structured_list_to_json_bolt` serializes `params`, `template_params`, `template_args` as JSON strings. ADR-14 revision required.
2. **Test pool size 8 (not 2)**: `make_sink()` in integration test uses `sessions: Some(8)`. Initial value of 2 caused infinite hang because `DetachedRowStream` instances held pool connections until dropped; final `reset()` could never acquire a connection. Explicit `drop(stream)` / `drop(estream)` added + pool bumped for defense-in-depth.
3. **EXPLAIN gate gracefully skipped**: Dev cluster does not expose port 7474. `neo4j_indexes_explain_uses_node_index_seek` logs a warning and passes; index existence is confirmed via SHOW INDEXES (Bolt).

### Follow-ups

- `@sr-dev` — ADR-14 revision: `List<Map>` Community incompatibility + JSON string strategy
- `@sr-dev` — EXPLAIN gate: enable assertion when HTTP port 7474 is accessible
- `@sr-dev` — Document `sessions` = `max_connections` mapping in Neo4jSinkConfig; `DetachedRowStream` holds pool connections

### References

- plan.md lines 202-237
- design.md §Phase 4
- adr-14.md, adr-15.md
- scenarios.md @AC-S44-1..5

## S45 — IndraDB native property writes + index_property parity

### Files changed

- `src/sink/indradb.rs` — 12 property-name constants added; `json_value<T>` helper; `ensure_indexes` extended with 4 M8 `index_property` calls; `write_nodes` emits `BulkInsertItem::VertexProperty` for each `Option::Some` M8 field; `write_edges` emits optional edge property items for association_type fields; chunking updated to worst-case 17 nodes / 4 edges; unit tests updated and extended
- `src/sink/neo4j.rs` — lint-only: removed 3 unused imports (`BoltList`, `BoltMap`, `BoltString`) introduced by S44; no logic change
- `tests/integration/indradb_properties.rs` — new gated integration test (4 `#[ignore]` tests)
- `tests/integration/sink_parity.rs` — new gated cross-sink parity test (2 `#[ignore]` tests)
- `Cargo.toml` — registered both new test files

### Exit-gate results

```
cargo build --all-targets              → OK
cargo clippy --all-targets -- -D warnings → OK (0 errors, 0 warnings)
cargo fmt --all -- --check             → OK
cargo test --lib sink::indradb         → 28 passed; 0 failed
cargo test --all-targets               → all passed; 0 failed
```

### Deviations from plan.md

1. **neo4j.rs lint fix** — plan says touch only `src/sink/indradb.rs` but three unused imports in neo4j.rs caused `LINT_FAIL`. Removed imports only; no logic change. Flagged to sr-dev.
2. **Chunking arithmetic** — worst-case `items_per_node = 17` (was 7); `items_per_edge = 4` (was 2). Effective throughput reduced ~40% for sparse optional fields. Operator may need to halve `DEFAULT_BATCH_SIZE` for code-heavy repos.
3. **`json_value<T>` helper** — added to avoid string round-trip for structured list serialisation (ADR-14).

### Follow-ups

- `@qa-engineer` — gated integration tests require `CPP_INDEXER_LIVE_INDRADB=1` and `CPP_INDEXER_LIVE_NEO4J=1`
- `@sr-dev` — batch size concern: document in deploy-notes if leveldb throughput regresses
- IndraDB v5 composite-index limitation documented in code comment + ADR-15; no v6 follow-up in M8

### References

- plan.md lines 239-276
- design.md §3.6
- adr-14.md, adr-15.md
- scenarios.md @AC-S45-1..5, @AC-S44-1, @AC-S44-2

## S46 — Schema docs, prompt/example refresh, wiki cross-links

### Files changed

- `docs/schema/SCHEMA.md` (new) — full promoted-property reference table (10 node + 2 edge fields), applicability matrix, Neo4j index table, Q1/Q5 worked Cypher examples, AccessKind value set, references to ADRs and source files.
- `docs/runbooks/staging-recovery.md` — appended §6 "Full Re-Index Against v5 Schema (M8 Upgrade Recipe)" with the exact CLI command sequence: confirm schema version → stop daemon → wipe staging → wipe graph (API or direct Cypher) → start daemon → trigger ingest → verify promoted fields + indexes.
- `prompt/graph_database/cpp/schema.txt` — added "M8 promoted node properties (schema-version v5)" section (10 fields), "M8 promoted edge properties" section (2 fields + AccessKind value set), and "Example queries" section with Q1 (`return_type` filter) and Q5 (`source_association_type` filter).
- `tests/schema_drift.rs` (new) — drift check integration test at top-level.

### Exit-gate results

```
cargo build --all-targets                                           → OK
cargo clippy --all-targets -- -D warnings                         → OK
cargo fmt --all -- --check                                        → OK (after auto-fmt)
test -f docs/schema/SCHEMA.md                                     → OK
grep -q "re-index" docs/runbooks/staging-recovery.md              → OK
grep -q "source_association_type" prompt/graph_database/cpp/schema.txt → OK
grep -q "return_type" prompt/graph_database/cpp/schema.txt        → OK
cargo test --test schema_drift                                     → 3 passed; 1 ignored
```

### Deviations from plan.md

1. **Test path**: plan.md specifies `tests/integration/schema_drift.rs`; implemented as `tests/schema_drift.rs` (top-level). The exit criterion `cargo test --test schema_drift` requires a top-level binary — the `tests/integration/` path would make it a submodule under the aggregator, not a named binary. Flagged to `@sr-dev`.
2. **Wiki page update** (AC-S46-3): `~/workspace/wiki/pages/code/cpp-indexer.md` update is delegated to Role 8 (doc-writer) per task dispatch notes. Not implemented here.

### Follow-ups

- `@doc-writer` — add M8 section to `~/workspace/wiki/pages/code/cpp-indexer.md` linking to `[[pages/planning/cpp-indexer-structured-attrs-prd]]` (AC-S46-3).
- `@sr-dev` — correct `tests/integration/schema_drift.rs` path in plan.md to `tests/schema_drift.rs`.

### References

- plan.md lines 278-315
- design.md §4 traceability table
- adr-11.md, adr-13.md, adr-15.md
- scenarios.md @AC-S46-1..5
