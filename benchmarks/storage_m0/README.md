# Storage M0 benchmark foundation

This directory defines the reproducible v34 SQLite benchmark contract for HSE-75.
It deliberately separates a workload plan from materialization: the 1M/5M/10M
node and 10M/50M/100M/500M relation scales are declared in the manifest, while
tests use the bounded `smoke` scale.

The generator writes the checked-in cidx schema and adds only `benchmark_meta`
for generation state and semantic digests. The runner records database/table/
index bytes, freelist/WAL/journal facts, query SQL/parameters/plans/row counts/
truncation/latency distributions, integrity and foreign-key checks, and measured
warm/update/transform/migration/backup/recovery operations on isolated copies.
SQLite prepare/step/write counters are reported as unsupported because the
Python sqlite3 API does not expose those counters; trace statement and
transaction counts remain separately labeled.

Run from the repository root with the Python project environment:

```bash
uv run --project python python -m benchmarks.storage_m0.generator \
  --manifest benchmarks/storage_m0/manifests/storage-m0-v1.json \
  --workload synthetic --scale smoke --output /tmp/cidx-storage-m0.db

uv run --project python python -m benchmarks.storage_m0.run \
  --db /tmp/cidx-storage-m0.db \
  --manifest benchmarks/storage_m0/manifests/storage-m0-v1.json \
  --workload synthetic \
  --profile benchmarks/storage_m0/profiles/local-macos-v1.json \
  --output /tmp/cidx-storage-m0.result.json

uv run --project python python -m benchmarks.storage_m0.gate \
  --result /tmp/cidx-storage-m0.result.json \
  --profile benchmarks/storage_m0/profiles/local-macos-v1.json
```

To run the intentionally bad-layout probe, copy a baseline with the hot
indexes removed, run it with `--configuration drop_hot_indexes`, and pass both
result files to `gate.py` with `--baseline` and `--bad-config`. The gate checks
that the manifest/profile/query set match and that the bad result is measurably
slower by the configured factor.

Use `--max-rows` and `--evidence-max-rows` as explicit safety caps when
materializing a large plan. A capped result retains requested and actual counts
so it cannot be mistaken for a full-scale measurement.

The custom-store gate is intentionally conservative. A decision of `propose`
must name an exact failed SLO and include evidence for schema/tuning and derived
accelerator alternatives plus engineering and compatibility costs. Each
alternative is recomputed against its artifact-bound profile: its canonical
run/identity/configuration, semantic database digest, query samples, and exact
failed-SLO set must match the signed evidence. Relabeled or duplicate
measurements, `not_run` compatibility checks, and placeholder or non-substantive
cost records are rejected.

The deterministic v34 smoke contract is checked in at
`benchmarks/storage_m0/baselines/v34-smoke.json`; its HSE-74 claim-to-result
map is `docs/benchmarks/storage-m0-hse74-citations.json`.
