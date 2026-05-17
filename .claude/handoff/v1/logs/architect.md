# Architect log — cpp-indexer v1

run_id: cpp-indexer-v1
date: 2026-05-17
stage: 3 of 8 — architect

## Deliverables

- design.md — full system design, module layout, phase contracts, traceability table
- adr-1.md — cpp-mcp boundary (resolves PRD Q5; blocker per CHARTER) — Status: accepted
- adr-2.md — GraphSink trait shape — Status: accepted
- adr-3.md — Parquet staging schema — Status: accepted
- adr-4.md — USR canonicalisation for system headers and vendored copies — Status: accepted
- adr-5.md — cxg-daemon REST contract (axum + RFC-7807 + bearer-token on writes) — Status: accepted
- adr-6.md — git2 workspace clone manager (host allowlist, PAT-via-env, shallow) — Status: accepted
- adr-7.md — Parallel ingestion (rayon + thread-local clang::Index) — Status: accepted
- adr-8.md — C++20 modules best-effort on libclang 18 (resolves PRD Q3) — Status: accepted
- adr-9.md — Cross-repo schema versioning: refuse on mismatch (resolves PRD Q4) — Status: accepted
- adr-10.md — Build-config + multi-tenant daemon defaults (resolves PRD Q2 + Q7) — Status: accepted

## Open PRD questions resolved

- Q2 (build config) → ADR-10: separate graph per `(profile, defines_hash)`.
- Q3 (C++20 modules) → ADR-8: best-effort with runtime capability probe.
- Q4 (cross-repo schema versioning) → ADR-9: refuse on mismatch.
- Q5 (cpp-mcp boundary) → ADR-1: schema lives in cpp-indexer; cpp-mcp consumes.
- Q7 (multi-tenant daemon) → ADR-10: single-tenant for v1.

## Cross-stage invariants honoured

- I2: all 10 ADRs Status == accepted (no `proposed` left). ADR_UNRESOLVED would not fire.

## Notes for senior-developer

- Plan.md must reference AC IDs per CHARTER traceability rule.
- Phase 5 advisory lock is on the GraphSink trait (ADR-2); plan stories for cxg-resolve-cross-repo must use it.
- Schema version bump CI gate (ADR-9) needs a baseline file `tests/schema-baseline.txt` checked in alongside the schema files.
- build.rs (ADR-1 + AC-M6-1..3) regenerates `prompt/graph_database/cpp/schema.txt`; CI gate `git diff --exit-code` after `cargo build` enforces it.
- Single-tenant scope (ADR-10) means M7 stories should not introduce per-tenant abstractions.

## References

- design.md
- adr-1.md through adr-10.md
- CHARTER.md
- requirements.md
- scenarios.md
