# HSE-95 benchmark report

This report records paired, reproducible HSE-95 measurements. Generated
corpora, caches, JSON, and profiler traces remain outside the repository.

## Provenance and method

- Host: Darwin arm64, Python 3.14.6.
- Baseline executable: latest `origin/main` commit `def1bdf`.
- Candidate executable: the rebased HSE-95 branch after replacing repeated
  resolved-symbol upserts with an idempotent declaration-site path.
- Corpus sizes: 32 and 1,000 translation units. Every TU includes the shared
  high-fan-in `shared.hpp`; TU 0 additionally includes `coverage.hpp`, which
  supplies warning-diagnostic, macro-use, call-argument, template,
  template-parameter, parameter, and pointer type-edge facts.
- Trials: three paired trials per executable and corpus size; values below are
  medians. Raw report: `/tmp/hse95-rebased-final-v8.json`.
- Command:

```text
python3 benchmarks/indexing/run.py \
  --baseline-cidx /tmp/hse95-baseline-rebased/build/cidx \
  --current-cidx build/cidx \
  --representative-files 32 \
  --scale-files 1000 \
  --per-tu 5 \
  --trials 3 \
  --output /tmp/hse95-rebased-final-v8.json
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
| 32 TUs | cold | 1.283 s / 0.940 | 1.236 s / 0.936 | +3.7% |
| 32 TUs | warm | 0.033 s / 0.905 | 0.039 s / 0.911 | -17.0% |
| 32 TUs | incremental | 0.069 s / 0.943 | 0.074 s / 0.946 | -7.4% |
| 1,000 TUs | cold | 200.688 s / 0.979 | 148.888 s / 0.979 | +25.8% |
| 1,000 TUs | warm | 0.317 s / 0.985 | 0.235 s / 0.985 | +25.8% |
| 1,000 TUs | incremental | 0.777 s / 0.988 | 0.466 s / 0.988 | +40.1% |

The three-trial aggregation demonstrates a repeatable full-refresh win at both
sizes, including the 1,000-TU acceptance case. The 1,000-TU warm and
incremental paths also improve and remain below the 5-second and 2-second
operational targets. Cold RSS was 44.2/44.4 MiB at 32 TUs and 55.3/55.2 MiB at
1,000 TUs (baseline/HSE-95).

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
| 0 | 0.681 (0.484, 0.681, 1.044) | 0.521 (0.464, 0.521, 0.667) |
| 1 | 0.588 (0.423, 0.588, 0.808) | 0.455 (0.398, 0.455, 0.559) |
| 2 | 0.580 (0.419, 0.580, 0.817) | 0.427 (0.400, 0.427, 0.577) |
| 3 | 0.574 (0.420, 0.574, 0.923) | 0.407 (0.395, 0.407, 0.606) |
| 4 | 0.636 (0.416, 0.636, 0.964) | 0.398 (0.398, 0.395, 0.540) |

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
