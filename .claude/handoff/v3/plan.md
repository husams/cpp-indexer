# Plan — Issue 0001 fix (tu-parse-fail)

run_id: tu-parse-fail-v3
stage: 4 of 8 — senior-developer
upstream: requirements.md (S1–S5, AC-1…AC-7), design.md, adr-1..adr-4.md (all Status: accepted)
downstream: developer reads this; one story at a time per dependency order

Toolchain (rust-conventions, per CHARTER project_root):
  - `cargo fmt --all -- --check`
  - `cargo clippy --workspace --all-targets -- -D warnings`
  - `cargo test --workspace`

Project root for all relative file paths below: `/Users/husam/workspace/cpp-indexer`.

Dependency graph:
  S1 → S2 → S3
              ↘
               S5
              ↗
       S2 → S4
Sequential chain (share files / state field): S1 → S2 → S3.
Parallel-safe: S4 may run in parallel with S3 once S2 is merged (disjoint files: `src/api/jobs.rs` + `src/bin/daemon.rs` vs `src/bin/index.rs`).
S5 must follow S1–S4 (validates end-to-end fix and depends on the sanitiser, summary line, exit codes, and daemon field).

---

## Story S1 — Sanitise libclang args

Goal: Strip compiler-driver token, `-c`/`-o` pairs, and source-file repeat from `compile_commands.json` arguments before handing them to libclang. (ADR-1)

AC satisfied: AC-1, AC-2

Files to touch:
- `/Users/husam/workspace/cpp-indexer/src/bootstrap/compile_commands.rs`
  - Add free function `sanitize_libclang_args(raw_args: &[String], canonical_file: &Path, directory: &Path) -> Vec<String>` per ADR-1 §Decision.
  - Add helper `fn is_driver_basename(token: &str) -> bool` matching the literal stems `cc | c++ | clang | clang++ | gcc | g++` and the suffix patterns `*-gcc | *-g++ | *-clang | *-clang++` (basename only via `Path::file_name`).
  - Call `sanitize_libclang_args` inside `parse()` after `resolve_args()` returns; canonicalise `entry.file` once before the call; replace `TuEntry.args` with the sanitised vector. Do NOT retain the raw form.
  - Extend the in-file `#[cfg(test)] mod tests` with the AC-2 table covering at minimum: leading `cc`, `c++`, `clang`, `clang++`, `gcc`, `g++`, `clang-18`, `arm-linux-gnueabi-g++`, `zig` (not a driver — kept), `foo.cc` source-file as token-0 (kept), `-c /tmp/x.cpp` pair stripped, `-o x.o` pair stripped, trailing source-file repeat stripped (absolute path), trailing source-file repeat stripped (relative path joined to `directory`), driver-only `["clang++"]` → empty, empty input → empty.
- (Audit only) Confirm no other call site of `TuEntry.args` in the workspace depends on the raw form: `rg "\.args"` in `src/bootstrap/` and `src/pipeline/`. No code change required if consumers only read.

New files: none.

Tests:
- Unit (table) in `src/bootstrap/compile_commands.rs::tests` — AC-2.
- All existing tests in `src/bootstrap/compile_commands.rs::tests` MUST continue to pass (synthetic `/tmp/foo.cpp` paths handled by literal-fallback canonicalise).

Exit-criteria commands (all must pass):
- `cargo fmt --all -- --check`
- `cargo clippy --workspace --all-targets -- -D warnings`
- `cargo test --workspace`
- `cargo test -p <crate-containing-bootstrap> --lib bootstrap::compile_commands::tests` (story-specific: confirm the new table test runs)

Parallel-safe: no (root of dependency chain; S2 reads `failed_tu_count` produced indirectly via this fix).

Risks / Out of scope:
- Out: changes to `manifest.json` hash format. (Cache invalidation is expected one-shot; documented for devops in ADR-1 §Consequences.)
- Risk: synthetic `/tmp/...` test paths may fail to canonicalise on some hosts — fall through to literal `directory.join(t)` comparison (ADR-1 §Edge cases).

References: requirements.md §S1, AC-1, AC-2; design.md §3, §4.1; adr-1.md.

---

## Story S2 — Surface failed-TU counter in pipeline summary

Goal: Thread `parallel_stats.tu_error` into `PipelineStats.failed_tu_count` and emit the closing summary line with the new token. (ADR-2)

AC satisfied: AC-4

Files to touch:
- `/Users/husam/workspace/cpp-indexer/src/pipeline/mod.rs`
  - Add field `pub failed_tu_count: usize` to `PipelineStats` (between `partial_tu_count` and `nodes_written` — design §3 token order).
  - At line ~204 (immediately after `stats.partial_tu_count = …`), add `stats.failed_tu_count = parallel_stats.tu_error.try_into().unwrap_or(usize::MAX);`.
  - Add `impl PipelineStats { pub fn closing_summary(&self) -> String { … } }` returning exactly `format!("cxg-index: done — {} TUs | {} partial | {} failed | {} nodes | {} edges", self.tu_count, self.partial_tu_count, self.failed_tu_count, self.nodes_written, self.edges_written)`.
  - Add unit-test (snapshot) `closing_summary_format` in `src/pipeline/mod.rs::tests` asserting the exact format for `PipelineStats { tu_count: 10, partial_tu_count: 2, failed_tu_count: 1, nodes_written: 100, edges_written: 50, ..Default::default() }`.
- `/Users/husam/workspace/cpp-indexer/src/bin/index.rs`
  - Replace the ad-hoc summary `eprintln!` at line ~178 with `eprintln!("{}", stats.closing_summary());`. Do not change `main`'s return type in this story (deferred to S3).
- Audit step (no code change unless found): `rg -n "parse-summary" /Users/husam/workspace/cpp-indexer/tools/release/`. If any consumer is found, update it to accept the new `failed:` token between `partial` and `nodes` and record in implementation-notes.md.

New files: none.

Tests:
- Unit (snapshot) in `src/pipeline/mod.rs::tests::closing_summary_format`.
- Existing `PipelineStats::default()` callers in the workspace MUST continue to compile unchanged (field defaults to `0`).

Exit-criteria commands:
- `cargo fmt --all -- --check`
- `cargo clippy --workspace --all-targets -- -D warnings`
- `cargo test --workspace`
- `cargo test --workspace closing_summary` (story-specific)

Parallel-safe: no (must precede S3 and S4 — both consume `stats.failed_tu_count`).

Risks / Out of scope:
- Out: changing exit codes (S3) or daemon wire shape (S4).
- Risk: an undiscovered `parse-summary.sh` consumer may break — mitigation: explicit grep audit step above; surface to user if found.

References: requirements.md §S2, AC-4; design.md §3, §4.2; adr-2.md.

---

## Story S3 — Exit-code policy via `--fail-on-tu-error`

Goal: Add CLI flag `--fail-on-tu-error <RATIO|never>` (default `1.0`) mapping failed TUs to exit code `2` per ADR-3.

AC satisfied: AC-5

Files to touch:
- `/Users/husam/workspace/cpp-indexer/src/bin/index.rs`
  - Add module-private `enum FailOnTuError { Never, Ratio(f64) }` with `impl FromStr` matching ADR-3 §1 (case-insensitive `never`; reject NaN; require `0.0..=1.0`).
  - Add `impl Default for FailOnTuError { fn default() -> Self { Self::Ratio(1.0) } }`.
  - Add Clap field on the CLI struct: `#[arg(long = "fail-on-tu-error", value_name = "RATIO|never", default_value = "1.0")] fail_on_tu_error: FailOnTuError`. Add help text per ADR-3 §4.
  - Add `impl FailOnTuError { fn exit_code(&self, failed: usize, total: usize) -> u8 { … } }` per ADR-3 §2 (corrected version: `failed == 0 → 0`; then `ratio >= r → 2 else 0`).
  - Change `main` signature from `anyhow::Result<()>` to `anyhow::Result<std::process::ExitCode>`. After the existing `eprintln!("{}", stats.closing_summary())`, compute `let code = cli.fail_on_tu_error.exit_code(stats.failed_tu_count, stats.tu_count);` and return `Ok(std::process::ExitCode::from(code))`.

New files:
- `/Users/husam/workspace/cpp-indexer/tests/integration/cli_fail_on_tu_error.rs` (new module inside the existing `tests/integration/` test crate; wire via `tests/integration/mod.rs` if that file declares submodules).
  - If `tests/integration/mod.rs` does not aggregate submodules in a way that supports adding this file, place it as a top-level `/Users/husam/workspace/cpp-indexer/tests/cli_fail_on_tu_error.rs` instead (developer verifies layout before adding).
  - Use `assert_cmd` (already a dev-dep per design §7 — developer to `grep` Cargo.toml and add iff missing; if missing, surface as BUILD_FAIL and consult senior-developer review before adding a dep).
  - Scenarios MUST cover: (a) ratio `1.0`, all-fail fixture → exit 2; (b) ratio `0.0`, any-fail fixture → exit 2; (c) ratio `1.0`, partial-fail fixture → exit 0; (d) `never`, all-fail fixture → exit 0; (e) zero-TU input → exit 0; (f) invalid value `1.5` → clap error (non-zero, stderr matches "ratio must be in").

Tests:
- Unit: `FromStr` round-trip + invalid-input cases in `src/bin/index.rs::tests`.
- Integration: per scenarios above.

Exit-criteria commands:
- `cargo fmt --all -- --check`
- `cargo clippy --workspace --all-targets -- -D warnings`
- `cargo test --workspace`
- `cargo test --test cli_fail_on_tu_error` (story-specific; or `cargo test --workspace cli_fail_on_tu_error` if placed inside the integration crate)

Parallel-safe: no with respect to S2 (S3 depends on S2's `closing_summary` and `failed_tu_count` field). YES with respect to S4 (disjoint files).

Risks / Out of scope:
- Out: applying the same threshold to the daemon (explicitly out-of-scope per ADR-4 §Consequences).
- Risk: clap's parse-error exit code is also `2`; documented in ADR-3 — stderr disambiguates. Do not attempt to remap clap's exit code.
- Risk: adding `assert_cmd` as a new dep would violate "no new dependencies" NFR — verify presence before use.

References: requirements.md §S3, AC-5; design.md §3, §4.3; adr-3.md.

---

## Story S4 — Daemon job-status field and back-compat

Goal: Add `failed_tu_count: u64` (serde default) and `status: Option<JobOutcome>` (skip-if-none) to `JobRecord`; extend `mark_done_with_counts`; update worker call site. (ADR-4)

AC satisfied: AC-6, AC-7

Files to touch:
- `/Users/husam/workspace/cpp-indexer/src/api/jobs.rs`
  - Add `pub failed_tu_count: u64` with `#[serde(default)]` to `JobRecord` (struct at ~line 97).
  - Add `pub status: Option<JobOutcome>` with `#[serde(default, skip_serializing_if = "Option::is_none")]` to `JobRecord`.
  - Add `pub enum JobOutcome { Completed, CompletedWithErrors, Failed }` with `#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]` and `#[serde(rename_all = "snake_case")]`. (`Deserialize` added so round-trip tests work; ADR-4 only stated Serialize but round-trip tests require both.)
  - Extend `mark_done_with_counts` (around line 334) signature to add `failed_tu_count: u64` parameter (positioned after `tus_total` per ADR-4 §3). Write both `rec.failed_tu_count` and derive `rec.status` per ADR-4 §3 body (`failed == 0 → Completed`; `failed >= tus_total → Failed`; else `CompletedWithErrors`).
  - Leave `mark_failed` untouched; do not set `status` there (ADR-4 §5).
  - Extend `#[cfg(test)] mod tests` to cover: (a) three `JobOutcome` transitions via `mark_done_with_counts`; (b) `total == 0 && failed == 0` → `Completed`; (c) legacy JSON literal without `failed_tu_count` and `status` deserialises (AC-7); (d) queued/running record serialises without `status` key present.
- `/Users/husam/workspace/cpp-indexer/src/bin/daemon.rs`
  - Update the single call site at ~line 135 to pass `stats.failed_tu_count.try_into().unwrap_or(u64::MAX)` in the new argument slot.

New files:
- `/Users/husam/workspace/cpp-indexer/tests/integration/api_jobs_status.rs` (or top-level `tests/api_jobs_status.rs` if the integration submodule wiring doesn't accept new files).
  - API-level test exercising `GET /v1/jobs/{id}` end-to-end via the existing daemon test harness; assert the three `status` transitions and that the legacy record round-trips.
  - If no daemon test harness exists, use direct calls into `JobQueue` and serde to assert the wire JSON shape (acceptable per design §7 — "unit + integration").

Tests:
- Unit in `src/api/jobs.rs::tests` (above).
- Integration in the new test file.

Exit-criteria commands:
- `cargo fmt --all -- --check`
- `cargo clippy --workspace --all-targets -- -D warnings`
- `cargo test --workspace`
- `cargo test --workspace jobs::tests` and `cargo test --test api_jobs_status` (or equivalent path) — story-specific.

Parallel-safe: yes, with respect to S3. Both share no source files. (S4 still depends on S2 having merged for the `stats.failed_tu_count` field to exist.)

Risks / Out of scope:
- Out: adding a `--fail-on-tu-error`-equivalent flag to the daemon (ADR-4 §Consequences).
- Risk: any other caller of `mark_done_with_counts` outside `src/bin/daemon.rs:135` would break — design §9 risk register notes the single-call-site invariant; developer must `rg "mark_done_with_counts" src/` and update each found site.

References: requirements.md §S4, AC-6, AC-7; design.md §3, §4.4; adr-4.md.

---

## Story S5 — spdlog integration smoke test

Goal: Opt-in (`#[ignore]`) integration test that builds spdlog's `compile_commands.json` and asserts `ok_tu_count >= 6` of 7 TUs after the sanitisation fix. (Validates AC-1..AC-7 end-to-end.)

AC satisfied: AC-3

Files to touch: none of the src/ tree.

New files:
- `/Users/husam/workspace/cpp-indexer/tests/integration/spdlog_smoke.rs` (new). If the integration submodule does not accept additional files, place at `/Users/husam/workspace/cpp-indexer/tests/spdlog_smoke.rs`.
  - Attributes: `#[ignore]` and `#[cfg(not(target_os = "windows"))]`.
  - Clone `https://github.com/gabime/spdlog.git` at HEAD into a tempdir (use existing `tempfile` dev-dep — confirm via `rg tempfile Cargo.toml`).
  - Run CMake in `Release` mode with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` to produce `compile_commands.json`.
  - Invoke the indexer's pipeline directly (or `cxg-index` binary via `assert_cmd` iff already a dep; otherwise call `cpp_indexer::pipeline::run` in-process to avoid new deps).
  - Assert `stats.tu_count - stats.failed_tu_count >= 6` (i.e. `ok_tu_count >= 6` allowing one partial). Assert `stats.failed_tu_count == 0` is preferred but not required (AC-3 allows one partial).
  - Skip cleanly with a `println!` and `return` if `git` or `cmake` is not on PATH (test must not hard-fail in environments lacking build tools — CI installs them; local dev may not have them).

Tests: itself.

Exit-criteria commands:
- `cargo fmt --all -- --check`
- `cargo clippy --workspace --all-targets -- -D warnings`
- `cargo test --workspace` (the new test is `#[ignore]`, so this MUST still pass)
- `cargo test --workspace -- --ignored spdlog_smoke` (story-specific — gated; QA runs this on macOS arm64 + Linux x86_64 per AC-3).

Parallel-safe: no (depends on S1+S2+S3+S4 already on the branch to validate the full chain).

Risks / Out of scope:
- Out: CI workflow file changes (devops scope per design §3 table). Developer does NOT edit `.github/workflows/*.yml`; deploy-notes.md will pick this up.
- Risk: spdlog HEAD churn could change TU count over time. Mitigation: assert `>= 6 of 7`, not exact equality; if upstream restructures, file a follow-up issue rather than pinning a SHA in this story.
- Risk: `git`/`cmake` absent on local dev box — test prints "skip" and returns Ok per design.

References: requirements.md §S5, AC-3; design.md §3, §7; adr-1.md (validates the sanitiser).

---

## Cross-story integration checklist (developer self-check before declaring done)

- All five stories landed: `cargo test --workspace` green AND `cargo test --workspace -- --ignored spdlog_smoke` green on macOS arm64.
- `rg "mark_done_with_counts" src/` finds exactly the call site at `src/bin/daemon.rs` (and the impl in `src/api/jobs.rs`). No orphan callers.
- `rg "closing_summary" src/` finds the impl in `src/pipeline/mod.rs` and the call site in `src/bin/index.rs` only.
- `rg "parse-summary" tools/` is empty OR every match has been updated to accept the new `failed:` token (record in implementation-notes.md).
- `cxg-index --help` output includes `--fail-on-tu-error` with the help text from ADR-3 §4.
- No new entries in `Cargo.toml [dependencies]` or `[dev-dependencies]` (NFR row 1). Acceptable exception: nothing.

## References
- /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/CHARTER.md
- /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/requirements.md
- /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/design.md
- /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/adr-1.md … adr-4.md
- /Users/husam/workspace/cpp-indexer/docs/issues/0001-silent-tu-parse-failures.md
- Wiki: `[[pages/code/cpp-indexer]]`
- Cognee tag: `task:tu-parse-fail`, `role:senior-developer`
