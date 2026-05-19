run_id: tu-parse-fail-v3

---

## Story S1 — Sanitise libclang args (story-slug: s1-sanitise-args)

### Files changed
- `src/bootstrap/compile_commands.rs`
  - Added `fn is_driver_basename(token: &str) -> bool` (deny-list predicate per ADR-1)
  - Added `fn sanitize_libclang_args(raw_args, canonical_file, directory) -> Vec<String>` (3-step algorithm per ADR-1 §Decision)
  - Modified `parse()`: calls `sanitize_libclang_args` after `resolve_args()`; `TuEntry.args` now holds the sanitised list
  - Updated `parse_valid_arguments_form` assertion (see Deviations)
  - Added 12 new unit tests covering all AC-2 table scenarios

### Tests added/run

```
cargo fmt --all -- --check                                              exit 0
cargo clippy --workspace --all-targets -- -D warnings                  exit 0
cargo test --workspace                                                  332/333 (1 pre-existing failure, see below)
cargo test -p cpp_indexer --lib bootstrap::compile_commands::tests      17/17
```

New test IDs (all in `bootstrap::compile_commands::tests`):
- `sanitize_empty_input_returns_empty`
- `sanitize_driver_only_returns_empty`
- `sanitize_driver_stems_are_stripped` (table: cc, c++, clang, clang++, gcc, g++, /usr/bin/clang++, /usr/local/bin/gcc, arm-linux-gnueabi-g++)
- `sanitize_versioned_driver_clang_18_is_kept` (see open items)
- `sanitize_source_file_as_token0_is_kept`
- `sanitize_c_flag_pair_is_stripped`
- `sanitize_o_flag_pair_is_stripped`
- `sanitize_trailing_source_repeat_absolute_is_stripped`
- `sanitize_trailing_source_repeat_relative_is_stripped`
- `sanitize_non_driver_token0_zig_is_kept`
- `sanitize_all_flags_pass_through_unchanged`

Pre-existing failure (unrelated to S1):
- `schema_drift::schema_txt_contains_all_promoted_fields` — fails on unmodified HEAD; not introduced by S1.

### Deviations from plan

1. **`parse_valid_arguments_form` updated.** Original asserted `entries[0].args[0] == "/usr/bin/c++"` (raw form). After sanitisation the driver is stripped; corrected assertion: `entries[0].args == vec!["-std=c++17"]`. Plan stated "all existing tests must continue to pass" with the intent of no regressions; this is a necessary update, not a regression.

2. **`clang-18` tests KEPT, not stripped.** The plan lists `clang-18` as a driver to strip, but ADR-1's predicate has no coverage for versioned drivers (stem `clang-18` does not end with `-gcc`/`-g++`/`-clang`/`-clang++`). Implemented ADR-1 verbatim; test documents current behaviour (kept). Tagged as open item for sr-dev.

### Follow-ups / open items

- **[sr-dev] Versioned driver support (`clang-18`, `gcc-12`, etc.):** ADR-1's predicate misses these. Options: extend suffix patterns to `*-clang-N` / `*-gcc-N`, or use a starts-with match on the stem after stripping cross-compiler prefix. Requires ADR-1 amendment.
- **[sr-dev] `clang` → `clang-18` version-suffix coverage:** scenarios.md AC-2 examples include `clang-18`; not currently matched by the deny-list. Decision on whether to add `stem.starts_with("clang-") || stem.starts_with("gcc-")` suffix check.
- **Cache invalidation on first deploy:** `manifest.json` entries hashed with raw args will miss on first re-index; documented in ADR-1 §Consequences. No action required here; devops runbook should note it.

### References
- plan.md §S1
- adr-1.md (Status: accepted)
- scenarios.md Feature S1

---

## Story S2 — Surface failed-TU counter in pipeline summary (story-slug: s2-summary)

### Files changed

- `src/pipeline/mod.rs`
  - Added field `pub failed_tu_count: usize` to `PipelineStats` (between `partial_tu_count` and `nodes_written`)
  - Added `stats.failed_tu_count = parallel_stats.tu_error.try_into().unwrap_or(usize::MAX);` immediately after `stats.partial_tu_count = …` (~line 204)
  - Added `impl PipelineStats { pub fn closing_summary(&self) -> String { … } }` returning the canonical format string
  - Added unit test `closing_summary_format` in `mod tests`

- `src/bin/index.rs`
  - Replaced ad-hoc 4-token `eprintln!` at line 178 with `eprintln!("{}", stats.closing_summary());`
  - `main` return type unchanged (`anyhow::Result<()>`) — deferred to S3 per plan

### Tests added/run

```
cargo fmt --all -- --check                                              exit 0
cargo clippy --workspace --all-targets -- -D warnings                  exit 0
cargo test --workspace closing_summary                                  1/1 PASS
cargo test --workspace                                                  PASS except pre-existing schema_drift failure (confirmed pre-existed on main)
```

### Audit step result

`rg -n "parse-summary" /Users/husam/workspace/cpp-indexer/tools/release/` → NO_MATCH.
No consumer found; no update needed.

### Deviations from plan

None.

### Follow-ups

- `schema_txt_contains_all_promoted_fields` (tests/schema_drift.rs) is pre-existing; confirmed via `git stash` verification. Tagging @sr-dev for triage.
- S3 and S4 may now proceed; both `closing_summary()` and `failed_tu_count` are available on `PipelineStats`.

### References
- plan.md §S2 (AC-4)
- adr-2.md (Status: accepted)
- src/pipeline/mod.rs
- src/bin/index.rs
- CHARTER.md

---

## Story S3 — Exit-code policy via `--fail-on-tu-error` (story-slug: s3-fail-on-tu-error)

### Files changed

- `src/bin/index.rs`
  - Added `use std::process::ExitCode`
  - Added module-private `enum FailOnTuError { Never, Ratio(f64) }` with `impl FromStr`, `impl Default`, `fn exit_code(&self, failed, total) -> u8`
  - Added `fail_on_tu_error: FailOnTuError` Clap field (long = "fail-on-tu-error", value_name = "RATIO|never", default = "1.0")
  - Changed `main` return from `anyhow::Result<()>` to `anyhow::Result<ExitCode>`
  - Changed `--version` early-return to `Ok(ExitCode::SUCCESS)`
  - Changed final return to compute threshold and return `Ok(ExitCode::from(code))`
  - Updated `cli_defaults()` in `mod tests` for new field
  - Added 14 unit tests (FromStr round-trip, invalid inputs, exit_code logic table)

- `tests/integration/cli_fail_on_tu_error.rs` (new)
  - 6 integration tests using existing `assert_cmd` dev-dep
  - Covers invalid ratio >1.0, invalid ratio <0.0, garbage string, --help, `never` accepted, `0.0` accepted

- `Cargo.toml`
  - Added `[[test]] name = "cli_fail_on_tu_error" path = "tests/integration/cli_fail_on_tu_error.rs"`

### Tests added/run

```
cargo fmt --all -- --check                                              exit 0 (pass 2 after formatter fix)
cargo clippy --workspace --all-targets -- -D warnings                  exit 0
cargo test --workspace                                                  all pass except pre-existing schema_drift failure
cargo test --test cli_fail_on_tu_error                                  6/6 PASS
```

### Deviations from plan

1. **Integration test strategy**: scenarios (a)–(e) (ratio/never threshold vs. actual TU counts) moved to unit tests on `FailOnTuError::exit_code`. Running the binary to completion requires a live backend; integration file covers CLI surface (clap parse errors, --help) via `assert_cmd`. Advisor-approved; equivalent coverage.

2. No new dependencies added — `assert_cmd` was already a dev-dep.

### Pre-existing failure

`schema_drift::schema_txt_contains_all_promoted_fields` — unchanged; not introduced by S3.

### Follow-ups

None.

### References

- plan.md §S3 (AC-5)
- adr-3.md (Status: accepted) — corrected exit_code logic (`failed == 0 → 0` short-circuit)
- scenarios.md Feature S3
- logs/developer-s3-fail-on-tu-error.md

---

## Story S4 — Daemon job-status field + back-compat (story-slug: s4-daemon-status)

### Files changed

- `src/api/jobs.rs`
  - Added `pub enum JobOutcome { Completed, CompletedWithErrors, Failed }` with `#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]` and `#[serde(rename_all = "snake_case")]`
  - Added `Deserialize` to `JobRecord`, `JobState`, `JobPhase` (required for AC-7 legacy JSON round-trip test to compile)
  - Added `pub failed_tu_count: u64` with `#[serde(default)]` to `JobRecord` (AC-7 back-compat)
  - Added `pub status: Option<JobOutcome>` with `#[serde(default, skip_serializing_if = "Option::is_none")]` to `JobRecord` (AC-6)
  - Updated `JobRecord::new` to initialise both new fields (`failed_tu_count: 0`, `status: None`)
  - Extended `mark_done_with_counts` signature: added `failed_tu_count: u64` after `tus_total` per ADR-4 §3; derives `status` from `failed_tu_count` vs `tus_total`
  - Updated existing `mark_done_sets_progress_to_1` test to pass 5 args (was 4)
  - Added 8 new unit tests (outcome transitions, zero-TU edge case, AC-7 legacy round-trip, queued/running no-status, completed JSON shape)
  - `mark_failed` left untouched per ADR-4 §5

- `src/bin/daemon.rs`
  - Updated single `mark_done_with_counts` call site (~line 135) to pass `stats.failed_tu_count.try_into().unwrap_or(u64::MAX)` as the new argument

- `Cargo.toml`
  - Registered `[[test]] name = "api_jobs_status" path = "tests/integration/api_jobs_status.rs" required-features = []`

- `tests/integration/api_jobs_status.rs` (new)
  - 5 integration tests: three `JobOutcome` transitions with wire JSON assertions; AC-7 legacy JSON round-trip; in-flight records omit `status` key

### Tests added/run

```
cargo fmt --all -- --check                                              exit 0
cargo clippy --workspace --all-targets -- -D warnings                  exit 0
cargo test --lib "api::jobs::tests"                                    17/17 PASS (8 new S4 tests)
cargo test --test api_jobs_status                                       5/5 PASS
cargo test --workspace                                                  all PASS except pre-existing schema_drift failure
```

### Call-site audit

`rg "mark_done_with_counts" src/` — found exactly two: impl in `src/api/jobs.rs` and call site in `src/bin/daemon.rs`. No orphan callers.

### Deviations from plan

- ADR-4 §2 derives `Serialize` only on `JobOutcome`; plan.md explicitly flagged adding `Deserialize` too for round-trip tests. Done. `Deserialize` also added to `JobState` and `JobPhase` since `JobRecord` derives `Deserialize`.

### Follow-ups

None.

### References
- plan.md §S4 (AC-6, AC-7)
- adr-4.md (Status: accepted)
- src/api/jobs.rs, src/bin/daemon.rs, tests/integration/api_jobs_status.rs
- CHARTER.md

---

## Story S5 — spdlog integration smoke test (story-slug: s5-spdlog-smoke)

### Files changed

- `tests/integration/spdlog_smoke.rs` (new)
  - `#[ignore]` + `#[cfg(not(target_os = "windows"))]` single async test
  - Skips cleanly when `git` or `cmake` not on PATH, when `git clone` fails (no network), or when `cmake` configure fails (no C++ toolchain)
  - Calls `pipeline::run` in-process with `MockSink`
  - Asserts `ok_tu_count >= 6` (AC-3)

- `Cargo.toml`
  - Added `[[test]] name = "spdlog_smoke" path = "tests/integration/spdlog_smoke.rs" required-features = ["test-mock"]`

### Tests added/run

```
cargo fmt --all -- --check                                              exit 0
cargo clippy --workspace --all-targets -- -D warnings                  exit 0
cargo test --workspace                                                  PASS — spdlog_smoke absent (not compiled without test-mock)
cargo test --features test-mock --test spdlog_smoke                     1 test, shown as `ignored` (correct; runs under -- --ignored)
```

### Deviations from plan

- `required-features = ["test-mock"]` added to `[[test]]` entry: necessary because the test references `MockSink`, which is gated on `any(test, feature = "test-mock")`. Without the feature, the test binary fails to compile. The default `cargo test --workspace` does not compile or run this test, satisfying the plan requirement that `--ignored` tests must not run in the default workspace pass.

### Pre-existing failure

`schema_drift::schema_txt_contains_all_promoted_fields` — confirmed pre-existing; out of scope per dispatch.

### Follow-ups

- DevOps (deploy-notes.md): CI must install `git`, `cmake`, and a C++ toolchain before invoking `cargo test --features test-mock --test spdlog_smoke -- --ignored` on macOS arm64 and Linux x86_64 runners.

### References

- plan.md §S5 (AC-3)
- adr-1.md (sanitiser this test validates end-to-end)
- requirements.md §S5, AC-3
- design.md §3, §7

---

## Story S1-QD1 — Extend is_driver_basename for versioned drivers (story-slug: s1-qd1-versioned-drivers)

### Files changed

- `src/bootstrap/compile_commands.rs`
  - Added `fn is_versioned_driver(stem: &str) -> bool` — checks if stem is `<known-driver>-<numeric-version>`
  - Added `fn is_numeric_version(s: &str) -> bool` — validates `[0-9]+(\.[0-9]+)*` without regex
  - Extended `is_driver_basename` to call `is_versioned_driver` after the existing literal/suffix checks
  - Replaced `sanitize_versioned_driver_clang_18_is_kept` unit test with `_is_stripped` (now asserts stripped)
  - Added 5 new unit tests: `gpp_12_is_stripped`, `gcc_10_is_stripped`, `clangpp_18_1_is_stripped`, `clang_tidy_is_not_stripped`, `clang_format_is_not_stripped`

- `tests/integration/qa_boundary.rs`
  - Removed `#[ignore = "QD-1"]` from `qa_versioned_driver_clang_18_is_stripped`
  - Removed `#[ignore = "QD-1"]` from `qa_versioned_driver_gpp_12_is_stripped`
  - Updated doc comments to reflect QD-1 as resolved

### Tests added/run

```
cargo fmt --all -- --check                                              exit 0
cargo clippy --workspace --all-targets -- -D warnings                  exit 0
cargo test --workspace                                                  all PASS; qa_versioned_driver_clang_18_is_stripped PASS; qa_versioned_driver_gpp_12_is_stripped PASS; qa_clang_tidy_is_not_stripped PASS
```

PRE_EXISTING failure: `schema_drift::schema_txt_contains_all_promoted_fields` — confirmed on unmodified main HEAD via `git stash` + rerun. Not introduced by this story.

### Deviations from plan

None.

### Follow-ups

- PRE_EXISTING: `schema_drift::schema_txt_contains_all_promoted_fields` — tagged @sr-dev.

### References

- plan.md (story s1-qd1-versioned-drivers)
- adr-1.md §Decision (driver basename match)
- tests/integration/qa_boundary.rs QD-1 section
- src/bootstrap/compile_commands.rs
