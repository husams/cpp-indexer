run_id: tu-parse-fail-v3
story-slug: s3-fail-on-tu-error
role: developer
date: 2026-05-19

## Skills loaded
- rust-conventions

## Skills considered but not loaded
- implement-story — task was fully dispatched with plan.md; skill not needed.
- cpp-conventions — project is Rust only.
- simplify — no duplication identified; not applicable.

## Commands run

| Command | Outcome |
|---------|---------|
| `cargo check --bin cxg-index` | ok (after FailOnTuError + Cli field added) |
| `cargo fmt --all -- --check` | FAIL pass 1 (cli_fail_on_tu_error.rs formatting) |
| `cargo fmt --all` | applied; fixed |
| `cargo fmt --all -- --check` | ok pass 2 |
| `cargo clippy --workspace --all-targets -- -D warnings` | ok |
| `cargo test --workspace` | FAIL — pre-existing `schema_txt_contains_all_promoted_fields` only |
| `cargo test --test cli_fail_on_tu_error` | ok — 6/6 |

## Files changed

- `src/bin/index.rs`
  - Added `use std::process::ExitCode;`
  - Added module-private `enum FailOnTuError { Never, Ratio(f64) }` with `FromStr`, `Default`, `exit_code` method
  - Added `fail_on_tu_error: FailOnTuError` Clap field on `Cli` struct
  - Changed `main` return type from `anyhow::Result<()>` to `anyhow::Result<ExitCode>`
  - Changed `--version` early-return from `Ok(())` to `Ok(ExitCode::SUCCESS)`
  - Changed final `Ok(())` to compute exit code via `fail_on_tu_error.exit_code(stats.failed_tu_count, stats.tu_count)` and return `Ok(ExitCode::from(code))`
  - Updated `cli_defaults()` in `mod tests` to include `fail_on_tu_error: FailOnTuError::default()`
  - Added 14 unit tests: `FromStr` round-trip (valid and invalid), `exit_code` logic table (ratio 1.0 all-fail, ratio 1.0 partial-fail, ratio 0.0 any-fail, ratio 0.0 no-fail, Never all-fail, Never no-fail, zero-TU edge, boundary met, boundary below, default is ratio 1.0)

- `tests/integration/cli_fail_on_tu_error.rs` (new)
  - 6 integration tests using `assert_cmd` (already a dev-dep)
  - Covers: invalid ratio >1.0 (clap error + stderr mentions "ratio must be in"), invalid ratio <0.0 (non-zero exit), garbage string (clap error), `--help` includes `--fail-on-tu-error` and `RATIO|never`, `never` accepted without clap error, `0.0` accepted without clap error

- `Cargo.toml`
  - Added `[[test]] name = "cli_fail_on_tu_error" path = "tests/integration/cli_fail_on_tu_error.rs"`

## Deviations from plan

1. **Integration test strategy** (advisor-guided): plan.md stated scenarios (a)–(e) as integration tests. Because the binary requires a live database backend to complete the pipeline, those scenarios are infeasible without `#[ignore]` gates. Per advisor recommendation: moved (a)–(e) logic to unit tests on `exit_code()` inside `src/bin/index.rs::tests`; integration tests cover CLI surface (clap parse errors, `--help`) only. This provides equivalent coverage without infra dependency and matches the spirit of the plan.

2. `default_value = "1.0"` on the Clap field triggers clap's `FromStr`-based auto-derivation. No explicit `value_parser` annotation was required; `cargo check` confirmed clap derives it correctly.

## Pre-existing failure (noted per dispatch)

`schema_txt_contains_all_promoted_fields` in `tests/schema_drift.rs` — pre-existing failure unrelated to S3. Confirmed unchanged by this story.

## Follow-ups

None. No new dependencies added. `assert_cmd` was already in `[dev-dependencies]`.

## References

- plan.md §S3
- adr-3.md (corrected logic: `failed == 0 → 0` short-circuit)
- scenarios.md Feature S3
- CHARTER.md
