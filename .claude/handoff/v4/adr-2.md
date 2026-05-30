# ADR-2: SCHEMA_VERSION bump for the integer-ID graph representation

Status: accepted

## Context

ADR-9 (the existing, governing project ADR) requires: *any change to node/edge representation bumps
`SCHEMA_VERSION` in the SAME PR* (CHARTER constraint C2). Replacing USR strings with integer IDs on
nodes and edges is exactly such a change. A CI gate (`tests/schema_version_bump.rs`) already enforces
that structural changes to `nodes.rs`/`edges.rs` are paired with a bump.

`src/schema/version.rs` currently holds three version-coupled constants:

- `SCHEMA_VERSION: u32 = 5`
- `SCHEMA_VERSION_TAG: &str = "cxg-schema-v5"` (kept in sync by a `debug_assert!` and a unit test)
- `PARQUET_MAGIC: &str = "cxg_parquet_v5"` (Phase 3 refuses mismatched-version shards)

The current schema (v5) refuses pre-v5 graphs at handshake. The Neo4j sink writes a `SchemaVersion`
singleton node; Phase 5 (`check_schema_version`) and the daemon read it back and refuse on tag
mismatch (`SCHEMA_VERSION_TAG`).

## Decision

1. **Bump `SCHEMA_VERSION` from 5 to 6** in `src/schema/version.rs`, in the same commit as the first
   integer-ID write to a sink (S4-SC-07, C2, ADR-9).
2. **Update the two derived constants in lock-step:** `SCHEMA_VERSION_TAG = "cxg-schema-v6"` and
   `PARQUET_MAGIC = "cxg_parquet_v6"`. The existing `assert_version_tag_consistent` /
   `schema_version_tag_matches_integer` / `parquet_magic_contains_version` tests already guard this;
   they will fail the build if any constant is left at v5.
3. **Add a `version.rs` doc-comment line** recording the bump rationale ("graph stores integer
   symbol/file IDs instead of USR/path strings; per-repo SQLite map is the USR↔id authority"),
   matching the existing per-milestone changelog convention in that file.
4. **No dual-write / no v5↔v6 compatibility shim in the graph.** A v6 binary refuses a v5 graph at
   handshake (the migration path is governed by ADR-4, this run: reset/re-index).

## Alternatives considered

- **(a) Do not bump; treat integer IDs as an additive, backward-compatible change.** Rejected:
  violates C2/ADR-9 mechanically (the CI gate fails), and is semantically wrong — a v5 reader
  expects `usr` properties that no longer exist, so a v6 graph read by v5 tooling would silently
  return empty/garbled symbol names (C7 silent-corruption risk).
- **(b) Bump only `SCHEMA_VERSION` and let the tag/magic drift.** Rejected: the in-tree consistency
  tests fail the build, and Phase 5 / daemon handshake compares the *tag*, so a drifted tag would
  pass version checks it should fail.
- **(c) Jump the version number (e.g. to 10) to leave headroom.** Rejected: the project uses a
  strictly monotonic `+1` convention (v1→v5 each milestone); a non-contiguous jump breaks the
  `parquet_magic_contains_version`/changelog narrative for no benefit.

## Consequences

- Positive: existing CI gate and consistency tests enforce correctness with zero new test
  infrastructure; the handshake refusal (already present) becomes the migration safety net (ADR-4).
- Positive: clear, greppable v6 marker across graph, Parquet, and handshake.
- Negative: every existing v5 graph must be re-indexed (no in-place upgrade) — see ADR-4; documented
  in `runbook.md` per S6-SC-04.
- Follow-up: the `SchemaVersion` node's `attrs_json` (built by `schema_version_attrs`) automatically
  carries `version: 6` / `tag: cxg-schema-v6` once the constants change — no separate code change.

## References

- ADR-9 (project, governing bump policy); CHARTER constraint C2; scenarios S4-SC-07, S6-SC-03
- `src/schema/version.rs`; `tests/schema_version_bump.rs`; `src/resolve/cross_repo.rs::check_schema_version`
- ADR-4 (this run, migration); cognee tags: `task:graph-symbol-ids`, `role:architect`
