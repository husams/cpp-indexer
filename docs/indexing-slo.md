# Production indexing SLO

The published service level for `cidx index`, the evidence behind it, and the
costs that are still open with an owner and a threshold. This is the S-078
integration report for PERF-002: every earlier story in that feature qualified
one mechanism in isolation, and this is where all of them are measured
together, in the configuration that actually ships.

The contract is executable. Every limit, allowance and verdict rule below lives
in [`benchmarks/indexing/slo.py`](../benchmarks/indexing/slo.py) with its own
offline tests, and the numbers are re-derived from recorded reports by
[`benchmarks/indexing/publish_slo.py`](../benchmarks/indexing/publish_slo.py).
A future run can therefore re-judge this contract without anyone editing a
threshold in prose.

## Scope

| In | Out |
| --- | --- |
| The generated production corpora: 32 and 1,000 translation units, header-heavy, many-owned-header, and controlled fan-in shapes, forward and reverse compilation-database order. | **Self-indexing the `cpp-indexer` checkout.** Excluded by explicit instruction for this qualification; re-indexing is not on the critical path of a change in this repository. |
| Cold, unchanged-warm, one-source, low/high-fan-in-header, configuration-change and generated-input states. | Absolute timings on shared CI hardware. The scheduled job records `host_quiescence` so a contended run is visible rather than averaged in. |
| The shipped automatic worker policy, plus a pinned serial run for the telemetry only the serial path produces. | PCH/preamble reuse beyond the shipped `none` mechanism: ADR-014 ships no other, and the report records the identity rather than inventing a candidate. |

## Reference host and identities

| Identity | Value |
| --- | --- |
| Host | Darwin 25.1.0 arm64 (Mach-O), 10 CPUs |
| Compiler | Apple clang 17.0.0 (clang-1700.6.3.2) |
| Candidate executable | `cidx 0.53.0`, SHA-256 `7b71ba20730ca1f90bfff2efc9205200a98ecd73a88c6c33eeb05e2989135be2` |
| Candidate source | `17d2c66` on the S-078 branch |
| Pre-feature executable | `d9f4754` — the merge immediately before the first PERF-002 change (`5ae38ad`, HSE-114). SHA-256 `70e1a1dd…`. It advertises none of `--profile-json`, `--jobs`, `--clean`, `--defer-transforms`, so its arm runs without telemetry and that is recorded rather than assumed. |
| Schema / catalog | version 40, catalog version 1, hash `c4f4262…9e37e9f7` |
| SQLite | 3.53.3, shipped rollback-journal profile (`journal_mode = DELETE`, `synchronous = FULL`) |
| Trials | three per case per executable; every figure below is a median, with the trial series recorded in the JSON |

## The published SLO

Absolute limits, on the reference host, at 1,000 translation units. A limit is
exclusive: a median exactly at it fails.

| Measurement | Median | Limit | Headroom |
| --- | ---: | ---: | ---: |
| Unchanged warm index | **0.692 s** | 5 s | 4.31 s |
| One-source incremental index | **0.668 s** | 2 s | 1.33 s |
| High-fan-in-header state | **0.322 s** | 2 s | 1.68 s |
| Cold index (1,000 units) | **10.511 s** | 900 s | 889 s |

Cold supporting figures: 21.133 s child CPU (2.01x utilisation, so the workers
are genuinely overlapping), 109.1 MiB peak RSS, trials 7.961 / 10.511 / 11.126 s.

Relative to the pre-feature executable, on the same corpus and host:

| Stage | Pre-feature | Candidate | Change |
| --- | ---: | ---: | ---: |
| Cold | 105.129 s | 10.245 s | **10.26x faster** |
| Configuration change | 94.112 s | 14.008 s | 6.72x faster |
| Generated-input rebuild | 96.342 s | 14.373 s | 6.70x faster |
| Unchanged warm | 0.213 s | 0.715 s | 0.502 s slower — **waived**, see below |
| One-source incremental | 0.352 s | 0.713 s | 0.361 s slower — **waived** |
| High-fan-in header | 0.148 s | 0.322 s | 0.174 s slower — **waived** |

### Non-regression allowance and the three waivers

The contract allows a candidate median to exceed the recorded pre-feature
median by the greater of 10% or 50 ms. All three incremental stages exceed
that, so each carries an explicit waiver naming a quantified benefit, a cause
and an owner. The waivers are data in the report, not a code path that skips
the check: an incomplete waiver is reported as malformed and does not rescue
the measurement.

**Cause, measured rather than asserted.** A warm run extracts nothing, so
everything it spends is per-invocation overhead. Of the 0.715 s warm run,
**0.376 s is the derived-publication transform pipeline** — and no individual
transform executed: the whole cost is the readiness evaluation that runs
whether or not anything is stale. The same run issues 16.6k prepared
statements, 4.2M SQLite virtual-machine steps and 237k full-scan steps while
publishing nothing. The one-source run spends 0.315 s in the pipeline of which
only 0.071 s is individually timed transform execution, and
`entity-graph-rollup` scans 20,018 rows under its generation-gated full-rebuild
contract for a single changed source. The high-fan-in-header stage spends 0 s
in transforms and still issues 13,131 prepared statements at 1,000 registered
files — a per-invocation fixed cost that scales with the registered file count
rather than with the change.

**Benefit.** 10.26x cold. **Owners.** S-077 for the transform readiness
evaluation, S-069 for the run-wide session/workspace snapshot. Both are carried
in the residual list below with a threshold, so the term cannot creep.

The absolute contract is what protects the operator, and it holds with large
margins: warm is at 14% of its limit and one-source at 33% of its.

## The provisional `>=4x` cold goal: retained, and met

**Disposition: retained. Measured: 10.26x. The goal is met.**

The arithmetic that had to accompany any other disposition:

| Quantity | Value |
| --- | ---: |
| Measured Clang front-end share of cold **wall** time | 4.717 s of 10.511 s = **44.88%** |
| Measured Clang front-end share of cold **CPU** time | 4.717 s of 21.133 s = 22.32% |
| LibTooling-inclusive share of cold wall time | 8.271 s = 78.70% |
| Amdahl ceiling on *further* improvement from here | **2.23x** |

The ceiling is stated as a ceiling on what remains, not on what was achieved:
it is computed from the candidate's own front-end share, so a 10.26x
improvement from a 105 s baseline down to 10.5 s is entirely consistent with
only ~2.2x remaining if the front end became free. Reading it the other way
round is the mistake this row exists to prevent.

Which workstreams attack that term:

| Story | Mechanism | What it attacks |
| --- | --- | --- |
| S-074 (HSE-109) | bounded parallel translation-unit extraction | front-end **wall** time, by overlapping parses. Measured: serial 44.156 s against parallel 10.511 s on the same corpus, **4.20x**. |
| S-075 (HSE-110) | configuration-compatible PCH/preamble reuse | front-end **CPU** time. ADR-014 ships only the `none` mechanism; the identity is recorded and the explicit `--no-front-end-reuse` control publishes the same facts. |
| S-076 (HSE-111) | transitive-header invalidation and the content-addressed TU fact cache | front-end **calls**, by avoiding the parse on a hit. Measured: a cached replay avoids 12 of 12 parser calls where the fresh arm avoids none. Reachability caveat in [Dependency invalidation](#dependency-invalidation-partly-met) below. |

## Was the superlinear term reproduced?

Both readings are published, because they disagree and the disagreement is
informative.

**Across corpus sizes the term is bounded.** Per-translation-unit cold cost is
essentially flat between 32 and 1,000 units in the shipped configuration —
0.010487 s against 0.010510 s per unit, an implied growth exponent of
**1.0007**. Serially it is 0.035975 s against 0.044156 s, exponent 1.06. Both
are a large improvement on S-074's earlier measurement of ~N^1.25 serial and
~N^1.35 parallel, and both are far inside the 1.4 threshold this report
establishes for the term.

**Within a single 1,000-unit run, a positive positional term is still
statistically preferred in the parallel arm.** AIC prefers the quadratic model
over linear and constant, with coefficient 1.468e-08 +/- 1.614e-09 s per
position squared (9.1 sigma) and R^2 = 0.118, reproduced in reverse
compilation-database order too (1.328e-08 +/- 1.682e-09). At the end of a
1,000-unit corpus that term contributes about 14.7 ms to a unit whose mean cost
is 10.5 ms.

**The serial arm shows no such term**: its coefficient is *negative*,
-1.064e-09 +/- 5.132e-10, and the harness reports `superlinear-unresolved`.
A positional term that appears only under concurrency, on a corpus where the
aggregate per-unit cost is flat, is far better explained by worker contention
and reorder-buffer occupancy late in a run than by database growth. That is the
conservative bound this report publishes: **the corpus-growth term is removed
at the aggregate level; a concurrency-positional term remains, is measured, and
is bounded by the flat aggregate.**

## Mode equivalence

Every pair below is declared equivalent by the story that shipped it. They were
compared on one corpus, with one binary, in one run: normalized canonical
semantic projection, normalized Layer-0 dump, and every non-volatile table row
count, plus `integrity_check` and `foreign_key_check` on each database.

| Axis | Arms | Result |
| --- | --- | --- |
| Worker topology | `--jobs 1`, `--jobs 2`, `--jobs 4`, automatic | identical |
| Cache state (cold) | cache disabled, cache populating, cache enabled under `--jobs 4` | identical |
| Cache state (replay) | fresh replay against cached replay, identical history on both sides | identical |
| Front-end reuse | shipped default against explicit `--no-front-end-reuse` | identical |
| Transform mode | inline against `--defer-transforms` | identical |
| Publication mode | in-place update against `index rebuild --clean` | identical |
| Operating bounds | unbounded against three squeezed settings | identical |

**Discriminating evidence, so the axes are not vacuous.** The cached replay arm
avoided 12 parser calls; the fresh replay arm avoided none. The cold cache arm
avoided none, as a cache-populating run must. Both replay arms performed the
identical two forced re-extraction rounds, because a replayed database is
legitimately not identical to a cold one and comparing a cached replay against
a *cold* fresh arm would compare index history rather than the cache.

**Worker completion order** is qualified by `parallel_index_database_test`,
which indexes a real corpus with completion forced into the exact reverse of
the dispatch order and compares row-by-row projections, plus delete/re-emit,
repeated declarations and the same symbol declared by every unit.

## Dependency invalidation (partly met)

This is the one acceptance criterion this report cannot claim end to end, and
it is stated plainly rather than folded into a green table.

**What is qualified.** Editing the main source, an owned header, or an
unowned/generated intermediate changes the recorded content identity and
invalidates the cache entry; `plan_affected_translation_units` computes the
affected set and proves the remainder unaffected. Both are covered by
`tu_fact_cache_test` and `tu_fact_cache_integration_test`, run green as part of
this qualification.

**What is not reached.** `cidx index` never asks for that plan.
`plan_affected` has no caller outside the test suite, and target selection in
`run_index_pass` takes only files whose own `index_status` is not `kOk`. A
stale owned header is a header, so it is reported `deferred`; the translation
units that include it are still `kOk`, so they are reported `already`. Measured
directly on a 6-unit corpus: after appending a line to the high-fan-in header,
`cidx index` reports `0 indexed, 0 failed, 7 already indexed`, extracts **zero**
translation units, reports every `tu_dependency.*` counter at zero — the
planner was never consulted — and leaves the canonical digest unchanged. The
production harness's `high-fan-in-header` and `generated-input` stages show the
same zero-extraction result on the **pre-feature** executable, so this is not a
PERF-002 regression; it is a mechanism that shipped without being wired to the
scheduler.

**Owner: S-076.** Closing it means deciding what an index run does when the
dependency plan is unavailable — the planner lives in the serial cache wrapper,
which a multi-worker run bypasses entirely — and that decision changes what a
plain `cidx index` costs after a header edit. It is deliberately not made here.

**Operator guidance until it is closed:** after editing a header, run
`cidx index rebuild` (or `cidx index rebuild --clean` for an atomically
published rebuild). `cidx index` alone will not pick up dependents.

## Artifacts, failure atomicity and recovery

Qualified by named tests, all executed as part of this report against this
build rather than cited from memory:

| Test | Covers |
| --- | --- |
| `clean_rebuild_process_test` | every `CIDX_CLEAN_REBUILD_FAIL_AT` point, in-phase extraction and writer-commit faults, SIGINT inside the publication window and immediately after `rename(2)`, sidecar refusal, and the backup/restore round trip |
| `clean_rebuild_test` | capture, verification and publication contract |
| `tu_fact_cache_test`, `tu_fact_cache_integration_test` | the full decision taxonomy — hit, missing, stale, corrupt, incompatible, partial, truncated, untrusted, unavailable — and end-to-end replay against a real extraction |
| `storage_artifact_test` | retention, leases, pins and recovery bounds |
| `storage_migration_test` | migration from older databases, in place |
| `parallel_extraction_test` | bounded queue, reorder buffer and worker budgets |

**The interruption probe, and what its result actually means.** Every SQLite
profile in the matrix was SIGKILLed during a real `cidx index` and resumed. In
every case the database passed `integrity_check`, reported no foreign-key
violations, and the resumed run exited 0 — the interruption itself is clean.
The probe nonetheless reports `recovered: false`, because it compares the
resumed database against the pre-kill original and they differ. That difference
is **not caused by the interruption**: an *uninterrupted* forced re-index of the
same database produces the identical difference. Its content is two `contains`
edge counts moving from 1 to 2 plus a handful of applicability rows, because
the probe clears `indexed` on every row including already-indexed owned
headers, and the `contains` edge kind accumulates its count by design
(`count = edge.count + excluded.count`). Re-registering an already-indexed
owned header for extraction therefore double-counts its namespace containment.
No supported invalidation route reaches that state — a source edit and revert,
and a header edit and revert, both produce a database canonically identical to
a cold build — so it is reported here as a characterised property of the probe,
owned by the accumulating edge-count semantics, rather than as an interruption
defect.

## Operating bounds

Squeezed to the tightest settings the CLI accepts, on a 24-unit corpus, all
publishing facts identical to the unbounded arm:

| Setting | Bound exercised | Peak RSS |
| --- | --- | ---: |
| `--jobs 4` (control) | — | 69.7 MiB |
| `--jobs 4 --max-queue-items 1` | extracted-but-unpublished translation units | 68.7 MiB |
| `--jobs 4 --max-queue-bytes 65536` | extracted-but-unpublished payload bytes | 68.7 MiB |
| `--memory-budget-bytes 67108864` | resident-memory ceiling deriving the worker count | 59.3 MiB |

The memory budget visibly bites: it is the only setting that moves peak RSS,
and it moves it down. Cold peak RSS at 1,000 units is 109.1 MiB parallel and
80.1 MiB serial — the difference is the bounded in-flight extraction the queue
budgets govern. Every knob and its default is documented in
[indexing-parallelism.md](indexing-parallelism.md); artifact retention, leases
and pins in [tu-fact-cache.md](tu-fact-cache.md).

## SQLite profile qualification

The shipped rollback-journal profile is **retained**. Alternatives were
measured on a disposable 32-unit corpus with the shipped profile as control:

| Setting | Cold median | Outcome |
| --- | ---: | --- |
| shipped control (`DELETE` / `FULL`) | 0.349 s | retained |
| `cache_size = -65536` | 0.382 s | no improvement |
| `mmap_size = 256 MiB` | 0.363 s | no improvement |
| `temp_store = MEMORY` | 0.357 s | no improvement |
| `journal_mode = DELETE, synchronous = FULL` (explicit) | 0.363 s | equals the control, as it must |
| `journal_mode = WAL, synchronous = NORMAL` | — | **refused: incompatible with the shipped indexer** |

The WAL row is an evidence-backed do-not-ship, not a missing measurement. Under
bounded parallel extraction every worker opens a read-only handle to a snapshot
of the database; a WAL database is not wholly contained in its main file, and
the run fails with `unable to open database file` on every translation unit.
This is the same containment property the clean rebuild refuses a WAL serving
database for. No alternative setting is recommended for production, and none
was faster than the control.

## Residual costs, with owners and thresholds

This report **establishes** this list. A regression threshold cannot honestly be
chosen before the quantity is measured, so each threshold below is set from the
measurement with stated headroom; from here on, crossing one reopens the term.

| Term | Shape | Owner | Threshold | Measured | Status |
| --- | --- | --- | ---: | ---: | --- |
| Per-translation-unit rooted AST traversals | fixed multiplier per unit | T-139 (symbol root) / S-078 (aggregate) | fixed routed-root median < 0.025 s | **0.023113 s** | within |
| Superlinear per-unit cost against corpus size | corpus-growth-sensitive | S-068 (HSE-103) | implied growth exponent <= 1.4 | **1.0007** | within |
| Serial controlled-writer publication | fixed multiplier, not parallelised | S-073 / S-074 | publication share of cold wall <= 0.85 | **0.755** | within |
| Per-invocation derived-transform readiness evaluation | corpus-growth-sensitive, paid every invocation | S-077 | unchanged-warm median <= 1.0 s | **0.692 s** | within |

**Rooted traversals after S-098.** The two-root fusion shipped in PR #86. Two
rooted whole-translation-unit walks remain per unit, and they are budgeted,
observed and published rather than left implicit: on the pinned
`header-heavy:8:forward` corpus, **16 registered and 16 observed** traversals in
every trial, fixed routed-root median 0.023113 s against the strict 0.025 s
threshold, down from the pre-fusion 0.040611 s baseline. Their measured share of
cold wall time on that corpus is **7.69%**; on the larger `header-heavy:32`
corpus the same component is 0.050674 s, **2.76%** of cold wall. Re-deriving
S-098's ship decision from this build's report returns `ship_eligible: true`
with no refusal reason. No do-not-ship decision remains open.

**The publication term is now the dominant one.** At 1,000 units the controlled
writer accounts for 7.936 s of a 10.511 s cold wall. That is what bounds any
further parallel gain, and it is why the front-end share (44.88%) and the
publication share (75.5%) sum past 100%: the front end runs on workers, the
writer on the scheduler thread, and they overlap. The threshold is set at 0.85,
about 12% headroom; the placeholder carried while the harness was being written
was 0.75, and the measurement is what set the published value. That revision is
recorded rather than quietly applied.

## Regression guard

[`.github/workflows/indexing-performance.yml`](../.github/workflows/indexing-performance.yml).

Timing on a shared runner is not a gate — it is noisy, and a red build meaning
"the runner was busy" trains people to ignore red builds. What is gated on every
change is the deterministic part, which is also the part a performance change
actually breaks:

| Job | Trigger | Cost | Gates |
| --- | --- | --- | --- |
| `contract` | every change to the indexing paths | seconds | the SLO contract and the matrix decision rules, offline |
| `equivalence` | every change to the indexing paths | minutes | cross-mode fact equivalence and the operating bounds, on a 6-unit and an 8-unit corpus |
| `production-scale` | weekly, or on demand | hours | the full matrix at 1,000 units in both topologies, uploading the reports the SLO is re-derived from |

## Defects this integration found and fixed

Three, all invisible until every mechanism was in one binary, none of them a
PERF-002 regression on its own:

1. **Diagnostic applicability depended on index history.** Publication was
   gated on a single-route publication as well as on the route matching the
   diagnostic's own file, so a cold run that also minted the translation unit's
   owned headers dropped the row while a later re-index of the same unchanged
   source added it. One corpus produced two different databases depending on
   how it was reached. Reproduces identically on the pre-feature executable.
2. **The parallel scheduler published no writer telemetry.** It owns its own
   controlled writer, so `record_writer_profile` was never called: the shipped
   configuration reported no `fact_batch_writer.*` counters or timings at all,
   and the production measurement gate could not run against it.
3. **Every parallel translation unit reported corpus position 0.** The position
   was read as "how many units have been recorded so far", which is the
   dispatch position only when units run one at a time. The per-unit
   cost-versus-position analysis this report is fitted against had no abscissa.

## Reproducing this report

Run on a quiescent host. All four commands write outside the checkout; the
committed `index.db` is never opened.

```sh
# The shipped automatic topology: an SLO describes the mode that ships.
python3 benchmarks/indexing/production.py --cidx build/cidx --checkout . \
  --skip-self-index --representative-files 32 --scale-files 1000 --trials 3 \
  --work-root /tmp/s078-A.noindex --output /tmp/s078-A.json

# The pinned S-098 header-heavy corpus, for the routed-root ship decision.
python3 benchmarks/indexing/production.py --cidx build/cidx --checkout . \
  --skip-self-index --skip-sqlite-matrix --representative-files 8 \
  --scale-files 9 --many-header-target 16 --trials 3 \
  --work-root /tmp/s078-C.noindex --output /tmp/s078-C.json

# The serial topology, which additionally publishes the translation-unit cache
# and dependency taxonomies.
python3 benchmarks/indexing/production.py --cidx build/cidx --checkout . \
  --skip-self-index --skip-sqlite-matrix --index-jobs 1 \
  --representative-files 32 --scale-files 1000 --trials 3 \
  --work-root /tmp/s078-B.noindex --output /tmp/s078-B.json

# The cross-mode matrix, the operating bounds, the cited qualification tests,
# and the pre-feature A/B. Build the pre-feature executable from d9f4754.
python3 benchmarks/indexing/integrated.py --cidx build/cidx \
  --pre-feature-cidx /path/to/d9f4754/build/cidx --build-dir build \
  --equivalence-files 12 --equivalence-shape header-heavy --bounds-files 24 \
  --pre-feature-files 1000 --pre-feature-shape baseline --trials 3 \
  --work-root /tmp/s078-D.noindex --output /tmp/s078-D.json

# Judge all four against the contract. Exits non-zero if the decision is not ok.
python3 benchmarks/indexing/publish_slo.py \
  --shipped /tmp/s078-A.json --serial /tmp/s078-B.json \
  --fusion /tmp/s078-C.json --integrated /tmp/s078-D.json \
  --representative-files 32 --scale-files 1000 --fusion-representative-files 8 \
  --cold-goal-disposition retained \
  --waivers benchmarks/indexing/fixtures/s078-regression-waivers.json \
  --output /tmp/s078-slo.json
```

Verify the contract itself, offline, with no binary and no corpus:

```sh
python3 -m unittest benchmarks.indexing.slo_test \
  benchmarks.indexing.integrated_test benchmarks.indexing.publish_slo_test \
  benchmarks.indexing.production_test
```
