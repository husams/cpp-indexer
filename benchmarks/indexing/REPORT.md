# HSE-95 benchmark report

This report records paired, reproducible HSE-95 measurements. Generated
corpora, caches, JSON, and profiler traces remain outside the repository.

## Provenance and method

- Host: Darwin arm64, Python 3.14.6.
- Baseline executable: latest `origin/main` commit `def1bdf`.
- Candidate executable: the rebased HSE-95 branch after replacing repeated
  resolved-symbol upserts with an idempotent declaration-site path guarded by
  a complete `add_symbol()` merge-semantic check.
- Corpus sizes: 32 and 1,000 translation units. Every TU includes the shared
  high-fan-in `shared.hpp`; TU 0 additionally includes `coverage.hpp`, which
  supplies warning-diagnostic, macro-use, call-argument, template,
  template-parameter, parameter, and pointer type-edge facts.
- Trials: three paired trials per executable and corpus size; values below are
  medians. Raw report: `/tmp/hse95-postfix-v10.json`.
- Command:

```text
python3 benchmarks/indexing/run.py \
  --baseline-cidx /tmp/hse95-baseline-rebased/build/cidx \
  --current-cidx build/cidx \
  --representative-files 32 \
  --scale-files 1000 \
  --per-tu 5 \
  --trials 3 \
  --output /tmp/hse95-postfix-v10.json
```

The baseline was rebuilt from `origin/main` in a disposable worktree. Because
that main revision's CMake graph does not emit the legacy `cli/souffle_rules.hpp`
include required by its existing sources, the baseline build additionally ran:

```text
cmake -DRULES_DIR=/tmp/hse95-origin-main-rebased/python/indexer/rules \
  -DOUT=/tmp/hse95-baseline-rebased/build/generated/cli/souffle_rules.hpp \
  -P /tmp/hse95-origin-main-rebased/cmake/embed_dl.cmake
cmake --build /tmp/hse95-baseline-rebased/build -j1 --target cidx
```

The harness performs fresh import, cold index, resolve, unchanged warm index,
one-file incremental index, and five per-TU incremental samples. It records
wall time, child CPU time, CPU utilization, peak RSS, SQLite page/row deltas,
header counters, database integrity, schema/catalog metadata, and canonical
semantic projections. Every baseline/current trial is compared and both builds
must be repeat-consistent. TU 0 is mutated again after the separate
incremental stage before it is timed as the first sample.

## Paired timing results

CPU utilization is child CPU seconds divided by stage wall seconds. Percentages
are current versus baseline wall time; positive means faster.

| Corpus | Stage | Baseline | HSE-95 | Improvement |
| ---: | --- | ---: | ---: | ---: |
| 32 TUs | cold | 1.866 s / 0.922 | 1.711 s / 0.927 | +8.3% |
| 32 TUs | warm | 0.045 s / 0.890 | 0.046 s / 0.884 | -2.0% |
| 32 TUs | incremental | 0.092 s / 0.925 | 0.088 s / 0.922 | +4.7% |
| 1,000 TUs | cold | 221.326 s / 0.957 | 203.757 s / 0.953 | +7.9% |
| 1,000 TUs | warm | 0.347 s / 0.886 | 0.345 s / 0.946 | +0.5% |
| 1,000 TUs | incremental | 0.647 s / 0.954 | 0.671 s / 0.959 | -3.8% |

The three-trial aggregation demonstrates a repeatable full-refresh win at both
sizes, including the 1,000-TU acceptance case. Warm and incremental paths
remain below the 5-second and 2-second operational targets. Cold RSS was
44.9/44.8 MiB at 32 TUs and 55.5/55.6 MiB at 1,000 TUs (baseline/HSE-95).

## SQLite activity and repeated work

Cold-stage values are deltas from the import snapshot; warm and incremental
values are deltas from the immediately preceding stage.

| Corpus / stage | Page bytes | File rows | Symbol rows | Edge rows | Edge-site rows | Fact rows | Header counters |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 32 / cold | 462,848 | 2 | 331 | 325 | 579 | 2,520 | 2 indexed, 31 already |
| 32 / warm | 0 | 0 | 0 | 0 | 0 | 0 | 0 indexed, 0 already |
| 32 / incremental | 12,288 | 0 | 0 | 0 | 0 | 0 | 0 indexed, 2 already |
| 1,000 / cold | 16,064,512 | 2 | 10,011 | 10,005 | 18,003 | 77,056 | 2 indexed, 999 already |
| 1,000 / warm | 0 | 0 | 0 | 0 | 0 | 0 | 0 indexed, 0 already |
| 1,000 / incremental | 8,192 | 0 | 0 | 0 | 0 | 0 | 0 indexed, 2 already |

Shared-header fan-in equals the TU count at both sizes. The cold counters show
the owned sources being parsed and the shared header being reused; warm performs
no new semantic writes. The candidate avoids the repeated resolved-symbol
upsert, updates declaration metadata, and records the idempotent `decl_site`
row directly, preserving all row counts and canonical digests.

## Database and semantic correctness

Every produced database passed `PRAGMA integrity_check` and the harness's
foreign-key check. Every database reported schema version `39`, catalog version
`1`, and catalog hash
`3337824260ee0afe1260859b6be88e6fb8280852fd736cde5e12cca5c3847ba4`.

The canonical projection has exactly these 23 required sections:
`semantic_universe`, `translation_unit_config`, `file`, `file_config`,
`symbol`, `decl_site`, `edge`, `edge_site`, `call_arg`, `template_arg`,
`template_param`, `definition`, `def_edge`, `type_node`, `type_edge`,
`parameter`, `symbol_type`, `include_config`, `include_edge`, `include_site`,
`include_macro_use`, `diagnostic`, and `fact_applicability`. All sections are
nonempty except the intentionally metadata-only `translation_unit_config`.

| Section | 32 TUs | 1,000 TUs |
| --- | ---: | ---: |
| semantic_universe | 2 | 2 |
| translation_unit_config | 1 | 1 |
| file | 34 | 1,002 |
| file_config | 66 | 2,002 |
| symbol | 331 | 10,011 |
| decl_site | 874 | 27,010 |
| edge | 325 | 10,005 |
| edge_site | 579 | 18,003 |
| call_arg | 1 | 1 |
| template_arg | 1 | 1 |
| template_param | 2 | 2 |
| definition | 327 | 10,007 |
| def_edge | 291 | 9,003 |
| type_node | 4 | 4 |
| type_edge | 2 | 2 |
| parameter | 5 | 5 |
| symbol_type | 330 | 10,010 |
| include_config | 32 | 1,000 |
| include_edge | 33 | 1,001 |
| include_site | 33 | 1,001 |
| include_macro_use | 1 | 1 |
| diagnostic | 1 | 1 |
| fact_applicability | 2,520 | 77,056 |

The harness compares all three trials independently at cold, warm, and
incremental states and fails on any trial or intra-build repeat mismatch. All
`parity_failures` are empty; every trial reports canonical semantic match,
database integrity match, and schema/catalog match.

Canonical symbol-bearing projections use one scoped key:
`(semantic_universe.key, identity_key)` with the identity key falling back to
USR. The same key is used in every symbol-bearing projection and every
`fact_applicability` key; surrogate IDs are resolved before hashing. The
duplicate-USR fixture uses `c:@F@duplicate_fixture#` in two universes and
changes the relationship assignment. Its digests are
`6ede23fce1c0e9e25d51407b3abcca5171f28579361e9f48b96a2732221002a8` before
and `56f5a6422475e3fcdb2ed1b0802967ba80a98b34d1d162b8956bc70c36a382ad` after,
proving reassignment changes the canonical digest.

## Per-TU latency

The five sampled 1,000-TU incremental updates show median wall time; the
parenthesized values are all three trial measurements in seconds.

| TU | Baseline | HSE-95 |
| --- | ---: | ---: |
| 0 | 0.665 (0.643, 0.665, 0.690) | 0.670 (0.662, 0.670, 0.685) |
| 1 | 0.575 (0.558, 0.575, 0.579) | 0.527 (0.526, 0.527, 0.549) |
| 2 | 0.562 (0.552, 0.562, 0.575) | 0.525 (0.525, 0.525, 0.543) |
| 3 | 0.569 (0.556, 0.569, 0.570) | 0.528 (0.512, 0.528, 0.548) |
| 4 | 0.576 (0.546, 0.576, 0.636) | 0.524 (0.514, 0.524, 0.577) |

## Profiler-derived attribution

The checked-in `benchmarks/indexing/profile.py` mode was used for successful
baseline and candidate captures on fresh named 8-TU corpora before the final
cache-hit replacement. The exact successful commands were:

```text
python3 benchmarks/indexing/profile.py --cidx /tmp/hse95-baseline/build/cidx --label baseline --files 8 --work-root /tmp/hse95-profile-baseline-v6 --trace /tmp/hse95-profile-baseline-v6.trace --xml /tmp/hse95-profile-baseline-v6.xml --summary /tmp/hse95-profile-baseline-v6.json
python3 benchmarks/indexing/profile.py --cidx build/cidx --label current --files 8 --work-root /tmp/hse95-profile-current-v5 --trace /tmp/hse95-profile-current-v5.trace --xml /tmp/hse95-profile-current-v5.xml --summary /tmp/hse95-profile-current-v5.json
```

Those exported `time-profile` tables attributed material work to Clang AST
traversal, `sqlite3Prepare`, `SqliteStmt::SqliteStmt`, and
`StorageSymbolSink::emit`. That evidence led to removing the repeated resolved
symbol upsert while retaining the declaration-site write. Fresh post-fix
captures were attempted with the same checked-in mode and fresh v8 paths, but
both failed at `xctrace record` with macOS `Failed starting ktrace session`;
the exact import/record commands and disposable traces remain available under
`/tmp/hse95-profile-{baseline,current}-v8*`.

## Scope note

No repository `index.db` was read, modified, regenerated, staged, or included;
all benchmark databases were disposable files under `/tmp`.
