# Session log — product-manager (tu-parse-fail-v3)

date: 2026-05-19

## Inputs read

- CHARTER.md: confirmed handoff paths, toolchain, cross-stage invariants, traceability chain.
- requirements-raw.md: umbrella user story, AC-1..AC-7, NFRs, risks, DoD, S1..S5 split hint.
- docs/issues/0001-silent-tu-parse-failures.md: root cause detail (Bug A + Bug B), reproduction steps, verbatim AC text, files-to-touch.
- wiki index.md (grepped): confirmed `GET /v1/jobs/{id}` route is live per `[[pages/code/cpp-indexer]]`; no existing wiki page for Issue 0001 fix.

## Decisions

- Preserved AC-1..AC-7 verbatim as directed.
- Split one umbrella story into five stories (S1..S5) following the split hint; each story has its own AC list.
- Priority: S1/S2/S3 = P0 (root-cause + loud-failure path); S4/S5 = P1 (daemon parity + integration smoke).
- Cross-story sections retained: Scope, NFRs, Risks, DoD — these are not story-level concerns.
- Three open questions filed: tools/release/ audit, --fail-on-tu-error type shape (architect call), daemon route location.

## Problems hit

- wiki index.md too large to read in full; used grep to extract cpp-indexer / daemon API entries. Wiki is silent on Issue 0001 fix — no prior page.
- requirements-raw.md had a path typo in references section (`.claire/` vs `.claude/`) — not fixed; out of scope for product-manager role.

## Skills loaded

- advisor (pre-write orientation call)

## Cognee ingest

Ingested this log via closing protocol command below.
