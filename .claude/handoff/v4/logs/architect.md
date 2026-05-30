# Architect log — graph-symbol-ids (v4)

## Deliverables
- design.md
- adr-1.md (id-namespace, per-repo IDs / cross-repo USR space) — accepted
- adr-2.md (SCHEMA_VERSION bump 5→6 + tag + PARQUET_MAGIC) — accepted
- adr-3.md (SQLite durable map + thread-safe allocator + write-through LRU) — accepted
- adr-4.md (existing-graph migration: reset/re-index, hard-error) — accepted

## Key decision (the seam)
Allocate integer IDs during the parallel libclang visit (`src/pipeline/parallel.rs`); carry BOTH
`usr` and integer `id` through the staging/Parquet records; sink writes id-only; Phase 5
(`src/resolve/cross_repo.rs`) keeps reading `usr` and is unchanged. This honours D1 (per-repo IDs;
cross-repo stays in USR space), satisfies S2-SC-10 (allocation on parallel path) and the S4
references to arrow.rs/nodes.rs/edges.rs.

Rejected the tidier "translate USR→ID at sink boundary only, Parquet stays USR-only" alternative
because it strands S2-SC-10 and leaves the S4-referenced schema files untouched (traceability break
the QA gate would catch). Advisor concurred; surfaced and adopted.

## Open questions resolved
- OQ-1 default DB path = `<stage_dir>/cxg-symbols.db` (per-repo, falls out of per-repo stage dir).
- OQ-2 field names = `symbol_id` (nodes), `file_id`, `src_id`/`dst_id` (edges).
- OQ-3 read-path consumers = cpp-mcp (get_definition/get_references/get_ast/query_graphdb) + daemon
  GET /v1/repos; deferred direct-graph readers must be grepped + listed by developer.
- OQ-4 = reset/re-index hard-error, not auto-migrate (auto-migrate infeasible: no id-map exists for a
  v5 graph; consistent with existing "pre-v5 graphs refused at handshake" precedent).

## Constraints coverage
C1 read resolution (S5), C2 schema bump (adr-2), C3 durable SQLite (adr-3), C4 thread-safe alloc
(single write-Mutex + WAL; Phase-5 lock NOT reused — clarified), C5 write-through LRU (size-0
disabled / size-1 evict / bidirectional), C6 configurable path, C7 no silent corruption
(hard-error + explicit reset + missing-ID error on read).

## Cross-repo EXTERNAL_REF correction (post-review)
Caught in advisor done-check: integers are repo-local, so an EXTERNAL_REF edge (repo A -> repo B)
cannot take `dst_id` from repo A's allocator. ADR-1 (points 4-5) + design.md §3.6 now specify:
Phase 5's USR *matching* is unchanged, but its edge *emission* resolves `dst_id` via the DESTINATION
repo's SQLite map, and `EdgeRecord` gains `dst_repo_name`; the Neo4j/IndraDB edge MATCH keys src on
`(src_id, repo_name)` and dst on `(dst_id, dst_repo_name)`. The earlier "Phase 5 fully unchanged"
framing was wrong and has been corrected throughout. Keeps S6-SC-03 (no USR strings on any edge).

## Notes for downstream
- Add `rusqlite` (bundled feature) to Cargo.toml.
- DO NOT change Phase 5 resolution logic (USR space).
- Bump SCHEMA_VERSION 5→6 in the SAME commit as the first integer write; in-tree consistency tests
  (`schema_version_bump.rs`) enforce tag/magic lock-step.
- cpp-mcp is a separate repo — coordinate the resolver wiring; flag in plan.md.
