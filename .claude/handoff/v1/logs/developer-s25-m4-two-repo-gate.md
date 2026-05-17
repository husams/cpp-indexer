# Developer log — S25-m4-two-repo-gate

## Skills loaded
- rust-conventions

## Skills considered but not loaded
- cpp-conventions — project is Rust; C++ fixture files are static text, no build toolchain needed
- implement-story — task was dispatched directly; skill not triggered
- cognee-memory — no prior context query needed; all context from source files

## Commands run

| Command | Outcome |
|---------|---------|
| `find . -type f \| grep '\.rs\|\.toml'` | Surveyed project structure |
| `cargo fmt --all -- --check` | EXIT 1 — formatting diffs in m4_exit_gate.rs |
| `cargo fmt --all` | Reformatted; exit 0 |
| `cargo fmt --all -- --check` | EXIT 0 |
| `cargo clippy --all-targets --all-features -- -D warnings` | EXIT 0 |
| `DYLD_LIBRARY_PATH=... cargo nextest run -p cpp_indexer --features test-mock --test m4_exit_gate -- --ignored` | EXIT 0; 1 PASS (early-return guard; no live Neo4j) |
| `DYLD_LIBRARY_PATH=... cargo nextest run -p cpp_indexer --features test-mock` | EXIT 0; 221 PASS, 22 skipped |

## Orientation reads
- CHARTER.md
- plan.md S25 (lines 431-444)
- requirements.md (AC-M4-9, AC-M4-10, M4-S4)
- tests/cross_repo.rs (fixture pattern)
- tests/integration/m2_exit_gate.rs (exit-gate pattern)
- src/resolve/cross_repo.rs (Phase5Options, phase5_run)
- src/resolve/per_repo.rs (cross_repo_candidate classification)
- src/visit/shallow.rs (Phase 1 visitor — confirmed no CallExpr handling)
- src/visit/cursor_map.rs (confirmed CallExpr → None)
- src/schema/edges.rs (EdgeKind variants)
- src/sink/neo4j.rs (CQL_MERGE_EDGES, from_graph, connect)
- Cargo.toml (existing test entries)

## Key finding that shaped implementation

`cursor_map.rs` maps `EntityKind::CallExpr` to `None`, meaning Phase 1 never emits CALLS edges. Indexing the two-repo C++ fixture via `pipeline::run` produces only structural edges (CONTAINS, INCLUDES, etc.), none of which are `kind=CALLS` cross_repo_candidate edges. Synthetic Parquet shards are therefore required to produce the `via=CALLS` result asserted by AC-M4-10. This is a deferred M5 gap, not an S25 defect.

## Deviations from plan.md
1. Synthetic Parquet shards used instead of real libclang CALLS edges (Phase 1 does not emit CallExpr). Tagged sr-dev in implementation-notes.md.
2. Cypher query uses `[:EDGE {kind:"EXTERNAL_REF"}]` (actual sink schema) not `[:EXTERNAL_REF]` (plan shorthand).
3. Raw `neo4rs::Graph` opened for Cypher assertion (Neo4jSink does not expose graph field).

## Tool failures / retries
- First `cargo fmt --check` failed; `cargo fmt --all` fixed formatting automatically (line-length differences in two let-binding sites and one method-chain). No logic changes.
