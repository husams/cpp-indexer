# senior-developer log — tu-parse-fail v3

stage: 4 of 8 — senior-developer (plan mode)
deliverable: /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/plan.md

## Inputs read
- CHARTER.md (paths, invariants, failure taxonomy)
- requirements.md (S1..S5, AC-1..AC-7, NFRs, risks)
- design.md (architecture overview, files-to-touch table, algorithm specs §4.1–4.4)
- adr-1.md (sanitize_libclang_args, accepted)
- adr-2.md (PipelineStats.failed_tu_count + closing_summary, accepted)
- adr-3.md (FailOnTuError enum + exit_code mapping, accepted)
- adr-4.md (JobRecord.failed_tu_count + JobOutcome, accepted)

## Pre-conditions verified
- All four ADRs Status: accepted (I2 satisfied).
- Every story in plan.md has exit-criteria commands (I3 satisfied; no MISSING_EXIT_CRITERIA).
- Every story references the AC IDs it satisfies (traceability chain).

## Story decomposition decisions
- Kept the architect's 5-story split (S1..S5). Requirements gave it as a hint; no story is large enough to justify further split.
- Dependency graph: S1 → S2 → {S3 ∥ S4} → S5.
- Parallel-safe annotation: S3 and S4 may run in parallel after S2 merges (disjoint files: src/bin/index.rs vs src/api/jobs.rs + src/bin/daemon.rs).
- Sequential chain S1→S2→S3 shares the `failed_tu_count` field and `closing_summary` helper across src/pipeline/mod.rs and src/bin/index.rs — cannot parallelise.

## Notable plan adjustments beyond design.md
- ADR-4 said only `Serialize` for JobOutcome; plan tightens to `Serialize + Deserialize` since AC-7 round-trip tests require deserialisation of the literal too.
- Plan calls out the assert_cmd dependency check explicitly (NFR row 1 forbids new deps). Developer must verify it exists before using in S3/S5 integration tests; if missing, in-process pipeline call is the fallback for S5 and the developer must surface as a question for S3.
- For S5, hardened the skip path so the test does not hard-fail when git/cmake are absent on local dev boxes — CI matrix runs `--ignored` explicitly.

## Exit-criteria coverage
Every story includes the project-wide gate (cargo fmt --check, cargo clippy --workspace --all-targets -- -D warnings, cargo test --workspace) plus story-specific test selectors.

## Risks surfaced to next stage
- Potential undiscovered consumer of the closing summary line (tools/release/parse-summary.sh) — explicit grep audit step in S2.
- spdlog HEAD churn (S5) — mitigated by `>= 6 of 7` threshold rather than exact equality.

## Status
- I1 (acceptance criteria): satisfied (requirements.md AC-1..AC-7).
- I2 (ADR status): satisfied (all four ADRs accepted).
- I3 (exit-criteria): satisfied (every story has commands).
- I4 (test-report): not applicable at this stage.

Return: clear; deliver: /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/plan.md; stories: 5 (parallel-safe pairs: 1 — {S3, S4}).
