# Semantic indexing scale benchmark

`run.py` is the reproducible benchmark harness for HSE-95. It generates a
disposable C++ corpus with one shared header and a configurable number of
translation units, imports it into a temporary cache, and measures:

- import, cold index, resolve, unchanged warm index, and one-file incremental
  index stages;
- wall time, child CPU time, and peak resident memory for every stage;
- per-translation-unit timings for a bounded sample;
- SQLite page/row deltas from the disposable benchmark database;
- shared-header fan-in and the indexer's `indexed`/`already` header counters.
- SQLite `integrity_check`, schema/catalog metadata, and a canonical semantic
  digest of normalized semantic/fact projections at each index state;
- the normalized Layer-0 projection produced by `scripts/dump_layer0.sh`;
- repeated trials with median timing, CPU utilization, and per-TU latency
  aggregation, including per-trial and intra-build parity checks.
- optional immediate-versus-batched external-identity reconciliation metrics:
  calls, prepared statements, VDBE steps, matched/changed rows, wall time, and
  CPU time.

Generated sources, caches, logs, and JSON reports belong outside the checkout.
The runner uses a temporary `INDEXER_CACHE`; it never opens the checkout's
database. Keep reports under `/tmp` (or another disposable directory).

## Production measurement gate (HSE-103)

`production.py` is the supported end-to-end command for the PERF-002
measurement gate. It retains the HSE-95 32/1,000-TU baseline (`D = 2` distinct
new owned headers), adds header-heavy, `D = 128` many-owned-header, and
controlled low/high-fan-in corpora, and benchmarks the current checkout using
its recorded compilation database. It runs cold, unchanged warm, one-source,
low-fan-in-header, high-fan-in-header, configuration-change, and
generated-input states. Forward and reversed compilation-database orders
separate translation-unit complexity from database-position effects.

Run it only on a quiescent host. By default it refuses to collect authoritative
timings while another `cidx index` process is visible:

```sh
python3 benchmarks/indexing/production.py \
  --cidx build/cidx \
  --uninstrumented-cidx /tmp/hse103-uninstrumented/build/cidx \
  --checkout . \
  --self-compile-db build/compile_commands.json \
  --representative-files 32 \
  --scale-files 1000 \
  --many-header-target 128 \
  --trials 3 \
  --work-root /tmp/hse103-production.noindex \
  --output /tmp/hse103-production.json
```

The output retains every trial, median aggregates, exact executable/commit,
schema/catalog/compile-database/host/SQLite/Clang identities, canonical
semantic parity, and the `--profile-json` per-TU series. The analysis compares
constant, linear, and quadratic-superlinear models with residuals, coefficient
standard errors, RMSE, R², and AIC. The SQLite matrix always includes the
shipped profile as its control and never recommends a production setting.
Index and resolve stages both retain profiles: `clang_front_end` excludes the
registered visitor/persistence pass timings, `clang_tool_inclusive` retains the
enclosing LibTooling wall time, and `transforms` measures the actual resolve
pipeline.
Each disposable setting is SIGKILL-interrupted during a real `cidx index`,
resumed, integrity/foreign keys checked, and compared by canonical semantic
digest before a separate production-profile change can be considered.

S-075 adds an explicit front-end reuse qualification contract. Use
`--front-end-reuse none` (the only shipped mechanism) or
`--no-front-end-reuse` for diagnosis. The report records the three-candidate
matrix, retained trial fields, the versioned none identity, semantic/diagnostic
parity requirements, and that no generated artifact is constructed or injected.

`--uninstrumented-cidx` enables the required paired disabled-profiling overhead
gate. It records all six cold 1,000-TU trial values, both medians and spreads,
the percentage overhead, and paired canonical digests. Build that executable
from the immediately preceding revision in a disposable worktree with the same
compiler and build settings.

Raw profile JSON, generated corpora, databases, command output, and recovery
probes are written below `--work-root`, which must be outside the checkout.
On macOS give that directory a `.noindex` suffix so Spotlight does not index
the generated 1,000-TU corpora and contaminate later trials.
Copy only a completed summary into
`benchmarks/indexing/PRODUCTION_REPORT_TEMPLATE.md`.

## S-098 routed root-fusion decision harness

S-098 changes traversal topology, so the decision it is judged by is frozen
first. `production.py` carries the pure summary and threshold logic; nothing in
it measures, indexes, or writes.

The frozen S-071 evidence lives in two places:

- the authoritative full report, attached to backlog task S-098 as
  `s071-authoritative-r3.json`. It is resolved **through the backlog artifact
  registry** (`backlog artifact list S-098` plus the store's own artifact root
  from `backlog where`) — never from an assumed repository `.backlog` path and
  never from a disposable `/tmp` copy. `load_baseline_artifact()` verifies the
  recorded size and SHA-256 and re-derives the summary before trusting it,
  refusing with `baseline-artifact-missing` or `baseline-artifact-mismatch`
  instead of synthesizing evidence;
- `fixtures/s071-header-heavy-baseline.json`, a compact checked-in excerpt with
  the same field shape, so the unit tests run offline with no store, binary,
  corpus, or database.

Treat that evidence as immutable control: `header-heavy:8:forward`, three
trials, median cold wall 0.255732833 s, fixed routed-root sum 0.040611 s
(`root_symbols` 0.031091 s plus 0.009520 s across `root_declarations`,
`root_definitions`, and `root_namespaces`), and 32 registered / 32 observed
whole-TU visits in every trial.

`fusion_decision(report)` derives the per-trial and median fixed routed-root
time from those four timings and returns `ship_eligible` only when **all** of
S-098 AC #1720/#1721 hold:

- at least three trials, each authoritative and measured on a quiescent host;
- no parity failure at report, aggregate, or stage level;
- observed whole-TU traversals never above the registered budget in any trial;
- median fixed routed-root time **strictly** below 0.025 s. Exactly 0.025 s is
  a rejection.

Anything else is refused with a named reason, reported in this fixed order:
`baseline-artifact-missing`, `baseline-artifact-mismatch`, `malformed-report`,
`identity-mismatch`, `host-not-quiescent`, `insufficient-trials`,
`semantic-parity-failure`, `traversal-budget-exceeded`,
`fixed-root-budget-not-met`. `identity-mismatch` pins the reference-host
method — corpus case and stage, host platform/machine/CPU count, schema and
catalog identity, Clang resource directory, SQLite version. The executable
digest and checkout commit are deliberately excluded, because a candidate run
must differ there.

The T-139 symbol budget (`root_symbols` median strictly below 0.015480 s) is
recorded in the decision as `symbol_root_budget_met` and is advisory here:
T-139 owns that gate, and S-098 eligibility is governed by the 0.025 s total.

A full `production.py` run appends the same decision to its report as
`s098_root_fusion`, computed for `header-heavy:<representative-files>:forward`.
Only `--representative-files 8` matches the pinned baseline; any other size is
recorded with `identity-mismatch` rather than silently compared. The decision
never changes the process exit code, which still tracks `parity_failures`.

Decide over an existing candidate report:

```sh
python3 -c 'import json, sys; from benchmarks.indexing.production import fusion_decision; \
  print(json.dumps(fusion_decision(json.load(open(sys.argv[1]))), indent=2))' \
  /tmp/s098-candidate.json
```

Verify the harness itself — deterministic, offline, no build required:

```sh
python3 -m unittest benchmarks.indexing.production_test
python3 -m py_compile benchmarks/indexing/production.py
```

## Reproduce a comparison

Build the current binary in `build/cidx`. Build a baseline binary from the
comparison revision in a separate temporary worktree, then run:

```sh
python3 benchmarks/indexing/run.py \
  --baseline-cidx /tmp/hse95-baseline/build/cidx \
  --current-cidx build/cidx \
  --representative-files 32 \
  --scale-files 1000 \
  --per-tu 5 \
  --trials 3 \
  --output /tmp/hse95-indexing.json
```

The JSON report contains one case per trial, median aggregates, and a
`comparison` section for current versus baseline. It reports wall-time deltas,
CPU utilization, SQLite activity, semantic-digest parity, and schema/catalog
parity for cold, warm, and incremental index stages. Every trial is compared;
the process exits nonzero and records `parity_failures` if any trial or
intra-build repeat differs. It does not claim an improvement when no baseline
executable is supplied.

## Reproduce external-identity reconciliation A/B evidence

Use one current binary for both modes so the comparison isolates the
reconciliation strategy:

```sh
python3 benchmarks/indexing/run.py \
  --current-cidx build/cidx \
  --reconciliation-ab \
  --representative-files 32 \
  --scale-files 1000 \
  --per-tu 5 \
  --trials 3 \
  --output /tmp/hse114-indexing.json
```

The baseline side uses immediate per-emission reconciliation, while the current
side batches distinct symbol identities once per translation-unit transaction.
The standard semantic, diagnostic, schema/catalog, integrity, and foreign-key
parity gates still apply.

## Reproduce profiler evidence

`profile.py` creates a named disposable corpus and cache, records the exact
import and `xctrace` launch commands, exports the time-profile table, and
extracts inclusive frame counts into a JSON summary:

```sh
python3 benchmarks/indexing/profile.py \
  --cidx build/cidx --label current --files 8 \
  --work-root /tmp/hse95-profile-current \
  --trace /tmp/hse95-profile-current.trace \
  --xml /tmp/hse95-profile-current.xml \
  --summary /tmp/hse95-profile-current.json
```

The generated corpus is intentionally simple and stable: every source includes
`shared.hpp`, repeats its own declaration 16 times after a resolved definition,
and visits eight distinct call edges twice. TU 0 additionally includes
`coverage.hpp`, a focused fixture that exercises canonical projections for
includes, diagnostics, macro use, call arguments, templates, parameters, and
type edges; the harness asserts those sections are non-empty. This makes
shared-header fan-in, resolved-identity reuse, and fact-ID de-duplication
explicit while retaining the 1,000+ TU scaling shape. Use a checked-out
representative repository separately when a project-specific workload is
required; the same stage and measurement fields apply. The first per-TU sample
mutates TU 0 after the separate incremental state, so it is not a no-op.

The initial usability target is a cold 1,000-TU run in under 15 minutes on the
benchmark host, an unchanged warm run in under 5 seconds, and a one-TU
incremental re-index in under 2 seconds. These are operational targets for
this harness, not correctness thresholds; the report records the measured
values and host so they can be revisited with later profiling evidence.

## Platform profiler guidance

The existing `profile.py` helper remains the macOS `xctrace` entry point. Keep
its `.trace` and exported XML below `/tmp`.

On Linux, import into a disposable cache first, then capture the identical
`cidx index --profile-json` command with:

```sh
INDEXER_CACHE=/tmp/hse103-cache \
  perf record --call-graph dwarf --output /tmp/hse103-perf.data -- \
  build/cidx index --profile-json /tmp/hse103-perf-profile.json

INDEXER_CACHE=/tmp/hse103-cache \
  strace -ff -tt -T -c -o /tmp/hse103-strace -- \
  build/cidx index --profile-json /tmp/hse103-strace-profile.json
```

For SQLite statement evidence, use the profile JSON statement counters first.
When SQL text is required, build a disposable diagnostic binary with SQLite
trace hooks enabled and write the trace outside the checkout. Never enable SQL
text tracing for authoritative timing: trace volume changes the workload.
