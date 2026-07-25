# HSE-95 benchmark report

This report records the reproducible benchmark run for the HSE-95 indexing
changes. The JSON output is intentionally kept outside the repository; rerun
the harness to refresh the measurements on another machine.

## Run

The run used the checked-out `cidx` executable and an `origin/main` executable
built from baseline commit `002cf0c`. The generated corpus contained 32 and
1,000 translation units, one shared header, repeated resolved declarations,
and repeated call edges. Each executable was measured with the following
command shape:

```text
python3 benchmarks/indexing/run.py \
  --current-cidx build/cidx \
  --representative-files 32 \
  --scale-files 1000 \
  --per-tu 5 \
  --output /tmp/hse95-indexing-current.json
```

The baseline output was `/tmp/hse95-final-baseline.json`; the current output
after the final sink fast-path adjustment was `/tmp/hse95-final-current2.json`.

## Results

Wall-clock seconds, with peak resident memory in MiB:

| Corpus | Build | Cold index | Warm unchanged | Incremental one TU | Cold peak RSS |
| ---: | --- | ---: | ---: | ---: | ---: |
| 32 files | baseline | 2.743 | 0.054 | 0.103 | 42.1 |
| 32 files | HSE-95 | 3.490 | 0.063 | 0.121 | 43.2 |
| 1,000 files | baseline | 313.894 | 0.320 | 0.727 | 51.4 |
| 1,000 files | HSE-95 | 463.984 | 0.197 | 0.398 | 50.1 |

The usability targets are cold 1,000-file indexing under 900 seconds, warm
reindexing under 5 seconds, and one-file incremental indexing under 2 seconds.
The HSE-95 run met all three targets. The harness also records child wall and
CPU time, peak RSS, SQLite page and row deltas, shared-header fan-in, indexed
versus already-indexed counts, and five per-translation-unit samples.
