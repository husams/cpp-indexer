# Developer Log — S35-daemon-metrics

## Skills loaded
- rust-conventions

## Skills considered but not loaded
- implement-story: not loaded; direct implementation was straightforward from plan.md S35 and ADR-5
- cpp-conventions: not applicable (Rust project)
- python-conventions: not applicable

## Commands run + outcomes

| Command | Outcome |
|---------|---------|
| `git worktree list` | Confirmed worktree at `/Users/husam/workspace/cpp-indexer/.worktrees/s35-daemon-metrics` on branch `story/s35-daemon-metrics` |
| `git -C .worktrees/s33-daemon-rest status` | Branch clean; no prior API code in s33 |
| `git -C .worktrees/s34-daemon-reset status` | s34 has uncommitted `src/api/mod.rs` (declares only `pub mod reset;`) + Cargo.toml with `sha2`/`hex` deps |
| `cargo fmt --all -- --check` (pass 1) | FAIL — formatting differences in jobs.rs and metrics.rs |
| `cargo fmt --all` | Fixed formatting |
| `cargo fmt --all -- --check` (pass 2) | PASS (exit 0) |
| `cargo clippy --all-targets --all-features -- -D warnings` | PASS (exit 0); ~4 min compile (first build) |
| `DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo nextest run -p cpp_indexer api::metrics` | PASS — 3/3 tests pass |
| `DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo nextest run -p cpp_indexer api::` | PASS — 5/5 tests pass (includes api::jobs:: tests) |
| `git add ... && git -c commit.gpgsign=false commit -m "S35: daemon metrics"` | Committed `b275ebe` on `story/s35-daemon-metrics` |

## Named exit gates

1. BUILD_FAIL (formatter): CLEAR after pass 2 (`cargo fmt` auto-fixed whitespace)
2. LINT_FAIL (clippy): CLEAR — 0 warnings
3. TEST_FAIL (nextest api::metrics): CLEAR — 3/3 PASS

## Key design decisions

- Used dedicated `prometheus::Registry` (not `default_registry()`) to avoid pollution from other test metrics.
- `CXG_QUEUE_DEPTH` is a `std::sync::LazyLock<IntGauge>` process-global so `JobQueue::try_enqueue` can update it without holding a registry reference.
- `cxg_libclang_errors_total` from `crate::metrics` is registered via `.clone()` — the original `OnceLock`-backed counter continues to work in indexer code.
- `metrics_router()` returns a minimal sub-Router; full composition + auth layers are S33's responsibility.

## Deviations from plan

- Exit-criteria command requires `DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib` on macOS (pre-existing infra gap; libclang not on default dyld path). Tests pass with it set.
- `JobPayload` is `serde_json::Value` (opaque); S33 will replace with concrete ingest body struct.

## Tool failures / retries

- First nextest run without `DYLD_LIBRARY_PATH` → exit 104 (SIGABRT in `m6_agent_gate` and `phase1_base` binaries during `--list` phase). Fixed by setting `DYLD_LIBRARY_PATH`.
- cargo fmt pass 1 failed (formatting); auto-fixed with `cargo fmt --all`; pass 2 succeeded.
