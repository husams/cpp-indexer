run_id: tu-parse-fail-v3
story-slug: s2-summary
role: developer
date: 2026-05-19

## Skills loaded
- rust-conventions (loaded; Cargo.toml + *.rs present)

## Skills considered but not loaded
- python-conventions — no .py files involved
- cpp-conventions — not modifying C++ code
- implement-story — task is directly dispatched; story details in plan.md

## Commands run

| Command | Outcome |
|---------|---------|
| `rg -n "parse-summary" .../tools/release/` | NO_MATCH — no consumer found |
| `git stash` + `cargo test --test schema_drift` | confirmed `schema_txt_contains_all_promoted_fields` fails on unmodified HEAD (pre-existing) |
| `git stash pop` | S2 changes restored |
| `cargo fmt --all -- --check` | exit 0 |
| `cargo clippy --workspace --all-targets -- -D warnings` | exit 0 |
| `cargo test --workspace closing_summary` | 1/1 PASS (`pipeline::tests::closing_summary_format`) |
| `cargo test --workspace` | PASS (334 total; 1 pre-existing ignore: schema_drift) |

## Changes made

### src/pipeline/mod.rs
1. Added `pub failed_tu_count: usize` field to `PipelineStats` (between `partial_tu_count` and `nodes_written`).
2. Added threading line after `stats.partial_tu_count = …`:
   `stats.failed_tu_count = parallel_stats.tu_error.try_into().unwrap_or(usize::MAX);`
3. Added `impl PipelineStats { pub fn closing_summary(&self) -> String }` with the exact format from ADR-2 §3.
4. Added `closing_summary_format` snapshot test in `mod tests`.

### src/bin/index.rs
1. Replaced 4-token `eprintln!` with `eprintln!("{}", stats.closing_summary());`.
2. `main` signature kept as `anyhow::Result<()>` — S3 changes it.

## Deviations from plan
None.

## Pre-existing failures
- `schema_drift::schema_txt_contains_all_promoted_fields` — present on unmodified `main`. Not introduced by S2. Flagged to sr-dev.

## Exit gate result
- BUILD_FAIL: cleared (formatter exit 0)
- LINT_FAIL: cleared (clippy exit 0)
- TEST_FAIL: cleared (all S2-relevant tests pass; pre-existing schema_drift failure is not S2-owned)
