# senior-developer log: graph-symbol-ids (plan mode)

Stage 4/8. Read CHARTER, requirements, scenarios, design, adr-1..4 (all Status: accepted → I2 satisfied). Loaded rust-conventions. Grounded file paths against the live `src/` tree.

## Deliverable
`/Users/husam/workspace/cpp-indexer/.claude/handoff/v4/plan.md` — 6 stories, all with exit-criteria commands (no MISSING_EXIT_CRITERIA).

## Story shape
1. SymbolAllocator (S1+S2): new `src/resolve/symbol_map.rs`, `rusqlite` bundled dep, thread-safe get-or-insert + bidirectional write-through LRU + read-only cross-repo resolver handle. Parallel-safe.
2. Config (S3): cache size + db path via CLI/env/struct (`config/{mod,env}.rs`, `bin/index.rs`). Parallel-safe.
3. Write path (S4 + cross_repo emission + v5→v6 bump + S7-SC-12): ATOMIC — schema fields, both sinks id-only, version bump, cross_repo `materialise_external_refs`. NOT parallel-safe; merge after Story 1.
4. Read/resolve (S5 in-repo + S7-SC-13/14): daemon `api/routes.rs` + id_resolver. cpp-mcp is a separate repo → documented follow-up. After Story 3.
5. Migration handshake (S6 / adr-4 follow-up i): write-path refuses v5 graph without reset. After Story 3.
6. Verification (S4-SC-06 measurement + S7-SC-15 regression). Last.

## Key planning decisions (from advisor)
- Did NOT split the v6 flip: `schema_version_bump.rs` gate + ADR-2 no-dual-write force schema fields + version bump + both sinks into ONE atomic story (else lying intermediate / v6 graph with USR strings violating S6-SC-03).
- Exposed a read-only cross-repo resolver handle in Story 1's API — Story 3 (EXTERNAL_REF dst_id) and Story 4 (read path) cannot compile without it (ADR-1 pts 4-5). Subtlest dependency; stated in both.
- S4-SC-06 ≥30% size goal is NOT a `cargo test` gate (no "before" in one run) — recorded measurement in implementation-notes; enforceable CI proxy is S6-SC-03 (no USR strings on v6 nodes/edges).
- Scoped every `cargo test <module>` to feature modules so the developer never loops on the pre-existing `schema_drift_live_neo4j` (S7-SC-15 exemption, needs live Neo4j).
- Flagged `tests/schema-baseline.txt` update in Story 3 (schema_drift static name-check must stay green).
- Field names verbatim per OQ-2: `symbol_id`, `file_id`, `src_id`, `dst_id`, `dst_repo_name`; IndraDB `usr_to_uuid` re-keyed `(repo_name, usr)` → `(repo_name, symbol_id)`.

## Status
clear. 6 stories (2 parallel-safe: Story 1, Story 2).
