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
- named forward/reverse relation plans, parameters, row counts, truncation,
  and repeated latency samples;
- rollback-journal versus WAL on identical temporary copies;
- read-only side-effect checks;
- online backup/restore identity plus integrity and foreign-key checks;
- interrupted DML, DDL/migration-shaped, and post-commit recovery probes.

The production gate is conservative: rollback/FULL remains current, WAL is
reported as qualification-only, and any missing index plan, mutation on
read-only open, identity mismatch, or invalid recovery state fails the run.
