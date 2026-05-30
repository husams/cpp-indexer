# ADR-4: Existing USR-string graph — reset / re-index, not auto-migrate

Status: accepted

## Context

This resolves OQ-4 (escalated from S6 / requirements.md). When a v6 (integer-ID) `cxg-index` or
`cxg-daemon` binary encounters an existing v5 (USR-string) graph, it must behave in a way that never
silently corrupts data (C7). The choice is between:

- **(a) hard-error on version mismatch, require an explicit reset/re-index**, or
- **(b) auto-migrate the existing graph in place.**

Relevant existing precedent in the tree:

- `src/schema/version.rs`: *"Pre-v5 graphs are refused at handshake."* The house style for a schema
  representation change is already refuse-on-mismatch.
- `src/resolve/cross_repo.rs::check_schema_version`: on tag mismatch it returns `Error::Schema(...)`
  naming both expected and actual tag and telling the operator to "re-index with a matching binary".
  Phase 5 and the daemon already gate on `SCHEMA_VERSION_TAG`.
- The sinks expose `reset(ResetTarget::Repo|All)` (`CQL_RESET_REPO`/`CQL_RESET_ALL`).

Why auto-migration is not merely undesirable but **infeasible** here: a v5 graph stores USR strings
and *no SQLite id-map exists for it*. To convert it to v6 we would have to assign integer IDs to
every existing node/edge, build the `symbols`/`files` tables from the graph, and rewrite every node
and edge property in place. That is the same work as a re-index (and riskier, since it mutates the
only copy of the durable graph). There is no cheap in-place path.

## Decision

1. **Hard-error on SCHEMA_VERSION/tag mismatch (option a).** On startup / at the existing handshake
   points, a v6 binary that reads a `SchemaVersion` node with tag `!= "cxg-schema-v6"` (e.g.
   `cxg-schema-v5`) returns an explicit `Error::Schema` that:
   - names the expected tag (`cxg-schema-v6`) and the actual tag found,
   - states that the graph is in the old USR-string format and is incompatible, and
   - instructs the operator to reset and re-index (pointing at the runbook recipe).
   This reuses and extends the existing `check_schema_version` pattern; no new gate type is invented.
2. **Reset is explicit and operator-driven, never automatic.** Re-indexing into the same sink first
   requires an explicit `reset` (`POST /v1/reset` on the daemon, or `--reset`/equivalent on
   `cxg-index`, using the existing `reset(ResetTarget)` plumbing). The destructive step is gated on an
   explicit operator action; if data will be deleted, the operator has chosen it (C7, S6-SC-02).
3. **No dual-write, no v5↔v6 compatibility shim** (consistent with ADR-2 and the existing "no
   dual-write; full promotion" precedent from S40).
4. **Runbook (S6-SC-04)** documents the upgrade procedure: stop writers → `reset` the repo (or `All`)
   → re-index with the v6 binary → fresh graph is fully integer-ID (S6-SC-03). The runbook is owned
   downstream (devops/doc-writer) but the recipe shape is fixed here.

## Alternatives considered

- **(b) Auto-migrate in place.** Rejected: infeasible without effectively re-indexing (no id-map
  exists for a v5 graph); mutating the sole durable copy is the highest-risk possible path and a C7
  hazard if interrupted; and it contradicts the established refuse-at-handshake house style.
- **(c) Side-by-side migration (read v5 graph, write a new v6 graph in a new namespace).** Rejected
  as default: still must traverse the entire old graph and re-derive IDs, i.e. re-index cost, while
  adding namespace-management complexity and doubling storage transiently. Offers no advantage over
  re-indexing from source, which is the canonical, already-tested path. May be revisited if an
  operator genuinely cannot re-run `cxg-index` against source — recorded as a future option, not
  this release.
- **(d) Silent best-effort read (treat missing `usr` as empty).** Rejected outright: this is exactly
  the silent corruption C7 forbids (S5-SC-03) — callers would get blank/garbled symbol names.

## Consequences

- Positive: zero risk of in-place corruption; reuses existing handshake + `reset` plumbing; one clear
  operator story; consistent with the "pre-v5 graphs refused" precedent.
- Positive: the read-path missing-ID guard (S5-SC-03) and the handshake guard share one philosophy —
  explicit error, never fabricate.
- Negative: operators with large v5 graphs must pay a full re-index. Acceptable: re-index is the
  supported, idempotent operation the tool exists to perform; the runbook documents it.
- Follow-up: (i) extend the startup/handshake check on the write path (currently the explicit
  mismatch error lives in Phase 5 / read paths) so `cxg-index` also refuses to *append* v6 data onto a
  v5 graph without `reset`; (ii) read-path resolver returns explicit error on missing/unknown ID
  (S5-SC-03), same philosophy.

## References

- requirements.md S6, OQ-4; scenarios S6-SC-01..04, S5-SC-03; constraint C7
- `src/schema/version.rs` ("Pre-v5 graphs are refused"); `src/resolve/cross_repo.rs::check_schema_version`
- `src/sink/mod.rs` / `src/sink/neo4j.rs` (`reset`, `CQL_RESET_REPO`, `CQL_RESET_ALL`)
- ADR-2 (schema bump to v6); cognee tags: `task:graph-symbol-ids`, `role:architect`
