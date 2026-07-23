# Storage M0: benchmark, scale, and SLO contract

HSE-75 establishes the measurement boundary for the v34 storage decision. It
does not select a replacement store. Every result is tied to:

1. the versioned manifest and SHA-256 digest;
2. schema version 34;
3. a recorded local hardware/SQLite profile;
4. a deterministic seed and workload topology; and
5. a machine-readable result artifact.

## Workload matrix

The checked-in manifest covers 1M, 5M, and 10M symbol/type/definition-like node
plans; 10M, 50M, 100M, and 500M relation plans; evidence multipliers from 3x to
20x; balanced, fan-in, fan-out, chain, diamond, cyclic, and skewed degree
distributions; repository/component/TU topology; and self-index/banking corpus
references. The banking checkout is a runtime input and is not vendored.

The generator never expands a large plan implicitly. `--max-rows` and
`--evidence-max-rows` are recorded in the output, and the actual counts are
reported separately from requested counts.

## Measurement contract

`result.schema.json` requires storage accounting, query evidence, operation
statuses, counters, recovery checks, semantic digest/equivalence fields, and
gate fields. SQLite `dbstat` is used when available for per-table and per-index
bytes; page-count/freelist and WAL/journal measurements remain available as
fallbacks. Query records retain SQL text, bound parameters, `EXPLAIN QUERY PLAN`,
row count, truncation, and p50/p95/p99 latency.

The runner measures cold materialization plus warm no-op, changed-TU update,
derived-transform rebuild, v33-to-v34 compatibility migration, online backup,
and recovery on isolated database copies. Each update/transform/migration/
backup path performs the operation twice and compares the stable semantic
digest. Corpus-specific self-index and banking runs remain manifest-governed
inputs; they are never represented by a synthetic result.

The Python sqlite3 API does not expose prepare/step or page-write counters.
Those fields are explicitly `unsupported`/`not_available`, while trace
statement and transaction counts are reported under their actual names.

## Decision gates

The local profile distinguishes storage, interactive query, and batch query
targets. The regression probe requires a deliberately bad layout/runtime result
to exceed the configured factor on the same query set and hardware profile.

A custom primary store may be proposed only when the decision record names the
exact failed SLO and supplies evidence that schema/tuning and a derived
accelerator cannot meet it. Engineering and compatibility costs are mandatory;
query latency alone is not a decision criterion.
