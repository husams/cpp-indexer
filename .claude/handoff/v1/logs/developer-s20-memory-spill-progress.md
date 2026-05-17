# Developer Log — S20: memory-spill-progress

## Skills loaded
- rust-conventions (loaded before any code was written)

## Skills considered but not loaded
- cpp-conventions: not applicable (Rust project)
- implement-story: not loaded; task dispatch already provided a scoped story section

## Orientation
- Read CHARTER.md, plan.md S20 section, design.md, adr-7.md, adr-8.md
- ADR-8 is C++20 modules (not spill threshold); 8 GiB threshold is in ADR-7 §Memory budget
- Read existing per_repo.rs, progress.rs, resolve/mod.rs, pipeline/mod.rs, parallel.rs, nodes.rs, Cargo.toml
- rocksdb = "0.22" already in dependencies; no new deps needed
- NodeKind has no Into<u8>/TryFrom<u8>; used try_from_arrow_str for RocksDB serialisation
- Reporter was a no-op stub; pipeline/mod.rs did not use it (only pub mod progress declaration)
- Called advisor before writing any code

## Commands run

| Command | Outcome |
|---|---|
| `cargo check --all-targets --all-features` | clean |
| `cargo fmt --all -- --check` | FAIL (line-length formatting in 3 files) |
| `cargo fmt --all` | fixed |
| `cargo fmt --all -- --check` | pass |
| `cargo clippy --all-targets --all-features -- -D warnings` | FAIL: `manual_is_multiple_of` lint in spill.rs (2 sites) |
| Fixed: replaced `count % BATCH_SIZE == 0` with `count.is_multiple_of(BATCH_SIZE)` | |
| `cargo clippy --all-targets --all-features -- -D warnings` | pass |
| `cargo nextest run -p cpp_indexer --lib -E 'test(resolve::spill) or test(pipeline::progress)'` | 15/15 PASS |

## Exit gate passes
- Pass 1: fmt fail (fixed); clippy fail (fixed)
- Pass 2: all gates green

## Deviations from plan
- `NodeMeta` stays in `per_repo.rs` (not moved to `spill.rs`) to avoid circular imports; `spill.rs` imports it from `per_repo`.
- ADR-8 resolved to C++20 modules, not the spill threshold. The 8 GiB value came from ADR-7 §Memory budget.
- `classify_edge` now returns `Result<()>` (was `()`) because `UsrMap::contains_key` is fallible for the RocksDB variant.
- `pipeline/mod.rs` was not updated: Reporter is declared but not yet wired into Phase 1 (the existing pipeline loop uses `stats.cache_hits += 1` directly). Wiring Reporter into the parallel pipeline is a follow-up for the story that integrates S17 parallel + S20 progress — marking as open item below.
- The nextest filter `resolve::spill pipeline::progress` (space-separated) was interpreted as an E-expression; used `-E 'test(resolve::spill) or test(pipeline::progress)'` syntax.

## Follow-ups (tag: sr-dev)
1. Wire `Reporter` into `pipeline::run` and `pipeline::parallel::run_phase1_parallel` so stderr progress appears during real runs. The reporter API is ready; call sites need a `Reporter::new(tu_entries.len())` + `tick_cache_hit()` / `tick_tu(nodes, edges)` per TU.
2. `UsrMap::get()` is defined but not called in `write_resolved_edges` (only `contains_key` is used). If Phase 4/5 needs `NodeMeta` from the spilled map, the `get()` path is available.
3. No integration test for Phase 3 + spill path end-to-end (through `resolve_per_repo_with_threshold`); plan.md listed this; added as a QA follow-up.

## References
- plan.md S20 (lines 356–368)
- design.md §Phase 3, §5.4
- adr-7.md §Memory budget, §Progress reporting
- adr-8.md (confirmed: C++20 modules, not spill)
- requirements.md AC-M3-12, AC-M3-13, AC-M3-14
