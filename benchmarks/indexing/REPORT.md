# HSE-95 benchmark report

This report records paired, reproducible HSE-95 measurements. Generated
corpora, caches, JSON, and profiler traces remain outside the repository.

## Provenance and method

- Host: Darwin arm64, Python 3.14.6.
- Baseline executable: `origin/main` commit `002cf0c`.
- Candidate executable: the HSE-95 worktree after the non-allocating identity
  cache guard and benchmark parity/coverage harness changes.
- Corpus sizes: 32 and 1,000 translation units. Every TU includes the shared
  high-fan-in `shared.hpp`; TU 0 additionally includes `coverage.hpp`, which
  supplies a warning diagnostic, macro use, call-argument, template,
  template-parameter, parameter, and pointer type-edge facts.
- Trials: three paired trials per executable and corpus size; values below are
  medians. Raw report: `/tmp/hse95-expanded-final-v7.json`.
- Command:

```text
python3 benchmarks/indexing/run.py \
  --baseline-cidx /tmp/hse95-baseline/build/cidx \
  --current-cidx build/cidx \
  --representative-files 32 \
  --scale-files 1000 \
  --per-tu 5 \
  --trials 3 \
  --output /tmp/hse95-expanded-final-v7.json
```

The harness performs fresh import, cold index, resolve, unchanged warm index,
one-file incremental index, and five per-TU incremental samples. It records
wall time, child CPU time, CPU utilization, peak RSS, SQLite page/row deltas,
header counters, database integrity, schema/catalog metadata, and canonical
semantic projections. Every baseline/current trial is compared and both builds
must be repeat-consistent. TU 0 is mutated again after the separate
incremental stage before it is timed as the first sample.

## Paired timing results

CPU utilization is child CPU seconds divided by stage wall seconds. RSS is the
cold-stage peak. Percentages are current versus baseline wall time.

| Corpus | Stage | Baseline | HSE-95 | Delta |
| ---: | --- | ---: | ---: | ---: |
| 32 TUs | cold | 2.224 s / 0.915 | 2.676 s / 0.889 | -20.3% |
| 32 TUs | warm | 0.046 s / 0.897 | 0.069 s / 0.848 | -48.9% |
| 32 TUs | incremental | 0.099 s / 0.918 | 0.120 s / 0.918 | -21.1% |
| 1,000 TUs | cold | 228.306 s / 0.947 | 285.145 s / 0.936 | -24.9% |
| 1,000 TUs | warm | 0.331 s / 0.940 | 0.237 s / 0.954 | +28.3% |
| 1,000 TUs | incremental | 0.730 s / 0.935 | 0.550 s / 0.955 | +24.6% |

The three-trial aggregation does not demonstrate a cold/full-refresh win on
this final coverage corpus; that remains an explicit performance blocker. The
1,000-TU warm and incremental paths improve and remain below the 5-second and
2-second operational targets. Cold RSS was 44.9/44.9 MiB at 32 TUs and
53.3/53.3 MiB at 1,000 TUs (baseline/HSE-95).

## SQLite activity and repeated work

Cold-stage values are deltas from the import snapshot; warm and incremental
values are deltas from the immediately preceding stage.

| Corpus / stage | Page bytes | File rows | Symbol rows | Edge rows | Edge-site rows | Fact rows | Header counters |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 32 / cold | 462,848 | 1 | 331 | 325 | 579 | 2,520 | 2 indexed, 31 already |
| 32 / warm | 0 | 0 | 0 | 0 | 0 | 0 | 0 indexed, 0 already |
| 32 / incremental | 12,288 | 0 | 0 | 0 | 0 | 0 | 0 indexed, 2 already |
| 1,000 / cold | 16,064,512 | 1 | 10,011 | 10,005 | 18,003 | 77,056 | 2 indexed, 999 already |
| 1,000 / warm | 0 | 0 | 0 | 0 | 0 | 0 | 0 indexed, 0 already |
| 1,000 / incremental | 8,192 | 0 | 0 | 0 | 0 | 0 | 0 indexed, 2 already |

Shared-header fan-in equals the TU count at both sizes. The cold counters show
the owned sources being parsed and the shared header being reused; warm performs
no new semantic writes. The raw JSON includes all selected table deltas and
SQLite header counters for every trial.

## Database and semantic correctness

Every produced database passed `PRAGMA integrity_check` and the harness's
foreign-key check. Every database reported schema version `39`, catalog version
`1`, and catalog hash
`1adb5f6663a2e48dc3a624c79703ceaa5287f2784731a00bbc469dba8d5935d4`.

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
| 0 | 0.103 (0.159, 0.103, 0.091) | 0.139 (0.129, 0.210, 0.139) |
| 1 | 0.084 (0.117, 0.084, 0.075) | 0.116 (0.104, 0.224, 0.116) |
| 2 | 0.085 (0.126, 0.085, 0.075) | 0.134 (0.098, 0.247, 0.134) |
| 3 | 0.087 (0.136, 0.087, 0.077) | 0.146 (0.099, 0.191, 0.146) |
| 4 | 0.093 (0.137, 0.093, 0.072) | 0.124 (0.100, 0.235, 0.124) |

## Profiler-derived attribution

Apple Instruments Time Profiler was run by the checked-in `profile.py` mode on
fresh, named 8-TU corpora. These are the exact successful reproduction
commands:

```text
python3 benchmarks/indexing/profile.py --cidx /tmp/hse95-baseline/build/cidx --label baseline --files 8 --work-root /tmp/hse95-profile-baseline-v6 --trace /tmp/hse95-profile-baseline-v6.trace --xml /tmp/hse95-profile-baseline-v6.xml --summary /tmp/hse95-profile-baseline-v6.json
python3 benchmarks/indexing/profile.py --cidx build/cidx --label current --files 8 --work-root /tmp/hse95-profile-current-v5 --trace /tmp/hse95-profile-current-v5.trace --xml /tmp/hse95-profile-current-v5.xml --summary /tmp/hse95-profile-current-v5.json
```

Each summary records its exact corpus/import/cache paths, `xctrace` launch,
trace, XML export, and frame-count extraction. Inclusive frame counts from the
exported `time-profile` tables were:

| Inclusive frame | Baseline | HSE-95 |
| --- | ---: | ---: |
| `cidx::ast::` | 297 | 76 |
| `cidx::cli::detail::index_one` | 208 | 55 |
| `clang::ParseAST` | 161 | 41 |
| `sqlite3Prepare` | 77 | 26 |
| `SqliteStmt::SqliteStmt` | 75 | 24 |
| `StorageSymbolSink::emit` | 72 | 16 |
| `SymbolVisitor::VisitNamedDecl` | 73 | 17 |

The traces attribute the dominant work to Clang AST traversal and SQLite
statement preparation/writes, with symbol-sink emission as a material path.
That connects the chosen identity-cache strategy to the measured hotspot, but
the full-refresh timing above shows the current strategy still needs further
optimization before the performance objective is satisfied.

## Scope note

No repository `index.db` was read, modified, regenerated, staged, or included;
all benchmark databases were disposable files under `/tmp`.
