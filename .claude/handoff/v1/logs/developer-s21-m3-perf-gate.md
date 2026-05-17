# Developer log — S21-m3-perf-gate

## Skills loaded
- rust-conventions

## Skills considered but not loaded
- cpp-conventions — project is Rust only
- python-conventions, typescript-conventions, go-conventions — not applicable
- implement-story — not loaded; task was direct (bench file, no complex story state machine needed)

## Commands run

| Command | Outcome |
|---|---|
| `ls /Users/husam/workspace/cpp-indexer/` | orientation — found benches/, tests/, Cargo.toml |
| `ls /Users/husam/workspace/cpp-indexer/benches/` | found sink_throughput.rs (pattern to mirror) |
| `grep -n "AC-M3" requirements.md` | extracted exact thresholds for AC-M3-3/4/5/10/11 |
| `cargo build --bin cxg-index` | verified binary builds; located at target/debug/cxg-index |
| `cargo fmt --all -- --check` (pass 1) | exit 1 — 4 formatting diffs |
| `cargo fmt --all` | auto-fixed formatting |
| `cargo fmt --all -- --check` (pass 2) | exit 0 |
| `cargo clippy --all-targets --all-features -- -D warnings` | exit 0 |
| `BENCH=1 cargo bench --bench llvm_index` | exit 0 — "skipped (set CXG_M3_LLVM_PATH...)" |
| `git add ... && git commit` | committed as "S21: M3 perf gate", SHA 0f46b24 |

## Deviations from plan.md
- Double env-var gate (BENCH + CXG_M3_LLVM_PATH): plan says `#[ignore]` but bench harness is `harness = false` (no `#[test]`), so `#[ignore]` is not applicable. Env-var gate in `main()` achieves the same effect.
- `libc` added as dev-dependency (not in plan) to support `getrusage` on Linux for AC-M3-11 RSS measurement.

## Tool failures / retries
- `cargo fmt --all -- --check` failed on first pass (4 line-length wrapping diffs). Fixed by running `cargo fmt --all` then re-checking. No further failures.
