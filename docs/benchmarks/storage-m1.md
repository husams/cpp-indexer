# Storage M1 qualification

`benchmarks/storage_m1/qualify.py` is the reproducible evidence runner for
ADR-011. It does not modify the supplied database. Writable profile and crash
probes operate on temporary copies.

```text
uv run --project python python benchmarks/storage_m1/qualify.py \
  --db index.db \
  --profile docs/storage/sqlite-profile-v1.json \
  --iterations 3 \
  --output /tmp/cidx-storage-m1.result.json
```

The result covers:

- v34 catalog/table/index facts and dbstat bytes when the SQLite build exposes
  dbstat;
- a resolved representative corpus plus every declared forward/reverse query
  ID, with parameters, row counts, truncation, and repeated latency samples;
- explicit deferred-relation evidence: `possible_call` is not declared as a
  required query because the canonical corpus has zero multi-definition
  fan-out rows; declaring it without a non-empty corpus fails qualification;
- deterministic workspace identity and full fact-content identity, including a
  same-count content-change negative check;
- rollback-journal versus WAL on identical temporary copies;
- read-only side-effect checks for a pre-existing WAL and missing sidecars;
- online backup/restore identity plus integrity and foreign-key checks;
- interrupted staging/indexing, named entity transform, migration-shaped DDL,
  WAL checkpoint, and ANALYZE maintenance probes, each classified as current
  or stale-but-valid after integrity/schema validation.

The production gate is conservative: rollback/FULL remains current, WAL is
reported as qualification-only, and any missing index plan, bogus/swapped
strategy that does not fail its negative probe, undeclared relation evidence,
persistent read-only mutation, identity mismatch, or invalid recovery state
fails the run. A pre-existing WAL `-shm` lock-state change is reported but is
not treated as persistent database/WAL mutation.
