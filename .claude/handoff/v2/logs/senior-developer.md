---
run_id: cpp-indexer-m8-v2
role: senior-developer
mode: plan
created: 2026-05-18
deliverable: /Users/husam/workspace/cpp-indexer/.claude/handoff/v2/plan.md
---

# senior-developer log — M8 plan

## Inputs read
- CHARTER.md, requirements.md, scenarios.md, design.md
- adr-11 (schema bump), adr-12 (code inline 32 KiB), adr-13 (USES classifier), adr-14 (serialization), adr-15 (indexes)
- Verified all 5 ADRs `Status: accepted` (CHARTER invariant I2 satisfied).

## Stories produced
7 stories (S40–S46), preserving the requirements.md grouping. None re-split: design.md narrows file lists so S41's 8 ACs fit one cohesive callable-visitor change.

| Story | Parallel-safe | Depends on | ACs |
|---|---|---|---|
| S40 | no (foundation) | — | 6 |
| S41 | no (shares shallow.rs) | S40 | 8 |
| S42 | no (shares shallow.rs) | S40 | 4 |
| S43 | no (shares shallow.rs) | S40 | 6 |
| S44 | yes (with S45) | S40-S43 | 5 |
| S45 | yes (with S44) | S40-S43 | 5 |
| S46 | no (last) | all | 5 |

Parallel-safe pairs: 1 (S44 ↔ S45).

## Conventions note
Project is Rust, not C++. cpp-conventions skill loaded per dispatch instructions but does not apply — exit-criteria use `cargo build / clippy / fmt / test` per design.md §7. Documented this explicitly in plan.md.

## Invariant compliance
- I3 (exit-criteria commands present): every story has a `### Exit criteria` block with concrete `cargo` commands. No MISSING_EXIT_CRITERIA.
- Traceability: every story names its AC IDs and ADRs.

## Open items surfaced to developer
- S41: confirm source-buffer caching covers all TUs before the visitor reads `entity.get_range()`.
- S42: verify `clang-rs` API surface for `get_template_arguments()`.
- S44: confirm `MAX_TRANSIENT_RETRIES = 3` holds under wider SET clauses; raise + note in implementation-notes if not.
- S45: ensure ADR-12 test-cap shim applied to IndraDB memory-backend tests.

## Cognee tags
`task:cpp-indexer-m8`, `role:senior-developer`
