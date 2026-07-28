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
- repeated trials with median timing, CPU utilization, and per-TU latency
  aggregation, including per-trial and intra-build parity checks.

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
Each disposable setting is SIGKILL-interrupted during a real `cidx index`,
resumed, integrity/foreign keys checked, and compared by canonical semantic
digest before a separate production-profile change can be considered.

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
