---
run_id: cpp-indexer-m8-v2
role: architect
created: 2026-05-18
---

# Architect log — M8 Structured Attributes

## Inputs read
- /Users/husam/workspace/cpp-indexer/.claude/handoff/v2/CHARTER.md
- /Users/husam/workspace/cpp-indexer/.claude/handoff/v2/requirements.md
- /Users/husam/workspace/cpp-indexer/.claude/handoff/v2/scenarios.md
- /Users/husam/workspace/wiki/pages/planning/cpp-indexer-structured-attrs-prd.md
- /Users/husam/workspace/cpp-indexer/.claude/handoff/v1/adr-9.md (parent versioning policy)
- src/schema/{version,nodes,edges,arrow}.rs (existing layout)
- src/sink/{neo4j,indradb}.rs (existing constants, deadlock retry already present at neo4j.rs:277,720)

## Dispatch correction
Dispatch said project is "C++ (CMake)". It is actually a Rust binary (`Cargo.toml`, `rustc`, libclang-rust). The Rust conventions apply, not C++. Confirmed via `ls /Users/husam/workspace/cpp-indexer/`. Design proceeded under Rust assumption matching existing code.

## Decisions made (5 ADRs, all accepted, continue v1 numbering past adr-10)
- ADR-11: SCHEMA_VERSION 4 → 5; full promotion (no dual-write). Resolves OQ-1.
- ADR-12: `code` inline on node with 32 KiB cap. Resolves OQ-2.
- ADR-13: USES classifier = closed 7-value enum; EXTERNAL_REF mirroring **in M8 scope**. Resolves OQ-3.
- ADR-14: Typed list-of-struct everywhere (Arrow List<Struct>, Bolt List<Map>, IndraDB per-field Json). Resolves serialization question implicit in AC-S40-2/4, AC-S42-2/3, AC-S45-1.
- ADR-15: Neo4j 4 indexes; IndraDB index_property parity; composite is v6 follow-up. Resolves OQ-5.

## Open-question resolution map
- OQ-1 → ADR-11 (no dual-write).
- OQ-2 → ADR-12 (inline + cap).
- OQ-3 → ADR-13 (EXTERNAL_REF mirror in-scope).
- OQ-4 → already implemented at neo4j.rs:277,720; documented in design.md §2 and §5. Not an ADR.
- OQ-5 → ADR-15 (v5 property indexes; composite deferred to v6).

## Status
- All 5 ADRs Status: accepted.
- design.md written.
- No ADR_UNRESOLVED.

## Deliverables
- /Users/husam/workspace/cpp-indexer/.claude/handoff/v2/design.md
- /Users/husam/workspace/cpp-indexer/.claude/handoff/v2/adr-11.md
- /Users/husam/workspace/cpp-indexer/.claude/handoff/v2/adr-12.md
- /Users/husam/workspace/cpp-indexer/.claude/handoff/v2/adr-13.md
- /Users/husam/workspace/cpp-indexer/.claude/handoff/v2/adr-14.md
- /Users/husam/workspace/cpp-indexer/.claude/handoff/v2/adr-15.md
