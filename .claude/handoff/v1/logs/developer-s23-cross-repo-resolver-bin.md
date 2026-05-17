# Developer Log — S23-cross-repo-resolver-bin

## Skills loaded
- rust-conventions

## Skills considered but not loaded
- cpp-conventions: project is Rust-only
- python-conventions: no Python files
- typescript-conventions: no TypeScript files

## Commands run + outcomes

1. Read CHARTER.md, plan.md S23, design.md, adr-4.md — orientation
2. `find /Users/husam/workspace/cpp-indexer/src -type f -name "*.rs" | sort` — file tree check
3. Read: cross_repo.rs (stub), sink/mod.rs, sink/lock.rs, sink/neo4j.rs, sink/indradb.rs, sink/mock.rs, schema/edges.rs, schema/nodes.rs, schema/version.rs, config/mod.rs, bin/index.rs, resolve/per_repo.rs, schema/arrow.rs, tests/sink_indradb.rs — orientation
4. Called advisor — confirmed approach, key risk surfaced: GraphSink has no query methods for Phase 5
5. `grep -n "acquire_phase5_lock..." sink/*.rs` — confirmed lock and read_schema_version already implemented in both neo4j.rs and indradb.rs
6. `cargo build` → FAIL (ParquetFileArrowReader wrong API, Vec<NodeRecord> not Result) — fixed to use `ParquetRecordBatchReaderBuilder::try_new` pattern matching per_repo.rs
7. `cargo build` → PASS (1 unused import warning) — cleaned up
8. `cargo fmt --all -- --check` → FAIL (formatting diffs) — `cargo fmt --all` applied
9. `cargo clippy --all-targets --all-features -- -D warnings` → FAIL (2 unused imports in tests/cross_repo.rs) — fixed
10. `cargo clippy --all-targets --all-features -- -D warnings` → PASS
11. `cargo nextest run -p cpp_indexer --test cross_repo` → FAIL (mock module gated; missing Cargo.toml [[test]] entry) — added `required-features = ["test-mock"]` and registered test
12. `cargo nextest run -p cpp_indexer --test cross_repo --features test-mock` → FAIL (libclang.dylib not in DYLD_LIBRARY_PATH)
13. `DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo nextest run ... --features test-mock -- --ignored` → 8/8 PASS
14. `git add -A && git -c commit.gpgsign=false commit -m "S23: cross-repo resolver"` → committed ab738d5

## Exit gate results
- Formatter: `cargo fmt --all -- --check` → PASS
- Linter: `cargo clippy --all-targets --all-features -- -D warnings` → PASS
- Tests: `cargo nextest run -p cpp_indexer --test cross_repo --features test-mock -- --ignored` → 8/8 PASS

## Deviations from plan.md
1. Phase 5 uses Parquet-based global USR map instead of DB queries. GraphSink trait has no query methods. SR-dev tagged.
2. ExternalRef EdgeKind added but SCHEMA_VERSION not bumped — sr-dev to confirm ADR-9 policy.
3. tests/cross_repo.rs registered in Cargo.toml with required-features = ["test-mock"] (pattern from other tests).

## Tool failures or retries
- Pass 1: Wrong Parquet API (ParquetFileArrowReader) → replaced with ParquetRecordBatchReaderBuilder
- Pass 2: Clippy unused imports in test file → fixed
- Pass 3: Mock module not visible to integration test → added Cargo.toml [[test]] entry + feature flag
