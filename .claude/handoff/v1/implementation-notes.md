# Implementation Notes

## S23-cross-repo-resolver-bin

Files changed:
- `src/schema/edges.rs` — added `ExternalRef` variant to `EdgeKind` (as_str, all(), try_from_arrow_str)
- `src/resolve/mod.rs` — exposed `pub mod cross_repo`
- `src/resolve/cross_repo.rs` — new: Phase5Options, Phase5Stats, run(), check_schema_version(), detect_repo_backends(), check_backend_homogeneity(), build_global_usr_map(), materialise_external_refs(), collect_shards()
- `src/bin/resolve_cross_repo.rs` — fleshed out: clap CLI → factory::create → phase5_run
- `Cargo.toml` — registered `[[test]] name = "cross_repo"` with `required-features = ["test-mock"]`
- `tests/cross_repo.rs` — new: 8 tests (5 non-ignored fixture tests + 3 ignored live-DB tests)

Tests added/run:
- `cargo fmt --all -- --check` → PASS
- `cargo clippy --all-targets --all-features -- -D warnings` → PASS
- `DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo nextest run -p cpp_indexer --test cross_repo --features test-mock -- --ignored` → 8/8 PASS (3 live-DB ignored tests pass via early-return guard when NEO4J_URI is absent)

Deviations from plan:
1. **Parquet-based resolution instead of DB queries.** design.md §Phase 5 says "Builds global Usr → (repo, node_id) map by querying the configured DB." `GraphSink` has no query methods (list_nodes, list_edges, etc.), so Phase 5 reads from the staged Parquet files that Phase 3 produced. This is functionally equivalent for the test fixture cases and avoids a breaking trait extension. Tagged sr-dev for review. If DB-side querying is required (e.g. for repos whose Parquet staging was cleaned up), a `GraphSink::query_cross_repo_candidates` method should be added in a follow-up story.
2. **`EXTERNAL_REF` EdgeKind added in S23.** EdgeKind::ExternalRef is in `all()` so Arrow schema is extended. sr-dev should confirm whether a SCHEMA_VERSION bump is required for the S23 PR or can wait until the next schema-affecting story per ADR-9.
3. **`required-features = ["test-mock"]` added for `cross_repo` test binary.** Follows the existing pattern used by `m1_exit_gate`, `m2_exit_gate`, `incremental_cache`, and `repo_meta` tests.

Follow-ups:
- [sr-dev] S24 `resolve::canonical` will extend `cross_repo.rs` with ADR-4 system-header canonicalisation
- [sr-dev] Confirm SCHEMA_VERSION bump required for `ExternalRef` variant addition (ADR-9)
- [sr-dev] If Phase 5 DB-side querying is needed (for cleaned-up staging dirs), add `GraphSink::query_cross_repo_candidates` + `GraphSink::query_repo_nodes`
- [devops] `DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib` required for macOS CI runs of `--test cross_repo`

References: plan.md S23, design.md §Phase 5, adr-2.md §lock, adr-4.md, adr-9.md

---

## S24-syshdr-canonicalisation

Files changed:
- `src/resolve/canonical.rs` (new) — path-rule matcher + 16 unit tests
- `src/resolve/mod.rs` — added `pub mod canonical`
- `src/resolve/cross_repo.rs` — added `canonical_overrides` field to `Phase5Options`, wired `canonical_repo()` into `build_global_usr_map`, updated `UsrEntry` doc-comment
- `src/bin/resolve_cross_repo.rs` — added `..Default::default()` to `Phase5Options` literal
- `tests/cross_repo.rs` — added `..Default::default()` to all six `Phase5Options` literals (struct literal exhaustiveness)

Tests added/run:
- `cargo fmt --all -- --check` → PASS
- `cargo clippy --all-targets --all-features -- -D warnings` → PASS
- `DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo nextest run -p cpp_indexer resolve::canonical` → 16/16 PASS

Deviations from plan:
1. Plan test description said "USR under `/usr/include/**` → `repo:system:libstdc++`" — ADR-4 says `/usr/include/c++/**` → `system:libstdc++` and `/usr/include/**` (plain) → `system:libc`. Implemented per ADR-4. Tests reflect ADR-4 semantics.
2. Plan said "under compiler-internal path → `repo:system:libc`" — ADR-4 says `lib/clang/*/include/**` → `system:clang-builtins`. Implemented per ADR-4.
3. Plan used `repo:system:*` prefix notation; ADR-4 uses `system:*` (no `repo:` prefix for system headers, `repo:vendored:*` for vendored). Implemented per ADR-4.
4. ADR-4 mentions file name `canonicalise.rs`; plan.md and exit criteria reference `resolve::canonical`. Used `canonical.rs` to match exit criteria filter.
5. Config override list exposed as `Phase5Options::canonical_overrides: Vec<CanonicalOverride>` (in-memory) rather than wired to a `[cross_repo]` TOML section. TOML round-trip is a follow-up tagged `sr-dev`.

Follow-ups:
- [sr-dev] Add `[cross_repo]` TOML section to `config/mod.rs` with `canonical_path_rules` and `canonical_priority` per ADR-4 §Implementation.
- [devops] Tests require `DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib` on macOS because llvm@18 is not installed via Homebrew.

References: plan.md S24, adr-4.md, design.md §Phase 5

---

## S25-m4-two-repo-gate

Files changed:
- `tests/fixtures/two_repo/lib-b/include/lib_b.h` (new)
- `tests/fixtures/two_repo/lib-b/src/lib_b.cpp`   (new)
- `tests/fixtures/two_repo/lib-b/compile_commands.json` (new)
- `tests/fixtures/two_repo/lib-a/src/main.cpp`    (new)
- `tests/fixtures/two_repo/lib-a/compile_commands.json` (new)
- `tests/integration/m4_exit_gate.rs`             (new)
- `Cargo.toml` — added `[[test]] name = "m4_exit_gate"` with `required-features = ["test-mock"]`

Tests added/run:
- `cargo fmt --all -- --check` → PASS
- `cargo clippy --all-targets --all-features -- -D warnings` → PASS
- `DYLD_LIBRARY_PATH=... cargo nextest run -p cpp_indexer --features test-mock` → 221 passed, 22 skipped
- `DYLD_LIBRARY_PATH=... cargo nextest run -p cpp_indexer --features test-mock --test m4_exit_gate -- --ignored` → PASS (early-returns when NEO4J_URI unset; full live-DB path runs when env vars present)

Deviations from plan:
1. **Synthetic Parquet shards instead of real libclang CALLS edges (tagged sr-dev).**
   Phase 1 (`shallow.rs`) does not visit `CallExpr` entities — `cursor_map.rs` maps `EntityKind::CallExpr` to `None` and the visitor skips it. Indexing the two-repo fixture through `pipeline::run` produces zero `cross_repo_candidate=true` CALLS edges. To satisfy AC-M4-10 (`via=CALLS`) today, the test constructs synthetic Parquet shards with `kind=Calls` cross_repo_candidate edges, then writes both repos' nodes to Neo4j and runs Phase 5. The real C++ fixture trees document the intended shape but are not parsed in the live test. CallExpr emission is M5/S26 work.

2. **Raw `neo4rs::Graph` for Cypher assertion.**
   `Neo4jSink` does not expose the inner `Graph` field publicly. The test opens a second `Graph` connection using the same env-var credentials. No production code was modified.

3. **Cypher uses `[:EDGE {kind:"EXTERNAL_REF"}]` not `[:EXTERNAL_REF]`.**
   The Neo4j sink stores all edges as `[:EDGE]` relationships with a `kind` string property (per `CQL_MERGE_EDGES` in `sink/neo4j.rs`). The Cypher assertion uses `MATCH p=()-[:EDGE {kind:"EXTERNAL_REF"}]->()` instead of the plan's `MATCH p=()-[:EXTERNAL_REF]->()`. Semantically equivalent given the current sink schema. Noted for QA review.

Follow-ups:
- [sr-dev] Add CallExpr visiting to Phase 1 so the two-repo fixture can be indexed end-to-end and the synthetic Parquet workaround removed (M5/S26 scope).
- [sr-dev] Consider exposing `Neo4jSink::graph()` accessor or a `GraphSink::raw_query` helper for test isolation.
- [qa] Confirm whether AC-M4-9 Cypher (`[:EXTERNAL_REF]`) is intended as shorthand or requires migration to typed relationships.

References: plan.md S25, requirements.md AC-M4-9 / AC-M4-10, tests/cross_repo.rs (write_fixture_shards pattern), src/resolve/cross_repo.rs, src/visit/shallow.rs, src/visit/cursor_map.rs, src/sink/neo4j.rs
