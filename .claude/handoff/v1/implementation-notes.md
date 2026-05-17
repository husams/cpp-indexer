# Implementation Notes — S23: cross-repo resolver

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
