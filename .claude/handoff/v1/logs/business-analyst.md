# Business Analyst Log — cpp-indexer v1

run_id: cpp-indexer-v1
stage: 2 of 8 — business-analyst
date: 2026-05-17
status: complete

## Inputs read

- `/Users/husam/workspace/cpp-indexer/.claude/handoff/v1/CHARTER.md`
- `/Users/husam/workspace/cpp-indexer/.claude/handoff/v1/requirements.md` (v1.0, 115 AC IDs)
- `~/workspace/wiki/index.md` (orientation)

## Deliverable

`/Users/husam/workspace/cpp-indexer/.claude/handoff/v1/scenarios.md`

## Coverage

- 115 AC IDs (AC-M1-1 through AC-M7-27) — all covered by ≥1 tagged Gherkin scenario.
- Boundary cases from dispatch: empty repo (OQ-BA-2, needs-clarification), missing compile_commands.json (confirmed), malformed USR (OQ-BA-1, needs-clarification), sink failure (confirmed), bearer-token auth bypass (confirmed), host allowlist miss (confirmed).
- 4 open questions raised (OQ-BA-1 through OQ-BA-4).

## Open questions raised

- OQ-BA-1: Malformed non-empty USR returned by libclang — behaviour unspecified by PM.
- OQ-BA-2: Empty repo with no C++ source files — behaviour unspecified.
- OQ-BA-3: AC-M3-1 "measurably less than sequential" lacks a numeric bound; QA cannot automate.
- OQ-BA-4: AC-M7-25 and AC-M7-27 are 7-day soak criteria; cannot be pytest-bdd; manual/operational harness needed.

## Blockers noted

- All M6 scenarios marked needs-clarification pending Q5 ADR (adr-1.md). Hard blocker per CHARTER I2 before M6 developer dispatch.
- M5-S3 (C++20 modules) scenarios conditional on Q3 ADR.
- M4-S2 schema-mismatch scenario conditional on Q4 ADR.

## Tags

task:cpp-indexer role:business-analyst
