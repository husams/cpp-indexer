# ADR-012: Optional graph accelerators and the custom-store gate

- Status: accepted as a qualification policy
- Date: 2026-07-24
- Scope: optional derived graph projections for local CIDX storage
- Related: HSE-75, HSE-77, HSE-78, HSE-79, HSE-81

## Decision

SQLite remains the authoritative source of CIDX semantic facts, evidence, and
projection metadata. Optional graph accelerators may exist only as local,
embedded, immutable or atomically replaceable, content-addressed, disposable,
and reproducibly rebuildable artifacts governed by the HSE-79 manifest policy.

The public boundary is a focused QueryPlan/application-service port. A
projection never exposes an engine-specific API or makes its database-local
IDs a semantic identity. Exported identity remains the USR/type key/artifact
identity; any projection-local IDs use an explicit generation-specific mapping.

The checked-in HSE-81 evaluation contract retains a SQLite-only baseline and a
deliberately unsuitable projection. A recommendation to do nothing is valid
when the named HSE-75 workload meets its SLOs. No native or custom store is a
mandatory infrastructure service.

## Reopening the custom-primary-store gate

A custom authoritative store may be proposed only when the same named workload
reproduces an exact failed SLO after HSE-77/HSE-78 optimization and a derived
accelerator cannot address the bottleneck. The evidence must include total
end-to-end cost, transactions, crash recovery, integrity, migrations, backup,
inspection, concurrency, query planning, packaging, and multi-year
compatibility. An ADR and explicit user decision are required before expanding
ownership.

## Consequences

Deleting an accelerator cannot destroy authoritative facts; a missing or stale
projection falls back to SQLite or returns a documented unknown/slow-path
result. Rebuilds are compared by content identity, not file path or local row
IDs. The policy adds artifact metadata and qualification work, but prevents a
fast traversal measurement from hiding publication, recovery, duplication, or
maintenance costs.
