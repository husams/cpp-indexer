# Implementation Notes — S20: memory-spill-progress

## Files changed
- `src/resolve/spill.rs` — new: `UsrMap` enum-dispatch (InMemory / Spilled-RocksDB); injectable threshold; `DEFAULT_SPILL_THRESHOLD_BYTES = 8 GiB`; NUL-separated `kind_str\x00repo_name` serialisation for RocksDB values
- `src/resolve/per_repo.rs` — updated: `build_usr_map` now uses `UsrMap` instead of bare `HashMap`; added `resolve_per_repo_with_threshold(stage_dir, threshold)` public entry point for test injection; `classify_edge` now returns `Result<()>` (RocksDB path is fallible)
- `src/resolve/mod.rs` — added `pub mod spill;`
- `src/pipeline/progress.rs` — full implementation replacing the stub: atomic counters (`AtomicU64`), background thread waking every 5 s, `tick_tu(nodes, edges)`, `tick_cache_hit()` (synchronous, AC-M3-14), `add_nodes/add_edges` batch helpers

## Tests added / run

```
cargo nextest run -p cpp_indexer --lib -E 'test(resolve::spill) or test(pipeline::progress)'
# 15/15 PASS
```

Tests in `resolve::spill`:
- `meta_roundtrip_function`, `meta_roundtrip_class`, `meta_from_bytes_empty_returns_none`
- `insert_and_contains_key_in_memory`, `get_returns_correct_meta_in_memory`
- `spill_triggered_by_low_threshold` — threshold=1 triggers RocksDB (no 8 GiB allocation)
- `data_survives_spill`, `insert_after_spill_readable`
- `estimated_bytes_zero_entries`, `estimated_bytes_grows_linearly`

Tests in `pipeline::progress`:
- `cache_hit_counted_synchronously` — AC-M3-14: synchronous increment on `tick_cache_hit()`
- `tick_tu_accumulates_counts`, `add_nodes_and_edges_helpers`
- `background_thread_terminates_on_drop`, `reporter_emits_progress_line_within_interval`

Exit criteria:
- `cargo fmt --all -- --check` — pass
- `cargo clippy --all-targets --all-features -- -D warnings` — pass
- nextest filter — 15/15 pass

## Deviations from plan
- `NodeMeta` kept in `per_repo.rs` (not duplicated in `spill.rs`) to avoid circular import; `spill.rs` imports it from `per_repo`.
- `Reporter` not yet wired into `pipeline::run` or `pipeline::parallel` call sites (see follow-ups).

## Follow-ups
- sr-dev: wire `Reporter` into Phase 1 call sites (`pipeline::run` sequential + `pipeline::parallel::run_phase1_parallel`)
- qa-engineer: add integration test for `resolve_per_repo_with_threshold` with spill triggered on a real Parquet fixture

## References
- plan.md S20, design.md §Phase 3 §5.4, adr-7.md §Memory budget §Progress reporting, requirements.md AC-M3-12..14
