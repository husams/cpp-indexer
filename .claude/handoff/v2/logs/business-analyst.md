---
run_id: cpp-indexer-m8-v2
role: business-analyst
task-slug: cpp-indexer-m8
created: 2026-05-18
---

# Business Analyst Log — M8 Structured Node Attributes

## Inputs consumed

- `/Users/husam/workspace/cpp-indexer/.claude/handoff/v2/requirements.md` — 190 lines, 7 stories (S40–S46), 39 ACs total.
- `/Users/husam/workspace/cpp-indexer/.claude/handoff/v2/CHARTER.md` — cross-stage invariants and failure taxonomy.
- `~/workspace/wiki/index.md` — wiki page index for reference cross-links.

## Output

- `/Users/husam/workspace/cpp-indexer/.claude/handoff/v2/scenarios.md` — Gherkin scenarios, pytest-bdd-compatible, tagged with AC IDs.

## Scenario coverage summary

| Story | ACs | Scenarios written | Notes |
|-------|-----|-------------------|-------|
| S40   | 6   | 6 (+ 1 Outline, 9 examples) | AC-S40-4 is an Arrow round-trip coverage gate |
| S41   | 8   | 6 (+ 1 Outline) | AC-S41-3/4 merged as Outline; AC-S41-8 is a coverage gate |
| S42   | 4   | 4 | AC-S42-4 is a coverage gate |
| S43   | 6   | 5 (+ 1 Outline, 4 examples) | AC-S43-6 modelled as Outline |
| S44   | 5   | 5 (+ 1 Outline, 4 examples) | AC-S44-5 is a regression gate |
| S45   | 5   | 6 (2 parity cross-sink scenarios added) | AC-S45-4 is a regression gate |
| S46   | 5   | 5 | — |

## Open questions surfaced (5)

1. OQ-1: is_virtual/is_pure_virtual/is_static promotion completeness (S40 AC dependency).
2. OQ-2: code storage model — node-embedded vs sidecar blob (S41 AC dependency).
3. OQ-3: EXTERNAL_REF USES classification mirroring (S43 scope).
4. OQ-4: Neo4j deadlock retry fix as S44 prerequisite.
5. OQ-5: IndraDB v5 index limitation — attempt now or document.

## Boundaries explicitly modelled

- 32 KiB exactly (≤ boundary, code populated, not truncated).
- 32 KiB + 1 byte (> boundary, code None, code_truncated true, no panic).
- Old SCHEMA_VERSION at handshake → SchemaVersionMismatch, not panic/generic I/O.
- Unclassifiable cursor → unknown, log entry, edge not dropped.
- Exactly 7 access-mode values permitted; no 8th value escapes.
- No double-write of promoted field to attrs_json.
- code_truncated true on IndraDB: no test failure.
- Neo4j EXPLAIN plan: NodeIndexSeek asserted, AllNodesScan/NodeByLabelScan negated.
- Sink parity: Neo4j top-level props == IndraDB vertex property keys (node + edge).

## Decisions

- AC-S41-3 and AC-S41-4 merged into a single Scenario Outline (method vs free function, cv-qualifier column).
- AC-S41-5 and AC-S41-6 written as separate scenarios (exact-limit and over-limit are distinct behaviors).
- AC-S43-6 modelled as Scenario Outline with read/write/call_arg/unknown rows.
- AC-S44-3 modelled as Scenario Outline with index-name column + separate idempotency scenario.
- Two sink-parity scenarios added (not explicit ACs but implied by parity requirement across AC-S44-1/S45-1 and AC-S44-2/S45-2).
- Coverage-gate ACs (AC-S40-4, AC-S41-8, AC-S42-4, AC-S44-5, AC-S45-4) surfaced as behavioral scenarios where possible; noted as coverage gates in this log.
