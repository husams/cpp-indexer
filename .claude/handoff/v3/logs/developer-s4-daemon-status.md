# Developer log — S4 (s4-daemon-status)

run_id: tu-parse-fail-v3
date: 2026-05-19
story: S4 — Daemon job-status field + back-compat (AC-6, AC-7)

## Skills loaded

- rust-conventions (loaded at task start)

## Skills considered but not loaded

- cpp-conventions — project is Rust, not C++
- python-conventions — no Python files in scope
- implement-story — using developer role directly per dispatch

## Orientation steps

1. Read CHARTER.md, plan.md §S4, adr-4.md
2. Read src/api/jobs.rs (full), src/bin/daemon.rs (full)
3. Checked `rg "failed_tu_count" src/pipeline/mod.rs` — S2 confirmed merged (field present)
4. Checked `rg "mark_done_with_counts" src/` — confirmed single call site in daemon.rs
5. Checked Cargo.toml [[test]] blocks + tests/ directory structure — confirmed integration tests placed under tests/integration/ with Cargo.toml [[test]] registration

## Advisor call

Called advisor before writing code. Key findings incorporated:
- `JobRecord` did not derive `Deserialize` — added to `JobRecord`, `JobState`, `JobPhase` (not just `JobOutcome`)
- Confirmed S2 merged before proceeding to daemon call site
- Confirmed integration test placement strategy: tests/integration/api_jobs_status.rs + Cargo.toml registration

## Commands run

| Command | Outcome |
|---------|---------|
| `cargo fmt --all -- --check` (pass 1) | FAIL — format diff in integration test and src/bin/index.rs (S3 pre-existing) |
| `cargo fmt --all` | Fixed format |
| `cargo fmt --all -- --check` (pass 2) | exit 0 |
| `cargo clippy --workspace --all-targets -- -D warnings` (pass 1) | FAIL — `needless_borrows_for_generic_args` in 2 locations in api_jobs_status.rs |
| Fix: removed `&` from `serde_json::to_value(&...)` calls | Fixed |
| `cargo clippy --workspace --all-targets -- -D warnings` (pass 2) | exit 0 |
| `cargo test --workspace` | All pass except pre-existing schema_drift failure |
| `cargo test --lib "api::jobs::tests"` | 17/17 PASS |
| `cargo test --test api_jobs_status` | 5/5 PASS |

## Deviations from plan

- Added `Deserialize` to `JobState` and `JobPhase` in addition to `JobOutcome` — necessary for `JobRecord: Deserialize` to compile (field enums must also deserialise).

## Pre-existing failures (not introduced by S4)

- `schema_drift::schema_txt_contains_all_promoted_fields` — pre-existed on main per S1/S2/S3 notes.

## Exit gate result

Pass 2 of 3 — all named signals clear:
- BUILD_FAIL: clear (cargo fmt --all -- --check exit 0)
- LINT_FAIL: clear (cargo clippy exit 0)
- TEST_FAIL: clear (cargo test --workspace — only pre-existing schema_drift fails)
