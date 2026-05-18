---
run_id: cpp-indexer-m8-v2
milestone: M8 — Structured Node Attributes
author: product-manager
created: 2026-05-18
references:
  - /Users/husam/workspace/cpp-indexer/.claude/handoff/v2/requirements-raw.md
  - /Users/husam/workspace/wiki/pages/planning/cpp-indexer-structured-attrs-prd.md
  - /Users/husam/workspace/wiki/pages/planning/cpp-indexer-structured-attrs-brief.md
---

# M8 — Structured Node Attributes: Requirements

## Scope

Promote a closed set of frequently-queried per-node/edge attributes from opaque `attrs_json` strings into native, indexable graph properties in both Neo4j and IndraDB sinks. Bump `SCHEMA_VERSION` per ADR-9. Out of scope: new node/edge kinds, NL-to-Cypher translator, agent loop, automatic migration of old graphs.

---

## S40 — Schema version bump, NodeRecord / EdgeRecord fields, Arrow round trips

Story: As a cpp-indexer developer, I want the `SCHEMA_VERSION` constant and Rust record structs updated to carry the new promoted fields, so that downstream story work has a stable, type-safe foundation and old graphs are rejected at handshake.

Acceptance criteria:

- **AC-S40-1** — Given the codebase before this story, when `SCHEMA_VERSION` is bumped, then the schema-handshake test (m6_agent_gate) fails if the old version constant is presented, verifying that old and new schemas cannot coexist in the same graph.
- **AC-S40-2** — Given the `NodeRecord` struct in `src/schema/`, when the story lands, then `NodeRecord` carries these new optional fields: `return_type: Option<String>`, `params: Option<Vec<{name: String, type: String}>>`, `signature: Option<String>`, `code: Option<String>`, `code_truncated: Option<bool>`, `template_params: Option<Vec<{name, kind, default?}>>`, `template_args: Option<Vec<{kind, value}>>`, `is_virtual: Option<bool>`, `is_pure_virtual: Option<bool>`, `is_static: Option<bool>`.
- **AC-S40-3** — Given the `EdgeRecord` struct in `src/schema/`, when the story lands, then `EdgeRecord` carries `source_association_type: Option<String>` and `target_association_type: Option<String>` as native fields.
- **AC-S40-4** — Given the new `NodeRecord` and `EdgeRecord` schemas, when Arrow read/write round-trip tests are executed, then all new fields serialize and deserialize without data loss for both `Some(value)` and `None` cases.
- **AC-S40-5** — Given a graph written by the previous schema version, when the schema handshake runs against it, then the system surfaces a `SchemaVersionMismatch` error explicitly (not a panic or generic I/O error).
- **AC-S40-6** — Given existing `attrs_json` fields, when the story lands, then `attrs_json` is retained for non-promoted long-tail attributes (`exception_spec`, `control_flow`, `bit_field`); no promoted field is double-written to both native and `attrs_json`.

Priority: P0 — foundational; S41–S45 cannot safely land until record structs and version handshake are in place.

Dependencies: none upstream within M8; requires ADR-9 (schema version policy) to be non-proposed before development dispatch.

Open questions:
- Should `is_virtual`, `is_pure_virtual`, `is_static` move fully out of `attrs_json`, or remain dual-written during a transition window? PRD implies full promotion (indexes on them at S44), but the NodeRecord field list in AC-S3.1 of the PRD does not list them explicitly — architect must confirm.

References: `[[pages/planning/cpp-indexer-structured-attrs-prd]]` §5 AC-S3; `requirements-raw.md` §Sequencing S40.

---

## S41 — Callable extraction: return type, params, signature, bounded code

Story: As a cpp-indexer developer, I want the shallow visitor to extract and populate `return_type`, `params`, `signature`, and bounded `code` for FUNCTION and METHOD nodes, so that agents can answer return-type and parameter questions with a single indexed Cypher query.

Acceptance criteria:

- **AC-S41-1** — Given a FUNCTION or METHOD cursor in `src/visit/shallow.rs`, when the visitor runs, then `return_type` is populated via `entity.get_result_type().get_display_name()` and written to `NodeRecord.return_type`.
- **AC-S41-2** — Given a FUNCTION or METHOD cursor, when the visitor runs, then `params` is populated as an ordered list of `{name, type}` pairs via `entity.get_arguments()`, using `arg.get_type().get_display_name()` and `arg.get_name()`.
- **AC-S41-3** — Given a METHOD cursor, when the visitor runs, then `signature` is constructed as `<return_type>(<param_types_csv>)` with cv-qualifiers preserved (e.g. `const`, `volatile` appended where applicable).
- **AC-S41-4** — Given a FUNCTION cursor, when the visitor runs, then `signature` is constructed as `<return_type>(<param_types_csv>)` (no cv-qualifier suffix for free functions).
- **AC-S41-5** — Given a FUNCTION or METHOD cursor whose source range is ≤ 32 KiB, when the visitor reads `entity.get_range()` from `file_path`, then `code` is populated with the verbatim source span and `code_truncated` is `false`.
- **AC-S41-6** — Given a FUNCTION or METHOD cursor whose source range exceeds 32 KiB, when the visitor reads the source span, then `code` is `None` and `code_truncated` is `true` (not an error or panic).
- **AC-S41-7** — Given a corpus containing `leveldb::DBImpl::Open`, when `MATCH (m:Node {qualified_name: 'leveldb::DBImpl::Open'}) RETURN m.return_type, m.signature, m.params` is executed, then all three fields return non-null values (Q1 from PRD §6).
- **AC-S41-8** — Given unit tests on fixture files in the test suite, when S41 lands, then at least one test asserts `return_type`, `params`, and `signature` values for a known fixture function and a known fixture method.

Priority: P0 — satisfies G1; enables Q1/Q2 from PRD §6; prerequisite input for S44/S45.

Dependencies: S40 (NodeRecord fields must exist).

Open questions:
- PRD §9: Should `code` snippets be stored on the node (current proposal) or in a sidecar blob keyed by `usr` to keep the graph lean? Decision pending size measurements on a 100k-file repo. If sidecar is chosen, AC-S41-5/6 must be revised before developer dispatch.

References: `[[pages/planning/cpp-indexer-structured-attrs-prd]]` §5 AC-S1.1–S1.4; §6 Q1/Q2.

---

## S42 — Template parameter and specialization argument extraction

Story: As a cpp-indexer developer, I want the visitor to extract structured `template_params` for TEMPLATE_DECL nodes and structured `template_args` for SPECIALIZATION nodes, so that agents can query which templates were instantiated with which arguments without JSON parsing.

Acceptance criteria:

- **AC-S42-1** — Given a TEMPLATE_DECL cursor in `src/visit/shallow.rs`, when the visitor runs, then `template_params` is populated as a list of `{name, kind, default?}` by iterating child cursors of types `TemplateTypeParameter`, `NonTypeTemplateParameter`, and `TemplateTemplateParameter`.
- **AC-S42-2** — Given a SPECIALIZATION cursor, when the visitor runs, then `template_args` is populated as a list of `{kind: "type"|"integral"|...; value: String}`, replacing the previous debug-printed string form stored in `attrs_json`.
- **AC-S42-3** — Given the leveldb corpus after indexing, when `MATCH (s:Node {kind:'SPECIALIZATION'})-[:SPECIALIZES]->(:Node {qualified_name:'std::vector'}) RETURN s.template_args` is executed, then `template_args` returns a non-empty structured list (not a debug string).
- **AC-S42-4** — Given unit tests on fixture files, when S42 lands, then at least one test asserts correct `template_params` for a known template declaration and correct `template_args` for a known specialization.

Priority: P0 — satisfies G2; enables Q4 from PRD §6.

Dependencies: S40 (NodeRecord fields must exist).

Open questions: none specific to this story beyond those in S40 open questions.

References: `[[pages/planning/cpp-indexer-structured-attrs-prd]]` §5 AC-S1.5–S1.6; §6 Q4.

---

## S43 — USES edge access classifier

Story: As a cpp-indexer developer, I want the visitor to classify each USES edge with `source_association_type` and `target_association_type`, so that agents can distinguish reads from writes and other access modes without requiring full-graph scans.

Acceptance criteria:

- **AC-S43-1** — Given a USES edge emission site in the visitor, when the cursor context is classified, then `source_association_type` is set to one of exactly: `read`, `write`, `addr_of`, `call_arg`, `return`, `decl_ref`, `unknown`. No other values are permitted.
- **AC-S43-2** — Given a USES edge emission site, when `target_association_type` is set, then it reflects the referenced entity kind symmetrically (e.g. a field used as write target → `write` on target side).
- **AC-S43-3** — Given a cursor context the classifier cannot confidently categorize (e.g. overloaded operators, pointer deref writes), when the edge is emitted, then `source_association_type` is `unknown` and an entry is logged; the edge is not dropped.
- **AC-S43-4** — Given both association type fields, when they are written to `EdgeRecord`, then they appear as native edge properties (not inside `attrs_json`).
- **AC-S43-5** — Given the leveldb corpus after indexing, when `MATCH (m:Node {kind:'METHOD'})-[u:EDGE {kind:'USES'}]->(:Node {qualified_name:'leveldb::DBImpl::mutex_'}) RETURN u.source_association_type` is executed, then the count per `source_association_type` returns non-zero buckets (G3 measurable).
- **AC-S43-6** — Given classifier unit tests on fixture files, when S43 lands, then tests cover at least: one `read`, one `write`, one `call_arg`, and one `unknown` classification case.

Priority: P0 — satisfies G3; enables Q5 from PRD §6.

Dependencies: S40 (EdgeRecord fields must exist).

Open questions:
- PRD §9: Does the cross-repo `EXTERNAL_REF` edge need the new USES classification mirrored on it? Suggested: yes, set from the original edge's class during Phase 5. Confirm before developer dispatch whether this is in M8 scope or a follow-up.

References: `[[pages/planning/cpp-indexer-structured-attrs-prd]]` §5 AC-S2; §6 Q5.

---

## S44 — Neo4j native property writes and indexes

Story: As a cpp-indexer developer, I want the Neo4j sink to write all promoted properties as native graph properties and create covering indexes, so that Cypher queries against the new attributes use index-backed operators instead of label scans.

Acceptance criteria:

- **AC-S44-1** — Given the updated `CQL_MERGE_NODES` in `src/sink/neo4j.rs` (line 53 area), when nodes are written, then `return_type`, `params`, `signature`, `code`, `code_truncated`, `template_params`, `template_args`, `is_virtual`, `is_pure_virtual`, and `is_static` are set as top-level Cypher properties from the Arrow row inputs.
- **AC-S44-2** — Given the updated `CQL_MERGE_EDGES`, when USES edges are written, then `source_association_type` and `target_association_type` are set as native edge properties.
- **AC-S44-3** — Given a fresh Neo4j instance after indexing, when `SHOW INDEXES` is run, then the following indexes exist: `node_return_type_idx` on `(n:Node) (n.return_type)`, `node_is_virtual_idx` on `(n:Node) (n.is_virtual)`, `node_is_static_idx` on `(n:Node) (n.is_static)`, and a composite index on `(n:Node) (n.kind, n.return_type)`. All are created with `IF NOT EXISTS`.
- **AC-S44-4** — Given a gated integration test, when `EXPLAIN MATCH (m:Node {kind:'METHOD', is_virtual:true}) WHERE m.return_type = 'leveldb::Status' RETURN m.qualified_name` is run, then the explain plan reports `NodeIndexSeek` (not `AllNodesScan` or `NodeByLabelScan`) for the indexed predicates (G5 measurable).
- **AC-S44-5** — Given the existing 3 ignored Neo4j tests, when S44 lands, then those 3 tests continue to pass (no regression).

Priority: P0 — satisfies G4/G5; without this, the queries in PRD §6 remain slow regardless of extraction work.

Dependencies: S40 (schema fields), S41 (callable data), S42 (template data), S43 (USES data).

Open questions:
- PRD §7: The live Neo4j deadlock bug (`Neo.TransientError.Transaction.DeadlockDetected`) will surface more frequently with additional properties per row. Is the deadlock retry fix a prerequisite for S44, a parallel track, or a follow-up? Must be resolved before developer dispatch.

References: `[[pages/planning/cpp-indexer-structured-attrs-prd]]` §5 AC-S4; §3 G4/G5.

---

## S45 — IndraDB native property writes

Story: As a cpp-indexer developer, I want the IndraDB sink to write all promoted properties as per-vertex and per-edge property keys, so that IndraDB queries can access the same attribute surface as Neo4j without JSON parsing.

Acceptance criteria:

- **AC-S45-1** — Given `src/sink/indradb.rs`, when nodes are written, then each promoted field (`return_type`, `params`, `signature`, `code`, `code_truncated`, `template_params`, `template_args`, `is_virtual`, `is_pure_virtual`, `is_static`) is written as a separate vertex property key (not folded into a single JSON blob).
- **AC-S45-2** — Given USES edges written to IndraDB, when edge properties are read back, then `source_association_type` and `target_association_type` appear as distinct edge property keys.
- **AC-S45-3** — Given a gated integration test using `get_vertex_properties`, when a FUNCTION or METHOD node is queried, then the response includes the keys `return_type`, `params`, and `signature` with non-null values for a known fixture node.
- **AC-S45-4** — Given the existing 12 ignored IndraDB tests, when S45 lands, then those 12 tests continue to pass (no regression).
- **AC-S45-5** — Given the IndraDB memory backend used in tests, when `code` snippets are present, then nodes with `code_truncated: true` do not cause test failures; the truncation path is exercised by at least one test.

Priority: P0 — satisfies property parity requirement (G4); IndraDB is a first-class sink.

Dependencies: S40 (schema fields), S41 (callable data), S42 (template data), S43 (USES data).

Open questions:
- PRD §8: IndraDB property-index hints are limited in v5; explicit index control deferred to v6. Confirm whether any index-equivalent behavior should be attempted now or documented as a known limitation.

References: `[[pages/planning/cpp-indexer-structured-attrs-prd]]` §5 AC-S5.

---

## S46 — Schema docs, prompt/example refresh, wiki cross-links

Story: As a cpp-indexer developer, I want `docs/schema/SCHEMA.md`, the graph schema prompt, and the wiki updated to reflect the promoted properties, so that agents and developers see consistent, accurate documentation after M8 lands.

Acceptance criteria:

- **AC-S46-1** — Given `docs/schema/SCHEMA.md` (new file), when it is written, then it lists every promoted property with: field name, type, which node/edge kinds carry it, the source-of-truth code path (e.g. `src/visit/shallow.rs`), and a sample Cypher query.
- **AC-S46-2** — Given `docs/runbooks/staging-recovery.md`, when S46 lands, then it includes a re-index recipe explaining that old-schema graphs must be wiped and re-indexed (not migrated in place), with the exact CLI command sequence.
- **AC-S46-3** — Given the `[[pages/code/cpp-indexer]]` wiki page, when S46 lands, then it is updated to reference M8 promoted properties and links to the PRD `[[pages/planning/cpp-indexer-structured-attrs-prd]]`.
- **AC-S46-4** — Given `prompt/graph_database/cpp/schema.txt`, when S46 lands, then the schema prompt and worked examples are updated to include at least Q1 and Q5 from PRD §6 as example queries demonstrating the new properties.
- **AC-S46-5** — Given a drift-check step in CI or documentation, when `schema.txt`, `docs/schema/SCHEMA.md`, and the sink writes disagree on a field name, then the discrepancy is detectable (manual checklist or automated test asserting field-name parity between schema.txt and one live sink query).

Priority: P1 — important but does not block the query correctness goals; can land last within M8.

Dependencies: S40, S41, S42, S43, S44, S45 (all prior stories must be complete so SCHEMA.md reflects the final state).

Open questions: none blocking.

References: `[[pages/planning/cpp-indexer-structured-attrs-prd]]` §5 AC-S6.2, S7.1, S7.2; §7 schema-prompt drift risk.

---

## Cross-cutting open questions (not assigned to a single story)

1. **Neo4j deadlock retry (S44 blocker candidate):** PRD §7 calls out `Neo.TransientError.Transaction.DeadlockDetected` as a live bug that will worsen with more properties per row. Is this a prerequisite, parallel, or follow-up story? Must be decided before S44 developer dispatch.
2. **`code` storage model (S41 AC dependency):** PRD §9 — node-embedded vs. sidecar blob keyed by `usr`. If sidecar wins, AC-S41-5/6 must be revised before developer dispatch.
3. **`is_virtual`/`is_pure_virtual`/`is_static` promotion completeness (S40 AC dependency):** PRD implies full promotion (indexed), but NodeRecord field list in PRD AC-S3.1 does not enumerate them explicitly. Architect to confirm these move fully out of `attrs_json`.
4. **`EXTERNAL_REF` USES classification (S43 scope question):** PRD §9 — mirror classification on cross-repo edges during Phase 5, or defer? Scope must be set before S43 developer dispatch.
5. **`exception_spec` / `control_flow` promotion:** PRD §9 — leave in `attrs_json` until a real query motivates promotion. Confirmed out of scope for M8 unless overridden.
