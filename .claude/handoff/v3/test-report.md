run_id: tu-parse-fail-v3
stage: 6 of 8 — qa-engineer
upstream: implementation-notes.md (all five stories S1–S5 implemented)

---

Scope: Issue 0001 fix — sanitise libclang args, surface failed-TU counter,
       exit-code policy, daemon status field, spdlog smoke test (S1–S5, AC-1–AC-7, AC-3)

Test plan: unit | integration | parametrised/boundary (QA additions) | spdlog smoke (gated)

---

## Commands run

```
cargo fmt --all -- --check                                                   exit 0
cargo clippy --workspace --all-targets -- -D warnings                        exit 0
cargo test --workspace                                                        exit 101 (1 pre-existing failure — see PRE_EXISTING below)
cargo test -p cpp_indexer --lib bootstrap::compile_commands::tests            exit 0   17/17
cargo test --workspace closing_summary                                        exit 0    1/1
cargo test --test cli_fail_on_tu_error                                        exit 0    6/6
cargo test --lib "api::jobs::tests"                                           exit 0   17/17
cargo test --test api_jobs_status                                             exit 0    5/5
cargo test --features test-mock --test spdlog_smoke -- --ignored              exit 0    1/1
cargo test --test qa_boundary -- qa_versioned_driver_clang_18_is_stripped     exit 0    1/1 (QD-1 re-verify)
cargo test --test qa_boundary -- qa_versioned_driver_gpp_12_is_stripped       exit 0    1/1 (QD-1 re-verify)
cargo test --test qa_boundary                                                 exit 0  7 passed / 0 ignored (QD-1 resolved)
```

QD-1 re-verification run date: 2026-05-19

---

## Results

| Story | AC(s) | Test location | Scenario ID | Result |
|-------|-------|---------------|-------------|--------|
| S1 | AC-1, AC-2 | bootstrap::compile_commands::tests (17 tests) | Feature S1 @AC-1 @AC-2 | PASS |
| S2 | AC-4 | pipeline::tests::closing_summary_format | Feature S2 @AC-4 | PASS |
| S3 | AC-5 | tests/integration/cli_fail_on_tu_error.rs (6 tests) + unit tests in index.rs (14 tests) | Feature S3 @AC-5 | PASS |
| S4 | AC-6, AC-7 | api::jobs::tests (17 tests) + tests/integration/api_jobs_status.rs (5 tests) | Feature S4 @AC-6 @AC-7 | PASS |
| S5 | AC-3 | tests/integration/spdlog_smoke.rs (gated #[ignore]) | Feature S5 @AC-3 | PASS (1/1 on macOS arm64) |
| QA | AC-2, AC-5, AC-6 | tests/integration/qa_boundary.rs (7 tests) | S1@AC-2, S3@AC-5, S4@AC-6 | 7 PASS / 0 IGNORED (QD-1 resolved) |

Workspace total: ~464 passed / 1 failed (PRE_EXISTING) / 12 ignored (live-service gates only)

---

## PRE_EXISTING (not a QA_DEFECT — confirmed pre-dates this branch)

- `schema_drift::schema_txt_contains_all_promoted_fields`
  - Confirmed pre-existing by developer on all five story passes (git stash verification documented in implementation-notes.md S1, S3, S4, S5).
  - Out of scope for this fix; tagging @sr-dev for triage.
  - NOT counted as open QA_DEFECT.

---

## Defects

- defect-id: QD-1
  scenario-id: Feature S1 @AC-2 @confirmed (Scenario Outline: Table-driven — driver shapes; plan.md S1 §AC-2 table row `clang-18`)
  failing-command: cargo test --test qa_boundary -- qa_versioned_driver_clang_18_is_stripped 2>&1
  exit-code: 0 (re-verified 2026-05-19 — both QD-1 tests PASS, #[ignore] removed)
  description: >
    RESOLVED. Developer (story s1-qd1-versioned-drivers) added `is_versioned_driver` and
    `is_numeric_version` helpers to `src/bootstrap/compile_commands.rs`, extending
    `is_driver_basename` to match versioned stems (clang-18, g++-12, clang++-18.1, etc.).
    QA re-ran both gating tests independently:
      cargo test --test qa_boundary -- qa_versioned_driver_clang_18_is_stripped  → PASS
      cargo test --test qa_boundary -- qa_versioned_driver_gpp_12_is_stripped    → PASS
    Full qa_boundary suite: 7/7 PASS, 0 ignored.
  status: closed

---

## Clarification items resolved by implementation

These `@needs-clarification` scenarios from scenarios.md were resolved during developer implementation:

- **Zero-TU ratio (Open question 1):** `FailOnTuError::exit_code(0, 0)` returns `0` for any ratio
  via the `failed == 0` short-circuit (unit tests `zero_tu_ratio_0`, `zero_tu_ratio_1` in index.rs::tests).
  Scenario Feature S3 @AC-5 "Zero TUs processed with --fail-on-tu-error 0.0" is resolved: exit 0.

- **Daemon 0/0 status (Open question 1 for S4):** `mark_done_with_counts(id, 0, 0, ...)` produces
  `status = Completed` (unit test `outcome_completed_when_zero_tus` in api::jobs::tests).
  Scenario Feature S4 @AC-6 "Job completed with zero total TUs and zero failures" is resolved: "completed".

- **`tools/release/parse-summary.sh` (Open question 2):** `rg -n "parse-summary"` returned NO_MATCH;
  no consumer exists, no update needed (documented in implementation-notes.md S2).

---

## Observations (advisory only — do not block dispatch)

- `tests/integration/cli_fail_on_tu_error.rs` line 69 has a redundant double-check of
  `!output.status.success()` — the second assert is dead code. Minor quality nit; does not affect correctness.

- `spdlog_smoke` requires `required-features = ["test-mock"]` due to `MockSink` gating.
  DevOps must note that `cargo test --features test-mock --test spdlog_smoke -- --ignored` is the
  correct CI invocation (deploy-notes.md should capture this — it is a devops concern).

- The `ExitPolicy` mirror in `qa_boundary.rs` duplicates production logic. If `FailOnTuError` is ever
  made `pub(crate)` or extracted to `src/lib.rs`, the QA mirror should be removed.

---

## Additions made

Category: **parametrised / boundary / mutation** (option 3 per QA role contract).

File: `tests/integration/qa_boundary.rs` (new, 7 tests):

1. `qa_versioned_driver_clang_18_is_stripped` — #[ignore = "QD-1"] boundary test documenting the
   clang-18 gap. Unignored when developer fixes `is_driver_basename`.
2. `qa_versioned_driver_gpp_12_is_stripped` — #[ignore = "QD-1"] same gap for g++-12.
3. `qa_clang_tidy_is_not_stripped` — guard: ensures a versioned-driver fix doesn't over-match
   non-compiler tools like `clang-tidy`. PASSING.
4. `qa_exit_code_ratio_just_above_threshold_exits_2` — floating-point ε boundary for
   `FailOnTuError::exit_code`; tests ratio 0.25±ε vs actual 0.25. PASSING.
5. `qa_exit_code_parametrised_table` — 10-row parametrised table covering all AC-5 confirmed
   scenarios plus epsilon boundaries. PASSING.
6. `qa_job_outcome_json_round_trip_all_variants` — serialise/deserialise lossless round-trip
   for all three `JobOutcome` variants. PASSING.
7. `qa_failed_tu_count_is_json_integer_in_all_transitions` — parametrised across (7,0), (7,3),
   (7,7) combos; asserts `failed_tu_count` is a JSON integer (not float/string). PASSING.

---

## References

- scenarios.md: /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/scenarios.md
- implementation-notes.md: /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/implementation-notes.md
- plan.md: /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/plan.md
- Cognee tags: task:tu-parse-fail, role:qa-engineer
