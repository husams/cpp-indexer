# HSE-95 benchmark report

This report records paired, reproducible HSE-95 measurements. The generated
corpora, caches, JSON, and profiler traces remain outside the repository.

## Provenance and method

- Host: Darwin arm64, Python 3.14.6.
- Baseline executable: `origin/main` commit `002cf0c`.
- Candidate executable: the HSE-95 worktree after the non-allocating identity
  cache guard was applied.
- Corpus sizes: 32 and 1,000 translation units; every TU includes one shared
  header, repeats 16 resolved declarations, and visits eight call edges twice.
- Trials: two paired trials per executable and corpus size; values below are
  medians. The raw report is `/tmp/hse95-expanded-final-trials.json`.
- Command:

```text
python3 benchmarks/indexing/run.py \
  --baseline-cidx /tmp/hse95-baseline/build/cidx \
  --current-cidx build/cidx \
  --representative-files 32 \
  --scale-files 1000 \
  --per-tu 5 \
  --trials 2 \
  --output /tmp/hse95-expanded-final-trials.json
```

The harness performs a fresh import, cold index, resolve, unchanged warm index,
one-file incremental index, and five per-TU incremental samples. It records
wall time, child CPU time, CPU utilization, peak RSS, SQLite page/row deltas,
header counters, database integrity, schema/catalog metadata, and canonical
digests of normalized semantic projections. Those projections include the
semantic-universe and symbol identity-key universe, file/config descriptors and
file-config associations, declaration/edge/definition/type/template/parameter
families, and configuration-qualified fact applicability with generations; all
database-local IDs are resolved to semantic keys. Every baseline/current trial
is compared, and both builds must be repeat-consistent. TU 0 is mutated again
after the separate incremental stage before it is timed as the first sample.

## Paired timing results

CPU utilization is child CPU seconds divided by stage wall seconds. RSS is the
cold-stage peak.

| Corpus | Build | Cold wall / CPU util | Warm wall / CPU util | Incremental wall / CPU util | Cold RSS |
| ---: | --- | ---: | ---: | ---: | ---: |
| 32 TUs | baseline | 3.486 s / 0.725 | 0.065 s / 0.756 | 0.182 s / 0.679 | 42.8 MiB |
| 32 TUs | HSE-95 | 2.104 s / 0.720 | 0.049 s / 0.704 | 0.123 s / 0.613 | 42.6 MiB |
| 1,000 TUs | baseline | 316.533 s / 0.836 | 0.344 s / 0.630 | 0.367 s / 0.948 | 51.3 MiB |
| 1,000 TUs | HSE-95 | 269.793 s / 0.903 | 0.330 s / 0.916 | 0.661 s / 0.907 | 51.4 MiB |

The paired cold result improves by 39.6% at 32 TUs and 14.8% at 1,000 TUs.
Warm and incremental timings remain within the operational targets of 5 and 2
seconds respectively. Their small absolute variation is retained in the raw
two-trial values rather than being substituted for the full-refresh result.

## SQLite activity and repeated work

Cold-stage values are median deltas from the import snapshot; warm and
incremental values are median deltas from the immediately preceding stage.

| Corpus / stage | Page bytes | File rows | Symbol rows | Edge rows | Edge-site rows | Fact rows | Header counters |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 32 / cold | 450,560 | 1 | 322 | 321 | 576 | 2,472 | 1 indexed, 31 already |
| 32 / warm | 0 | 0 | 0 | 0 | 0 | 0 | 0 indexed, 0 already |
| 32 / incremental | 8,192 | 0 | 0 | 0 | 0 | 0 | 0 indexed, 1 already |
| 1,000 / cold | 16,003,072 | 1 | 10,002 | 10,001 | 18,000 | 77,008 | 1 indexed, 999 already |
| 1,000 / warm | 0 | 0 | 0 | 0 | 0 | 0 | 0 indexed, 0 already |
| 1,000 / incremental | 4,096 | 0 | 0 | 0 | 0 | 0 | 0 indexed, 1 already |

Both corpus sizes have shared-header fan-in equal to the TU count. The cold
indexed/already-indexed counters show one owned source being parsed and the
shared header being reused for the remaining TUs; the warm stage performs no
new semantic writes.

## Database and semantic correctness

Every produced database passed `PRAGMA integrity_check` and the harness's
foreign-key check. Every database reported schema version `39`, catalog version
`1`, and catalog hash
`1adb5f6663a2e48dc3a624c79703ceaa5287f2784731a00bbc469dba8d5935d4`.

The final harness compares both trials independently at cold, warm, and
incremental states and requires intra-build repeat consistency. The expanded
canonical digest comparison is equal for every baseline/current trial at both
corpus sizes; the raw JSON also records the per-section row counts and the
`parity_failures` list (empty):

| Corpus | Cold digest | Warm digest | Incremental digest |
| ---: | --- | --- | --- |
| 32 TUs | `82275d283121f17b5d09534d54b3e10b06f70893899ae812150d09b743e99af8` | same | `075e87e9fb215544ce289b53dc824a4a6867f3cce15cc361020d1f6562ae8487` |
| 1,000 TUs | `e28eb7af420e692d76df1985f96e2fd75eaab17bd2f728513b3e43c7f85fe03d` | same | `16d02e0d4aeaaa6c3b66f6bd1b80fbe21b604b9e14c86a745d5c4c94eafc31e8` |

The digest normalizes the disposable corpus root and generated `build:<hash>`
universe labels. It includes canonical file, symbol, declaration-site, edge,
edge-site, call-argument, definition, def-edge, type, template, parameter,
include, diagnostic, semantic-universe, file-config, translation-unit-config,
symbol-type, and fact-applicability projections with surrogate IDs resolved to
semantic keys.

## Per-TU latency

The five sampled 1,000-TU incremental updates had these median wall times; the
parenthesized values are the two trial measurements in seconds.

| TU | Baseline | HSE-95 |
| --- | ---: | ---: |
| 0 | 0.347 (0.414, 0.279) | 0.683 (1.099, 0.268) |
| 1 | 0.384 (0.481, 0.286) | 2.096 (3.918, 0.274) |
| 2 | 0.388 (0.446, 0.331) | 1.070 (1.873, 0.267) |
| 3 | 0.321 (0.352, 0.291) | 0.838 (1.416, 0.259) |
| 4 | 0.461 (0.644, 0.278) | 0.805 (1.349, 0.261) |

## Profiler-derived attribution

Apple Instruments Time Profiler was run by the checked-in
`benchmarks/indexing/profile.py` mode on fresh, named 8-TU corpora. The exact
baseline and candidate commands were:

```text
python3 benchmarks/indexing/profile.py --cidx /tmp/hse95-baseline/build/cidx \
  --label baseline --files 8 --work-root /tmp/hse95-profile-baseline-v2 \
  --trace /tmp/hse95-profile-baseline-v2.trace \
  --xml /tmp/hse95-profile-baseline-v2.xml \
  --summary /tmp/hse95-profile-baseline-v2.json
python3 benchmarks/indexing/profile.py --cidx build/cidx --label current \
  --files 8 --work-root /tmp/hse95-profile-current-v2 \
  --trace /tmp/hse95-profile-current-v2.trace \
  --xml /tmp/hse95-profile-current-v2.xml \
  --summary /tmp/hse95-profile-current-v2.json
```

The mode records the exact import, profile-launch, and XML-export commands in
each summary JSON. The counts below are inclusive frame appearances extracted
by the same mode from those exported `time-profile` tables:

| Inclusive frame | Baseline samples | HSE-95 samples |
| --- | ---: | ---: |
| `sqlite3LockAndPrepare` | 164 | 122 |
| `SqliteStmt::SqliteStmt` | 160 | 120 |
| `StorageSymbolSink::emit` | 148 | 96 |
| `SymbolVisitor::VisitNamedDecl` | 151 | 98 |

The profile attributes the dominant cold cost to Clang AST traversal and
SQLite statement preparation/writes, with `StorageSymbolSink::emit` a material
secondary path. That is the path changed by HSE-95: a non-allocating identity
fingerprint guards the exact key construction, and exact collision-safe
verification remains on fingerprint hits. The paired measurements and expanded
canonical semantic checks provide the performance and correctness evidence for
that strategy.
