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
  medians. The raw report was `/tmp/hse95-hash-final-trials.json`.
- Command:

```text
python3 benchmarks/indexing/run.py \
  --baseline-cidx /tmp/hse95-baseline/build/cidx \
  --current-cidx build/cidx \
  --representative-files 32 \
  --scale-files 1000 \
  --per-tu 5 \
  --trials 2 \
  --output /tmp/hse95-hash-final-trials.json
```

The harness performs a fresh import, cold index, resolve, unchanged warm index,
one-file incremental index, and five per-TU incremental samples. It records
wall time, child CPU time, CPU utilization, peak RSS, SQLite page/row deltas,
header counters, database integrity, schema/catalog metadata, and a canonical
digest of the seven Layer-0 semantic row projections. TU 0 is mutated again
after the separate incremental stage before it is timed as the first sample.

## Paired timing results

CPU utilization is child CPU seconds divided by stage wall seconds. RSS is the
cold-stage peak.

| Corpus | Build | Cold wall / CPU util | Warm wall / CPU util | Incremental wall / CPU util | Cold RSS |
| ---: | --- | ---: | ---: | ---: | ---: |
| 32 TUs | baseline | 4.312 s / 0.719 | 0.075 s / 0.855 | 0.152 s / 0.847 | 43.2 MiB |
| 32 TUs | HSE-95 | 4.082 s / 0.628 | 0.075 s / 0.806 | 0.147 s / 0.811 | 42.7 MiB |
| 1,000 TUs | baseline | 550.991 s / 0.738 | 0.372 s / 0.960 | 0.761 s / 0.942 | 50.7 MiB |
| 1,000 TUs | HSE-95 | 499.288 s / 0.717 | 0.544 s / 0.747 | 1.124 s / 0.759 | 51.0 MiB |

The paired cold result improves by 5.3% at 32 TUs and 9.4% at 1,000 TUs.
Warm and incremental timings remain within the operational targets of 5 and 2
seconds respectively. Their small absolute variation is retained in the raw
two-trial values rather than being substituted for the full-refresh result.

## SQLite activity and repeated work

Cold-stage values are median deltas from the import snapshot; warm and
incremental values are median deltas from the immediately preceding stage.

| Corpus / stage | Page bytes | File rows | Symbol rows | Edge rows | Edge-site rows | Header counters |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| 32 / cold | 450,560 | 1 | 322 | 321 | 576 | 1 indexed, 31 already |
| 32 / warm | 0 | 0 | 0 | 0 | 0 | 0 indexed, 0 already |
| 32 / incremental | 8,192 | 0 | 0 | 0 | 0 | 0 indexed, 1 already |
| 1,000 / cold | 16,003,072 | 1 | 10,002 | 10,001 | 18,000 | 1 indexed, 999 already |
| 1,000 / warm | 0 | 0 | 0 | 0 | 0 indexed, 0 already |
| 1,000 / incremental | 4,096 | 0 | 0 | 0 | 0 | 0 indexed, 1 already |

Both corpus sizes have shared-header fan-in equal to the TU count. The cold
indexed/already-indexed counters show one owned source being parsed and the
shared header being reused for the remaining TUs; the warm stage performs no
new semantic writes.

## Database and semantic correctness

Every produced database passed `PRAGMA integrity_check` and the harness's
foreign-key check. Every database reported schema version `39`, catalog version
`1`, and catalog hash
`1adb5f6663a2e48dc3a624c79703ceaa5287f2784731a00bbc469dba8d5935d4`.

The canonical Layer-0 digest comparison was equal for baseline and HSE-95 at
cold, warm, and incremental states:

| Corpus | Cold digest | Warm digest | Incremental digest |
| ---: | --- | --- | --- |
| 32 TUs | `df7973083610f05f9cf1e4be2ad313c842c539994b4d9de6cfd5c1c0a5c22143` | same | `750ad425d8dbe8a22af57e3031f2144aaca8e1cabee21b015d574993267ca8f1` |
| 1,000 TUs | `7a15271a20aa739a083d5b7e93670271589a6517dfb7c5c2b74fd5354c968b24` | same | `2c8e23588e75d18e27fd7b15124b60f9e91d9fbc8dda983e0a931fb231ec5103` |

The digest normalizes only the disposable corpus root; it includes canonical
file, symbol, declaration-site, edge, edge-site, call-argument, and
template-argument projections with surrogate IDs resolved to semantic keys.

## Per-TU latency

The five sampled 1,000-TU incremental updates had these median wall times; the
parenthesized values are the two trial measurements in seconds.

| TU | Baseline | HSE-95 |
| --- | ---: | ---: |
| 0 | 0.715 (0.656, 0.774) | 1.092 (0.794, 1.390) |
| 1 | 0.785 (0.576, 0.995) | 0.969 (0.861, 1.078) |
| 2 | 0.791 (0.509, 1.073) | 0.887 (0.840, 0.935) |
| 3 | 0.716 (0.526, 0.906) | 1.045 (0.833, 1.257) |
| 4 | 0.777 (0.688, 0.867) | 1.026 (0.992, 1.060) |

## Profiler-derived attribution

Apple Instruments Time Profiler (`xcrun xctrace record --template "Time
Profiler"`) was run on the same generated 8-TU corpus for each executable.
Raw traces and XML exports are `/tmp/hse95-baseline-8.trace`,
`/tmp/hse95-current-8b.trace`, `/tmp/hse95-baseline-8.profile.xml`, and
`/tmp/hse95-current-8b.profile.xml`. The counts below are inclusive sampled
stack appearances from the exported time-profile table:

```text
xcrun xctrace record --template "Time Profiler" --output /tmp/hse95.trace \\
  --launch -- build/cidx index
```

| Inclusive frame | Baseline samples | HSE-95 samples |
| --- | ---: | ---: |
| `sqlite3LockAndPrepare` | 200 | 184 |
| `SqliteStmt::SqliteStmt` | 197 | 182 |
| `StorageSymbolSink::emit` | 175 | 140 |
| `SqliteStorageService::add_symbol` | 132 | 113 |
| `TranslationUnitIndexer::run_edge_pass` | 139 | 130 |
| `SymbolVisitor::VisitNamedDecl` | 176 | 144 |

The profile attributes the dominant cold cost to Clang AST traversal and
SQLite statement preparation/writes, with the symbol sink as a material
secondary path. The original allocating identity-key lookup added work to
that sink for every symbol after the first resolved repeat. HSE-95 now uses a
non-allocating identity fingerprint as a guard and constructs the exact key
only on a fingerprint hit, retaining exact collision-safe verification. The
profile shows fewer sink and symbol-write samples, while the paired full
refresh measurements improve and the canonical semantic digests remain equal.
