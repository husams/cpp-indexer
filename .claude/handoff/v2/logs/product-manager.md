---
agent: product-manager
task-slug: cpp-indexer-m8
run_id: cpp-indexer-m8-v2
date: 2026-05-18
---

# Session Log — product-manager

## Inputs read

1. `/Users/husam/workspace/cpp-indexer/.claude/handoff/v2/CHARTER.md` — run_id, blackboard paths, cross-stage invariants (I1–I4), failure taxonomy.
2. `/Users/husam/workspace/cpp-indexer/.claude/handoff/v2/requirements-raw.md` — planning brief with 7 stories S40–S46, high-level requirements, success criteria, risks, design constraints.
3. `/Users/husam/workspace/wiki/pages/planning/cpp-indexer-structured-attrs-prd.md` — full PRD: problem statement, goals G1–G6, acceptance criteria AC-S1 through AC-S7 (PRD's own numbering), worked Cypher examples Q1–Q5, risks, out-of-scope, open questions, sequencing.

## Key decisions made

### AC redistribution

PRD §5 organizes ACs under its own AC-S1–AC-S7 groupings that do not directly correspond to implementation story identifiers S40–S46. Redistributed as follows:

- S40 ← PRD AC-S3 (schema/Arrow/version) + AC-S6.1 (handshake refuses old graphs) + explicit promotion of `is_virtual`/`is_pure_virtual`/`is_static` as native fields (was implicit in PRD indexing requirement).
- S41 ← PRD AC-S1.1–S1.4 (callable extraction).
- S42 ← PRD AC-S1.5–S1.6 (template extraction, bundled separately in sequencing §10).
- S43 ← PRD AC-S2.1–S2.3 (USES classifier).
- S44 ← PRD AC-S4.1–S4.4 plus G5 EXPLAIN/NodeIndexSeek measurable.
- S45 ← PRD AC-S5.1–S5.2.
- S46 ← PRD AC-S6.2 (runbook) + S7.1–S7.2 (docs + wiki).

### AC format

PRD ACs are declarative. Converted to Given/When/Then format while preserving all load-bearing specifics: 32 KiB cap with `code_truncated: true`, cv-qualifier preservation for METHOD, exact seven enum values for association_type, composite index `(kind, return_type)`, source file paths `src/visit/shallow.rs` and `src/sink/neo4j.rs:53`.

### `is_virtual`/`is_pure_virtual`/`is_static` promotion

Advisor flagged gap: PRD implies full promotion (indexes on these in AC-S4.3) but NodeRecord field list in PRD AC-S3.1 does not enumerate them. Added explicit AC-S40-2 listing them, and raised as an open question for architect confirmation.

### Neo4j deadlock retry

PRD §7 calls this out as a live bug that will worsen with M8. Not assigned to any S40–S46 story; raised as cross-cutting open question #1 with a note that it must be resolved before S44 developer dispatch.

### Priority

- S40–S45: P0 (all on critical path to G5).
- S46: P1 (docs; does not block query correctness goals; can land last).

### Dependencies chain documented

S41/S42/S43 depend on S40. S44/S45 depend on S40+S41+S42+S43. S46 depends on all six.

## Skills loaded

None (source files were sufficient; no Cognee query needed; no wiki edit needed).

## Problems

None blocking. All open questions surfaced in requirements.md rather than resolved unilaterally.
