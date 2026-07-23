# ADR-011: SQLite physical layout and runtime profile

- Status: accepted for schema v34
- Date: 2026-07-23
- Scope: CIDX core `index.db` connection/runtime behavior and qualification
- Related: [HSE-75](https://linear.app/hsenussi/issue/HSE-75), HSE-76, HSE-77,
  HSE-79, HSE-82

## Decision

CIDX keeps SQLite as the authoritative single-file store for schema v34. The
runtime contract is explicit and machine-readable in
`docs/storage/sqlite-profile-v1.json` and is implemented by the `SqliteDb`
profiles:

- indexing/build, migration, and maintenance use rollback journaling with
  `synchronous=FULL`, `foreign_keys=ON`, and a 5-second busy timeout;
- interactive reads and replay use a read-only connection with
  `foreign_keys=ON`, `query_only=ON`, and a 5-second busy timeout;
- read-only connections never set journal mode, synchronous, page size, cache,
  mmap, or statistics state;
- WAL is a qualification candidate only. It is not a shipped default because
  its sidecar/checkpoint and cross-process atomicity costs are not justified by
  an unmeasured concurrency claim;
- `Storage::run_maintenance()` and `Storage::refresh_statistics()` make
  maintenance/statistics mutation explicit;
- `Storage::backup_to()` uses SQLite's online backup API. A restored file is
  accepted only after schema/catalog/fact-set identity and integrity checks.

## Physical layout qualification

Existing unique B-trees are reused when their leading columns match a hot
traversal. Separate indexes are retained only for the reverse direction or a
distinct configuration/evidence access path. The profile names the strategy
for every required relation in both directions. In particular:

- `edge`, `def_edge`, and `entity_edge` use source and destination indexes;
- `possible_call` uses its unique source-leading key forward and a destination
  index reverse;
- `type_edge` uses its `WITHOUT ROWID` primary key forward and a destination
  index reverse;
- `include_edge` uses its source/path/configuration unique key forward,
  destination index reverse, and configuration index for applicability;
- `edge_site` and `include_site` use their edge-leading primary/index paths.

The existing nullable `entity_edge` identity expression index is retained for
schema-v34 compatibility. A normalized sentinel representation is a separate
HSE-77 identity decision and is not duplicated here.

## Qualification and recovery

Run the reproducible qualifier against a v34 database:

```text
uv run --project python python benchmarks/storage_m1/qualify.py \
  --db index.db --profile docs/storage/sqlite-profile-v1.json \
  --output /tmp/cidx-storage-m1.result.json
```

The result records PRAGMA state, database/table/index bytes, every named query
SQL/parameters/plan/row count/latency, rollback-vs-WAL measurements, read-only
side-effect checks, backup/restore identity, and interruption recovery probes.
The qualifier fails on an unexpected scan for a strategy that requires an
index, a read-only mutation/sidecar, identity mismatch, failed integrity/FK
checks, or a recovery state presented as current.

The current schema version remains 34. No migration is needed for this
runtime-only change, and old v34 databases retain their existing semantics.

## Consequences

The profile is intentionally conservative: cache size, mmap, temp-store,
page-size, auto-vacuum, and WAL decisions remain benchmark variables rather
than ambient process defaults. A later schema/layout change must update the
machine-readable strategy table and provide before/after evidence through
HSE-75 without silently changing the production profile.
