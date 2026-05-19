# Business-analyst log — tu-parse-fail

run_id: tu-parse-fail-v3
stage: 2 of 8 — business-analyst
date: 2026-05-19

## Inputs consumed
- /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/CHARTER.md
- /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/requirements.md
- Wiki: [[pages/code/cpp-indexer]] (confirmed GET /v1/jobs/{id} route exists, M1-M8 context)

## Output produced
- /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/scenarios.md

## Coverage summary
- AC-1: 6 scenarios (happy path, pass-through prefixes, empty args, driver-only args, relative/absolute path, table-driven driver shapes)
- AC-2: 1 scenario (Scenario Outline table-driven, 9 driver shape rows)
- AC-3: 3 scenarios (smoke pass, Windows skip, CI matrix invocation)
- AC-4: 3 scenarios (failed>0 format, failed==0 format, token order stability)
- AC-5: 10 scenarios (4 AC-specified cases + threshold boundary x2 + 3 invalid input cases + 0-TU edge)
- AC-6: 4 scenarios (3 status transitions + integer type check)
- AC-7: 2 scenarios (legacy back-compat deserialise, no existing field removed)

## Open questions flagged (5)
1. total_tu_count == 0: ratio undefined, exit code and daemon status unspecified — needs-clarification, carry to architect
2. tools/release/parse-summary.sh existence — needs-clarification, verify during PR
3. --fail-on-tu-error parser shape (FromStr enum vs bool flag) — needs-clarification, architect ADR
4. Flag-prefix whitelist completeness vs "never strip -* tokens" rule — needs-clarification, architect to reconcile
5. Daemon GET /v1/jobs/{id} response struct location — needs-clarification, architect to verify

## Decisions / assumptions
- Gherkin serves as traceability artifact; QA executes as cargo test / rstest, not pytest-bdd
- Driver-token detection assumed to be basename/suffix match (not any-position)
- "never strip tokens starting with -" assumed to cover all unlisted prefixes (e.g., -O*, -m*, -g*)
