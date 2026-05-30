# Business Analyst Log — graph-symbol-ids

run_id: graph-symbol-ids-4
date: 2026-05-29
agent: business-analyst
stage: 2 of 8

## Actions taken

- Read CHARTER.md, requirements.md (S1–S7), wiki pages/code/cpp-indexer and pages/code/cpp-mcp.
- Mapped all 8 dispatched edge-case categories to story IDs and gave each scenario a stable ID (S1-SC-01 … S7-SC-15).
- Wrote 40 Gherkin scenarios across 6 Features covering:
  - S1: SQLite map (4 scenarios)
  - S2: LRU cache + concurrency (10 scenarios)
  - S3: Config surface (6 scenarios)
  - S4: Both-sink integer write (7 scenarios)
  - S5: Read path ID resolution (4 scenarios)
  - S6: Migration/compatibility (4 scenarios)
  - S7: Tests (15 scenarios)
- Marked 4 open questions (OQ-1 through OQ-4) as needs-clarification.
- Did not resolve OQ-4 (hard-error vs auto-migrate — deferred to ADR per role boundary).

## Deliverable

/Users/husam/workspace/cpp-indexer/.claude/handoff/v4/scenarios.md — 40 scenarios, 4 open questions.
