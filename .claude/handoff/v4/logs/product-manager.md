# product-manager session log — graph-symbol-ids

## Sources read
- CHARTER.md (handoff/v4/CHARTER.md) — project facts, cross-stage invariants, failure taxonomy
- wiki index.md — confirmed [[pages/code/cpp-indexer]], [[pages/code/cpp-mcp]], [[pages/planning/compact-cpp-graph-storage]] are relevant
- Feature brief (dispatch payload) — authoritative requirements text

## Decisions recorded
- D1 (per-repo ID namespace) and D2 (both sinks) treated as ACCEPTED fixed inputs; not re-litigated
- C1–C7 each mapped to ≥1 AC across S1–S7
- C1 (read path) scoped to cpp-mcp tools + daemon with open question to enumerate further consumers in design phase
- C7 (migration/compat) left default-path (reset vs auto-migrate) as open question for architect/ADR phase
- Success criterion for graph-size reduction set at ≥30% total node+edge storage bytes on a representative repo (spdlog or {fmt}); explicit and measurable
- S5 (read path) marked P1 not P0 — can be parallelised against S4 in dev phase

## Skills loaded
- None (all source material available as local files and dispatch payload)

## Problems / gaps
- Exact integer field names for node/edge IDs (symbol_id, src_id, dst_id, file_id) left to design phase — not a PM decision
- Default SQLite path convention left to design phase
- Full enumeration of read-path consumers beyond cpp-mcp + daemon left to design phase
