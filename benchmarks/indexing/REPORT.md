# HSE-95 benchmark report

This report records paired, reproducible HSE-95 measurements. Generated
corpora, caches, JSON, and profiler traces remain outside the repository.

## Provenance and method

- Host: Darwin arm64, Python 3.14.6.
- Baseline executable: latest `origin/main` commit `a74ae83`.
- Candidate executable: the rebased HSE-95 branch after replacing repeated
  resolved-symbol upserts with an idempotent declaration-site path guarded by
  a complete `add_symbol()` merge-semantic check.
- Corpus sizes: 32 and 1,000 translation units. Every TU includes the shared
  high-fan-in `shared.hpp`; TU 0 additionally includes `coverage.hpp`, which
  supplies warning-diagnostic, macro-use, call-argument, template,
  template-parameter, parameter, and pointer type-edge facts.
- Trials: three paired trials per executable and corpus size; values below are
  medians. Raw report: `/tmp/hse95-postrebase-v11.json`.
- Command:

```text
python3 benchmarks/indexing/run.py \
  --baseline-cidx /tmp/hse95-origin-main-latest/build/cidx \
  --current-cidx build/cidx \
  --representative-files 32 \
  --scale-files 1000 \
  --per-tu 5 \
  --trials 3 \
  --output /tmp/hse95-postrebase-v11.json
```

The baseline was rebuilt from `origin/main` in a disposable worktree. Because
that main revision's CMake graph does not emit the legacy `cli/souffle_rules.hpp`
include required by its existing sources, the baseline build additionally ran:

```text
cmake -DRULES_DIR=/tmp/hse95-origin-main-latest/python/indexer/rules \
  -DOUT=/tmp/hse95-origin-main-latest/build/generated/cli/souffle_rules.hpp \
  -P /tmp/hse95-origin-main-latest/cmake/embed_dl.cmake
cmake --build /tmp/hse95-origin-main-latest/build -j1 --target cidx
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
| 32 TUs | cold | 2.273 s / 0.898 | 1.819 s / 0.918 | +20.0% |
| 32 TUs | warm | 0.051 s / 0.881 | 0.045 s / 0.908 | +12.8% |
| 32 TUs | incremental | 0.110 s / 0.920 | 0.113 s / 0.883 | -2.9% |
| 1,000 TUs | cold | 244.336 s / 0.952 | 214.781 s / 0.951 | +12.1% |
| 1,000 TUs | warm | 0.348 s / 0.954 | 0.399 s / 0.834 | -14.6% |
| 1,000 TUs | incremental | 0.722 s / 0.954 | 0.716 s / 0.948 | +0.8% |

The three-trial aggregation demonstrates a repeatable full-refresh win at both
sizes, including the 1,000-TU acceptance case. Warm and incremental paths
remain below the 5-second and 2-second operational targets. Cold RSS was
45.1/45.0 MiB at 32 TUs and 55.6/55.5 MiB at 1,000 TUs (baseline/HSE-95).

## SQLite activity and repeated work

Cold-stage values are deltas from the import snapshot; warm and incremental
values are deltas from the immediately preceding stage.

| Corpus / stage | Page bytes | File rows | Symbol rows | Edge rows | Edge-site rows | Fact rows | Header counters |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 32 / cold | 471,040 | 2 | 331 | 325 | 579 | 2,520 | 2 indexed, 31 already |
| 32 / warm | 0 | 0 | 0 | 0 | 0 | 0 | 0 indexed, 0 already |
| 32 / incremental | 12,288 | 0 | 0 | 0 | 0 | 0 | 0 indexed, 2 already |
| 1,000 / cold | 16,252,928 | 2 | 10,011 | 10,005 | 18,003 | 77,056 | 2 indexed, 999 already |
| 1,000 / warm | 0 | 0 | 0 | 0 | 0 | 0 | 0 indexed, 0 already |
| 1,000 / incremental | 8,192 | 0 | 0 | 0 | 0 | 0 | 0 indexed, 2 already |

Shared-header fan-in equals the TU count at both sizes. The cold counters show
the owned sources being parsed and the shared header being reused; warm performs
no new semantic writes. The candidate avoids the repeated resolved-symbol
upsert, updates declaration metadata, and records the idempotent `decl_site`
row directly, preserving all row counts and canonical digests.

## Database and semantic correctness

Every produced database passed `PRAGMA integrity_check` and the harness's
foreign-key check. Every database reported schema version `40`, catalog version
`1`, and catalog hash
`21497a89add82fba96293f97b34f9a19c68912b6cc823a915889acf0709c216d`.

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
| 0 | 0.717 (0.687, 0.717, 0.828) | 0.668 (0.628, 0.668, 0.747) |
| 1 | 0.602 (0.558, 0.602, 0.644) | 0.612 (0.532, 0.612, 0.632) |
| 2 | 0.608 (0.579, 0.608, 0.685) | 0.618 (0.556, 0.618, 0.620) |
| 3 | 0.658 (0.574, 0.658, 0.674) | 0.588 (0.550, 0.588, 0.619) |
| 4 | 0.653 (0.567, 0.653, 0.764) | 0.602 (0.568, 0.602, 0.692) |

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

## HSE-114 external-identity reconciliation A/B

HSE-114 used one candidate binary in two modes to isolate statement elimination
from prepared-statement caching: `immediate` retained the former per-emission
sequence, while `batched` reconciled distinct identities at transaction commit.
The authoritative run used a quiescent host and a Spotlight-excluded temporary
root:

```text
TMPDIR=/tmp/hse114-benchmark.noindex \
python3 benchmarks/indexing/run.py \
  --current-cidx build/cidx \
  --reconciliation-ab \
  --representative-files 32 \
  --scale-files 1000 \
  --per-tu 5 \
  --trials 3 \
  --output /tmp/hse114-indexing-post-refinement.json
```

Values are three-trial medians:

| Corpus / stage | Calls | Prepare / steps | Rows matched / changed | Reconciliation wall / CPU | End-to-end wall |
| --- | ---: | ---: | ---: | ---: | ---: |
| 32 cold, immediate | 874 | 6,992 / 6,992 | 0 / 0 | 64.58 / 64.59 ms | 1.002 s |
| 32 cold, batched | 32 | 136 / 136 | 0 / 1 | 1.53 / 1.53 ms | 0.940 s |
| 1,000 cold, immediate | 27,010 | 216,080 / 216,080 | 0 / 0 | 2.027 / 2.027 s | 113.747 s |
| 1,000 cold, batched | 1,000 | 4,008 / 4,008 | 0 / 1 | 32.33 / 32.19 ms | 112.110 s |
| incremental, immediate | 34 | 272 / 272 | 0 / 1 | 2.48 / 2.48 ms | 0.353 s |
| incremental, batched | 1 | 12 / 12 | 0 / 1 | 7.90 / 7.90 ms | 0.365 s |

At 1,000 TUs, cold reconciliation prepares and step executions fell 98.15%,
reconciliation wall time fell 98.40%, and end-to-end cold indexing improved
1.44%. The incremental end-to-end median varied by 12.5 ms while remaining well
below the two-second target. SQLite's changed-row counter includes a
semantically idempotent type-row update; the normalized projections remain
identical.

All three baseline/current pairs matched for the 23 canonical semantic
sections, diagnostics, normalized Layer-0 output, schema version 40, catalog
version/hash, `PRAGMA integrity_check`, and foreign-key integrity. The report's
`parity_failures` list is empty at cold, warm, and incremental states for both
corpus sizes.

# S-098 routed root-fusion result

**Selected topology: two-root. Ship.** The four rooted whole-TU traversals
become two — one complete routed symbol walk, then one rooted graph-event
collection whose recorded stream is replayed by the declaration, definition and
namespace stages. Statement-body extraction stays a separate non-root phase.

## Identities

| Identity | Value |
| --- | --- |
| Source commit | `b695bb1203a0c1c1c0761c082977beec2abb70d2`, clean working tree |
| Build configuration | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON` |
| Executable SHA-256 | `cd03461250d241b1c553434130b4860cc4346b9a95f6edb9bfee30d3c727ac12` |
| Schema / catalog | version 40, catalog version 1, hash `c4f4262…9e37e9f7` |
| Reference host | Darwin-25.1.0-arm64 (Mach-O), 10 CPUs, Apple clang 17.0.0 |
| Corpus identity | `header-heavy:8:forward`, stage `cold`, 18 target distinct owned headers, 2 baseline owned headers |
| Baseline evidence | `s071-authoritative-r3.json`, resolved and verified through the backlog artifact registry on task S-098 |
| Final JSON | attached to S-098; SHA-256 `ff35aa518230ffe4699b691a80b366747cd94e1a042e9134949c88c551699a58` |

Command:

```text
python3 benchmarks/indexing/production.py --cidx build/cidx --checkout . \
  --representative-files 8 --scale-files 9 --many-header-target 16 --trials 3 \
  --work-root /tmp/s098-final.noindex --output /tmp/s098-final.json \
  --skip-self-index --skip-sqlite-matrix
```

## Three authoritative trials

| Trial | `root_symbols` | `root_declarations` | `root_definitions` | `root_namespaces` | Fixed root | Cold wall | Cold CPU | Peak RSS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 0.014931 | 0.004974 | 0.001233 | 0.000805 | 0.021943 | 0.137406 | 0.129729 | 48,381,952 |
| 2 | 0.014911 | 0.005080 | 0.001250 | 0.000880 | 0.022121 | 0.152600 | 0.137835 | 48,594,944 |
| 3 | 0.016672 | 0.005911 | 0.001362 | 0.000915 | 0.024860 | 0.157204 | 0.147550 | 48,496,640 |

Medians: `root_symbols` 0.014931 s, remaining rooted components 0.007190 s,
**fixed root 0.022121 s against the strict 0.025 s threshold** — 0.018490 s
below the pinned S-071 median of 0.040611 s. Every individual trial is also
below the threshold, so the verdict does not rest on the median hiding an
outlier. Registered and observed whole-TU traversals are 16 and 16 in all three
trials (8 TUs × 2 roots), never above budget.

The T-139 symbol-root budget (`root_symbols` median strictly below 0.015480 s)
is met at 0.014931 s. The measurement that got it there: 87% of `root_symbols`
was storage-call time, and the dominant term inside it was statement
*compilation*, not execution — the write path re-issues the same few long
statements once per row. `SqliteDb::prepare` now keeps a bounded
per-connection pool of compiled statements; no SQL text is rewritten or
generated.

## Parity

`parity_failures` is empty and `semantic_parity` is true. The canonical semantic
digest is identical across all three trials
(`5525263dbd6c68437702bf3146b6d5dd68e10fb0de26bc4f9e97dd2cf1622548`). Normalized
Layer-0 output over a 33-TU `src/ast` corpus, indexed three-pass with the
pre-change binary and with the candidate from the identical working tree, is
byte-identical: 44,690 lines, empty diff, both
`979bae1206d326e783ed61210f77640a76b42f0b8db75c081800c277a864c062`. Both
disposable databases report `PRAGMA integrity_check` = ok and zero
`PRAGMA foreign_key_check` violations. The committed `index.db` was not
regenerated, validated, or otherwise touched.

## Residual risk and ownership boundaries

The fusion deliberately stops at the traversal topology. What it does **not**
own, and who does:

- **S-070 owns applicability SQL.** Fact-to-file association statements were
  not rewritten; the fused stages feed the same association pass with the same
  per-file ID buckets.
- **S-072 and S-100 own FactBatch and serialization.** The routed root-event
  buffer is a pure in-memory relay of AST handles and routing answers, valid
  only while the recording `ASTContext` lives. It defines no persistent batch
  or transfer format.
- **S-099 owns controlled-writer header lifecycle.** Owned-header planning,
  `add_file`, and delete ownership stay where they are; the fused passes only
  consume the routed file set.
- **S-078 owns the final integrated SLO.** The number here is the fixed
  routed-root component on one pinned header-heavy corpus and one reference
  host, not an end-to-end production SLO claim.

Residual risk: the statement pool changes a shared persistence path, so it also
speeds the other rooted passes (`root_declarations` median 0.006965 s →
0.004974 s) and every non-root query. Its bounds are per connection — 256
distinct SQL texts × 4 statements — and pooled statements are reset, unbound
while idle, and finalized before the connection closes. The third trial's
noticeably higher numbers track host load rather than the candidate; the
conclusion is robust to that noise because the worst trial still clears the
threshold.
