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

Generated sources, caches, logs, and JSON reports belong outside the checkout.
The runner uses a temporary `INDEXER_CACHE`; it never opens the checkout's
database. Keep reports under `/tmp` (or another disposable directory).

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
  --output /tmp/hse95-indexing.json
```

The JSON report contains one case per requested corpus size and executable.
The `comparison` section reports current-versus-baseline wall-time deltas for
the cold, warm, and incremental index stages. The harness does not claim an
improvement when no baseline executable is supplied.

The generated corpus is intentionally simple and stable: every source includes
`shared.hpp`, repeats its own declaration 16 times after a resolved definition,
and visits eight distinct call edges twice. This makes shared-header fan-in,
resolved-identity reuse, and fact-ID de-duplication explicit while retaining the
1,000+ TU scaling shape. Use a checked-out
representative repository separately when a project-specific workload is
required; the same stage and measurement fields apply.

The initial usability target is a cold 1,000-TU run in under 15 minutes on the
benchmark host, an unchanged warm run in under 5 seconds, and a one-TU
incremental re-index in under 2 seconds. These are operational targets for
this harness, not correctness thresholds; the report records the measured
values and host so they can be revisited with later profiling evidence.
