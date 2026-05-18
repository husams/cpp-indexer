---
run_id: cpp-indexer-m8-v2
milestone: M8 — Structured Node Attributes
author: senior-developer
created: 2026-05-18
references:
  - /Users/husam/workspace/cpp-indexer/.claude/handoff/v2/CHARTER.md
  - /Users/husam/workspace/cpp-indexer/.claude/handoff/v2/requirements.md
  - /Users/husam/workspace/cpp-indexer/.claude/handoff/v2/scenarios.md
  - /Users/husam/workspace/cpp-indexer/.claude/handoff/v2/design.md
  - /Users/husam/workspace/cpp-indexer/.claude/handoff/v2/adr-11.md
  - /Users/husam/workspace/cpp-indexer/.claude/handoff/v2/adr-12.md
  - /Users/husam/workspace/cpp-indexer/.claude/handoff/v2/adr-13.md
  - /Users/husam/workspace/cpp-indexer/.claude/handoff/v2/adr-14.md
  - /Users/husam/workspace/cpp-indexer/.claude/handoff/v2/adr-15.md
cognee_tags: [task:cpp-indexer-m8, role:senior-developer]
---

# M8 Implementation Plan — Structured Node Attributes

## Goal

Promote 10 node fields + 2 USES-edge fields from `attrs_json` to native columns across NodeRecord/EdgeRecord → Arrow → Neo4j → IndraDB, bump `SCHEMA_VERSION` to 5 with hard handshake refusal, and add 4 Neo4j indexes + 4 IndraDB `index_property` registrations. No automatic migration.

## Toolchain note

This is a **Rust** project (per design.md §7). Exit-criteria commands use `cargo`, not the cpp-conventions toolchain. The cpp-conventions skill was loaded per dispatch instructions; it does not apply to cpp-indexer's own Rust deliverables.

## Sequencing & parallelism

```
S40 (foundation)
  ├── S41 (callable) ─┐
  ├── S42 (templates)─┼── S44 (Neo4j) ─┐
  └── S43 (USES)     ─┘                ├── S46 (docs)
                       S45 (IndraDB) ──┘
```

- S40 must land first; S41/S42/S43 are parallel-safe relative to each other (touch disjoint visitor branches but share `src/visit/shallow.rs` — sequential merge recommended to avoid trivial rebase conflicts; mark parallel-safe=false).
- S44 and S45 are parallel-safe (different sink files).
- S46 lands last.

Parallel-safe stories: 1 of 7 (S45 vs S44).

---

## S40 — Schema version bump + NodeRecord/EdgeRecord fields + Arrow round trip

**AC IDs satisfied:** AC-S40-1, AC-S40-2, AC-S40-3, AC-S40-4, AC-S40-5, AC-S40-6
**ADRs:** ADR-11, ADR-14
**Parallel-safe:** no (blocks all others)
**Priority:** P0

### Files to touch
- `src/schema/version.rs` — bump `SCHEMA_VERSION` constant 4 → 5; ensure `SchemaVersionMismatch` error variant is reachable from the handshake path.
- `src/schema/nodes.rs` — add 10 new optional fields to `NodeRecord` per design.md §3.2; define `Param`, `TemplateParam`, `TemplateArg` structs with `serde(rename="type")` on `Param.type_`.
- `src/schema/edges.rs` — add `source_association_type: Option<String>` and `target_association_type: Option<String>` to `EdgeRecord`.
- `src/schema/arrow.rs` — extend `node_schema()` and `edge_schema()` with the typed columns from design.md §3.4; extend `to_record_batch` / `from_record_batch` for both records to round-trip new fields including `List<Struct>` columns per ADR-14.
- `tests/schema-baseline.txt` — regenerate to reflect v5 schema (gate file paired with the version bump).
- `tests/integration/arrow_roundtrip.rs` — extend existing `nullable_fields_none_round_trip` (`arrow.rs:671`) to cover every new field with three cases: `Some(non-empty)`, `Some(empty)`, `None`.
- `tests/schema_version_bump.rs` — assert handshake on a pre-v5 graph returns `SchemaVersionMismatch` (not panic, not generic IO error) — AC-S40-1/5.

### New files
- None (all additions land in existing modules).

### Tests required
- Unit: Arrow round-trip for `Some(non-empty)`, `Some(empty)`, `None` for each new field (AC-S40-4).
- Unit: `NodeRecord` and `EdgeRecord` struct shape assertions (AC-S40-2/3).
- Unit: `attrs_json` no-double-write assertion for one promoted field (AC-S40-6).
- Integration: schema-handshake mismatch returns typed error (AC-S40-1/5).

### Exit criteria (commands; all MUST return zero)
```bash
cargo build --all-targets
cargo clippy --all-targets -- -D warnings
cargo fmt --all -- --check
cargo test --test schema_version_bump
cargo test --test arrow_roundtrip
cargo test --lib schema::
```

### Risks / Out of scope
- Out of scope: dual-write of `is_virtual`/`is_pure_virtual`/`is_static` (ADR-11 chose full promotion).
- Risk: missing baseline update will cause cascading CI failure across S41–S45. Land baseline edit in the same commit as the constant bump.

---

## S41 — Callable extraction (return_type, params, signature, bounded code)

**AC IDs satisfied:** AC-S41-1, AC-S41-2, AC-S41-3, AC-S41-4, AC-S41-5, AC-S41-6, AC-S41-7, AC-S41-8
**ADRs:** ADR-12 (32 KiB inline cap), ADR-14
**Parallel-safe:** no (shares `src/visit/shallow.rs` with S42/S43)
**Priority:** P0
**Depends on:** S40

### Files to touch
- `src/visit/shallow.rs` — at FUNCTION / METHOD cursor branches: populate `return_type` via `entity.get_result_type().and_then(|t| t.get_display_name())`; build `params` via `entity.get_arguments()` mapping each `arg.get_name()` + `arg.get_type().get_display_name()` to `Param`; build `signature` as `format!("{ret}({csv})")` + append ` const` / ` volatile` for METHOD when `entity.is_const_method()` / `entity.is_volatile_method()` returns true; slice cached source via `entity.get_range()` byte offsets and apply 32 KiB cap from `src/schema/limits.rs`.

### New files
- `src/schema/limits.rs` — `pub const MAX_CODE_BYTES: usize = 32 * 1024;` and helper `pub fn clip_code(src: &str) -> (Option<String>, bool)` returning `(Some(src.to_owned()), false)` when `src.len() <= MAX_CODE_BYTES`, else `(None, true)` per ADR-12.
- `tests/visit/callable_extraction.rs` — fixture-based unit tests asserting `return_type`, `params`, `signature` for a known free function and a known method (`const`-qualified case); plus tests for the exact 32 KiB boundary (32768 = within, 32769 = truncated) per AC-S41-5/6.

### Tests required
- Unit: free-function fixture asserts all three fields (AC-S41-1/2/4/8).
- Unit: `const` method fixture asserts signature includes ` const` suffix (AC-S41-3/8).
- Unit: 32768-byte body → `code=Some`, `code_truncated=Some(false)` (AC-S41-5).
- Unit: 32769-byte body → `code=None`, `code_truncated=Some(true)`, no panic (AC-S41-6).
- Gated integration: leveldb corpus indexed; Cypher Q1 returns non-null `return_type`, `signature`, `params` for `leveldb::DBImpl::Open` (AC-S41-7).

### Exit criteria
```bash
cargo build --all-targets
cargo clippy --all-targets -- -D warnings
cargo fmt --all -- --check
cargo test --test callable_extraction
cargo test --lib visit::shallow
CPP_INDEXER_LIVE_NEO4J=1 cargo test --test neo4j_indexes -- --ignored --nocapture
```

### Risks / Out of scope
- Out of scope: storing `code` in a sidecar blob (ADR-12 chose inline).
- Risk: source-buffer caching may not cover all TUs in current visitor; verify in dev whether `file_path` read is already cached or if the read must be added.

---

## S42 — Template parameter and specialization argument extraction

**AC IDs satisfied:** AC-S42-1, AC-S42-2, AC-S42-3, AC-S42-4
**ADRs:** ADR-14
**Parallel-safe:** no (shares `src/visit/shallow.rs`)
**Priority:** P0
**Depends on:** S40

### Files to touch
- `src/visit/shallow.rs` — at TEMPLATE_DECL: iterate child cursors, collect `TemplateTypeParameter` (kind=`type`), `NonTypeTemplateParameter` (kind=`non_type`), `TemplateTemplateParameter` (kind=`template`); capture default-argument child as `default: Option<String>`. At SPECIALIZATION: replace existing debug-string code with structured `Vec<TemplateArg>` from `entity.get_template_arguments()`; classify kind as `type` / `integral` / `template` / `expression` based on libclang variant; remove the old debug-string write to `attrs_json` (AC-S40-6 enforcement on template_args).

### New files
- `tests/visit/template_extraction.rs` — fixture template declaration with one of each parameter kind (type, non-type, template-template); fixture specialization (e.g. `std::vector<int>`).

### Tests required
- Unit: template-decl fixture asserts the 3 kinds appear in `template_params` (AC-S42-1/4).
- Unit: specialization fixture asserts `template_args` is structured `Vec<TemplateArg>`, not a debug string; asserts `attrs_json` no longer holds the old form (AC-S42-2/4 + AC-S40-6).
- Gated integration: leveldb corpus → Cypher Q4 returns non-empty structured list for `std::vector` specializations (AC-S42-3).

### Exit criteria
```bash
cargo build --all-targets
cargo clippy --all-targets -- -D warnings
cargo fmt --all -- --check
cargo test --test template_extraction
cargo test --lib visit::shallow
CPP_INDEXER_LIVE_NEO4J=1 cargo test --test neo4j_indexes -- --ignored --nocapture
```

### Risks / Out of scope
- Risk: libclang `get_template_arguments()` availability varies by crate version; verify `clang-rs` API surface before dev.

---

## S43 — USES edge access classifier

**AC IDs satisfied:** AC-S43-1, AC-S43-2, AC-S43-3, AC-S43-4, AC-S43-5, AC-S43-6
**ADRs:** ADR-13 (closed 7-value enum + EXTERNAL_REF Phase 5 mirror)
**Parallel-safe:** no (shares `src/visit/shallow.rs`)
**Priority:** P0
**Depends on:** S40

### Files to touch
- `src/visit/shallow.rs` — at every USES emission site, call `classify_use(&cursor, &parent_chain)` and write `source_association_type` + `target_association_type` to `EdgeRecord` via `AccessKind::as_str()`.
- `src/schema/edges.rs` — already extended in S40; no further change.
- `src/resolve/cross_repo.rs` — at EXTERNAL_REF synthesis (Phase 5), copy the source USES edge's `source_association_type` / `target_association_type` onto the synthesized edge per ADR-13.

### New files
- `src/visit/access_classifier.rs` — `pub enum AccessKind { Read, Write, AddrOf, CallArg, Return, DeclRef, Unknown }` with `as_str(&self) -> &'static str`; `pub fn classify_use(cursor: &Entity, parent_chain: &[Entity]) -> AccessKind` implementing the decision tree from ADR-13; emit `tracing::warn!` log on `Unknown` per AC-S43-3.
- `tests/visit/access_classifier.rs` — fixture cases per AC-S43-6 examples table: variable read → `read`, field assignment → `write`, function-arg pass → `call_arg`, overloaded operator → `unknown` (+ log assertion).
- `tests/integration/cross_repo_access_mirror.rs` — gated test asserting Phase 5 EXTERNAL_REF carries the source edge's classification.

### Tests required
- Unit: 4 classification cases (AC-S43-6).
- Unit: `Unknown` path emits log and edge is NOT dropped (AC-S43-3).
- Unit: enum variant set is exactly 7 (compile-time enforced by exhaustive match in `as_str`; runtime test asserts the full set) — AC-S43-1.
- Unit: symmetric target classification for write target (AC-S43-2).
- Gated integration: leveldb Cypher query on `leveldb::DBImpl::mutex_` returns non-zero bucket per `source_association_type` (AC-S43-5); EXTERNAL_REF mirror copy verified (ADR-13).

### Exit criteria
```bash
cargo build --all-targets
cargo clippy --all-targets -- -D warnings
cargo fmt --all -- --check
cargo test --test access_classifier
cargo test --lib visit::access_classifier
CPP_INDEXER_LIVE_NEO4J=1 cargo test --test neo4j_indexes -- --ignored --nocapture
CPP_INDEXER_LIVE_NEO4J=1 cargo test --test cross_repo_access_mirror -- --ignored --nocapture
```

### Risks / Out of scope
- Risk: high `unknown` rate on operator-overloaded corpora; ADR-13 accepts this for M8, follow-up in M9.
- Out of scope: classification of CALLS edges (separate story per design.md §6).

---

## S44 — Neo4j native property writes + covering indexes

**AC IDs satisfied:** AC-S44-1, AC-S44-2, AC-S44-3, AC-S44-4, AC-S44-5
**ADRs:** ADR-14 (serialization), ADR-15 (indexes)
**Parallel-safe:** yes (with S45 — different sink files)
**Priority:** P0
**Depends on:** S40, S41, S42, S43

### Files to touch
- `src/sink/neo4j.rs` — extend `CQL_MERGE_NODES` SET clause (around line 53) with the 10 promoted node properties from design.md §3.5; extend `CQL_MERGE_EDGES` with `source_association_type` / `target_association_type`; ensure parameter map building serializes `params` / `template_params` / `template_args` as Bolt `List<Map>` per ADR-14; add 4 `CREATE INDEX IF NOT EXISTS` statements in the sink-init path (`node_return_type_idx`, `node_is_virtual_idx`, `node_is_static_idx`, composite `node_kind_return_type_idx`) per ADR-15.

### New files
- `tests/integration/neo4j_indexes.rs` — gated (`#[ignore]`, opted in via `CPP_INDEXER_LIVE_NEO4J=1`) against dev Neo4j `bolt://192.168.1.200:7687`: asserts `SHOW INDEXES` contains all 4 new indexes (AC-S44-3); asserts `EXPLAIN` plan reports `NodeIndexSeek` for the AC-S44-4 query; asserts idempotent re-run (no error on duplicate creation).

### Tests required
- Unit: parameter-map builder for nodes round-trips a `NodeRecord` with every promoted field set (AC-S44-1).
- Unit: parameter-map builder for edges includes both association_type fields (AC-S44-2).
- Gated integration: indexes exist (AC-S44-3); `EXPLAIN` shows NodeIndexSeek, not AllNodesScan/NodeByLabelScan (AC-S44-4); re-run idempotent.
- Regression: the 3 previously-ignored Neo4j tests continue to pass (AC-S44-5).

### Exit criteria
```bash
cargo build --all-targets
cargo clippy --all-targets -- -D warnings
cargo fmt --all -- --check
cargo test --lib sink::neo4j
CPP_INDEXER_LIVE_NEO4J=1 cargo test --test neo4j_indexes -- --ignored --nocapture
CPP_INDEXER_LIVE_NEO4J=1 cargo test --package cpp-indexer --test '*' -- --ignored neo4j
```

### Risks / Out of scope
- Risk: deadlock retry (`MAX_TRANSIENT_RETRIES = 3`) at `src/sink/neo4j.rs:277,720` may saturate under wider SET clauses; if dev hits the ceiling, dev must raise the ceiling in the same commit and flag in implementation-notes.
- Risk: Bolt frame size grows; dev should test against leveldb corpus and flag in deploy-notes if `DEFAULT_BATCH_SIZE` needs halving.
- Out of scope: Neo4j range/text indexes beyond the 4 in ADR-15.

---

## S45 — IndraDB native property writes + index_property parity

**AC IDs satisfied:** AC-S45-1, AC-S45-2, AC-S45-3, AC-S45-4, AC-S45-5
**ADRs:** ADR-14, ADR-15
**Parallel-safe:** yes (with S44 — different sink files)
**Priority:** P0
**Depends on:** S40, S41, S42, S43

### Files to touch
- `src/sink/indradb.rs` — add 12 property-name constants per design.md §3.6; in the bulk-insert builder, emit `BulkInsertItem::VertexProperty` per populated `Option::Some` field (skip `None`); serialize `params` / `template_params` / `template_args` as `indradb::Json` per ADR-14; add 4 `client.index_property(…)` calls during sink init per ADR-15 (document IndraDB v5 composite-index limitation in code comment).

### New files
- `tests/integration/indradb_properties.rs` — gated (`#[ignore]`, opted in via `CPP_INDEXER_LIVE_INDRADB=1`): asserts `get_vertex_properties` for a fixture METHOD/FUNCTION node returns `return_type`, `params`, `signature` with non-null values (AC-S45-3); asserts `get_edge_properties` for a USES edge returns both association_type keys (AC-S45-2); asserts no-panic path for a `code_truncated:true` node (AC-S45-5).
- `tests/integration/sink_parity.rs` — gated combined-sink test: index one fixture through both sinks; assert promoted property-key set is identical between Neo4j top-level Cypher props and IndraDB `get_vertex_properties` (parity scenarios in scenarios.md).

### Tests required
- Unit: bulk-insert builder skips `None` fields (no spurious property writes).
- Unit: structured list fields serialize to `indradb::Json` correctly.
- Gated integration: vertex/edge property reads (AC-S45-1/2/3); `code_truncated:true` write path (AC-S45-5).
- Regression: the 12 previously-ignored IndraDB tests pass (AC-S45-4).
- Gated cross-sink: parity assertion (scenarios.md @AC-S45-1 @AC-S44-1 + @AC-S45-2 @AC-S44-2).

### Exit criteria
```bash
cargo build --all-targets
cargo clippy --all-targets -- -D warnings
cargo fmt --all -- --check
cargo test --lib sink::indradb
CPP_INDEXER_LIVE_INDRADB=1 cargo test --test indradb_properties -- --ignored --nocapture
CPP_INDEXER_LIVE_INDRADB=1 CPP_INDEXER_LIVE_NEO4J=1 cargo test --test sink_parity -- --ignored --nocapture
CPP_INDEXER_LIVE_INDRADB=1 cargo test --package cpp-indexer -- --ignored indradb
```

### Risks / Out of scope
- Risk: IndraDB memory backend OOM on `code`-heavy fixtures; ADR-12 test-cap shim mitigates — dev must confirm shim is applied to memory-backend tests.
- Out of scope: IndraDB v6 composite/range indexes (documented as known limitation per ADR-15).

---

## S46 — Schema docs, prompt/example refresh, wiki cross-links

**AC IDs satisfied:** AC-S46-1, AC-S46-2, AC-S46-3, AC-S46-4, AC-S46-5
**ADRs:** (consumes all)
**Parallel-safe:** no (must observe final shipped field names)
**Priority:** P1
**Depends on:** S40, S41, S42, S43, S44, S45

### Files to touch
- `docs/runbooks/staging-recovery.md` — append re-index recipe section: wipe old graphs, exact CLI command sequence to perform a fresh index against v5 schema (AC-S46-2).
- `prompt/graph_database/cpp/schema.txt` — update field listing with all 12 promoted fields; add Q1 (return_type/signature query) and Q5 (source_association_type query) as worked examples (AC-S46-4).
- `~/workspace/wiki/pages/code/cpp-indexer.md` — add M8 section referencing the promoted properties and linking to `[[pages/planning/cpp-indexer-structured-attrs-prd]]` (AC-S46-3).

### New files
- `docs/schema/SCHEMA.md` — table of every promoted property with: field name, type, applicable NodeKind/EdgeKind, source-of-truth file path (e.g. `src/visit/shallow.rs`, `src/sink/neo4j.rs`), and sample Cypher query (AC-S46-1).
- `tests/integration/schema_drift.rs` — automated drift check: parse field names from `schema.txt`, `docs/schema/SCHEMA.md`, and one live Cypher property-keys query; fail if any name disagrees (AC-S46-5).

### Tests required
- Doc: SCHEMA.md exists with required columns per AC-S46-1 (manual review during developer/QA gate).
- Doc: runbook recipe present (AC-S46-2).
- Integration: drift check passes when names match, fails when a name is intentionally mutated in a test fixture (AC-S46-5).

### Exit criteria
```bash
cargo build --all-targets
cargo clippy --all-targets -- -D warnings
cargo fmt --all -- --check
test -f docs/schema/SCHEMA.md
grep -q "re-index" docs/runbooks/staging-recovery.md
grep -q "source_association_type" prompt/graph_database/cpp/schema.txt
grep -q "return_type" prompt/graph_database/cpp/schema.txt
CPP_INDEXER_LIVE_NEO4J=1 cargo test --test schema_drift -- --ignored --nocapture
```

### Risks / Out of scope
- Risk: wiki write is outside the project repo; documented as a separate commit on `~/workspace/wiki/` per llm-wiki conventions.
- Out of scope: NL-to-Cypher prompt expansion beyond Q1/Q5 worked examples.

---

## References

- Requirements: `/Users/husam/workspace/cpp-indexer/.claude/handoff/v2/requirements.md`
- Scenarios: `/Users/husam/workspace/cpp-indexer/.claude/handoff/v2/scenarios.md`
- Design: `/Users/husam/workspace/cpp-indexer/.claude/handoff/v2/design.md`
- ADRs: adr-11 (schema bump), adr-12 (code inline cap), adr-13 (USES classifier), adr-14 (serialization), adr-15 (indexes)
- Charter: `/Users/husam/workspace/cpp-indexer/.claude/handoff/v2/CHARTER.md`
- PRD: `[[pages/planning/cpp-indexer-structured-attrs-prd]]`
- Wiki: `[[pages/code/cpp-indexer]]`
- Cognee tags: `task:cpp-indexer-m8 role:senior-developer`
