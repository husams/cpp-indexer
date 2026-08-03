# docs/prefs — indexer performance experiments

Performance measurements of `cidx`, with the raw data they were derived from.
Each experiment is one dated report plus its raw artifacts under `raw/`.

## Index

| Date | Experiment | Report | Headline result |
| --- | --- | --- | --- |
| 2026-08-03 | CPU profile of `cidx index` on a single translation unit | [`index-single-tu-profile-2026-08-03.md`](index-single-tu-profile-2026-08-03.md) | 84 % of the run is SQLite; the hottest method is `SqliteStorageService::lookup_symbol` at 62 % inclusive, because its query cannot use the partial index `idx_symbol_identity` |

---

## Experiment 2026-08-03 — single-TU index profile

### Question

For one translation unit, **which function/method and which call path consume
the most time during `cidx index`?**

Deliberately scoped to a single file so the answer is a clean per-TU cost
breakdown rather than an aggregate over a whole repository.

### Design

| Choice | Value | Why |
| --- | --- | --- |
| Object under test | `src/query/exec.cpp` | Largest TU in the tree (4,735 lines, 68.6 MiB preprocessed) — the heaviest single-file workload available, so hot spots are well above sampling noise |
| Build | fresh out-of-tree `RelWithDebInfo` (`-O2 -g`) | The checked-in `build/` is `Debug`, which distorts relative costs; `Release` has no debug info so `sample` cannot symbolize static functions. `RelWithDebInfo` gives production optimization *and* full symbolization |
| Database | fresh, empty `index.db` (`INDEXER_CACHE` pointed at a scratch dir) | Cold-start behavior: every symbol and type is minted, not matched. This is the expensive path and the one that scales with repository size |
| Wall-clock/phase data | `cidx index --profile-json` | Built-in, opt-in telemetry (`src/profile/index_profile.hpp`) — authoritative pass/phase timings, fact counters, and SQLite counters that a sampler cannot see |
| CPU profile | `/usr/bin/sample <pid> 60 1 -mayDie` | 1 ms sampling of the whole process lifetime, attached **by PID**. Gives the call graph, inclusive and self time |
| Cross-check | `EXPLAIN QUERY PLAN` + a SQL micro-benchmark on the produced database | Turns "SQLite is hot" into a specific, falsifiable root cause |

Two independent instruments were used on purpose: `--profile-json` says *which
pass* is slow, `sample` says *which function and path* is slow, and they agree
(`sqlite_vdbe` 16.06 s ≈ `libsqlite3.dylib` self time 15.68 s).

### Method

```bash
# 1. Build a profiling binary (not the repo's Debug build/)
cmake -S . -B /tmp/build-prof -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCIDX_LLVM_CONFIG=/opt/homebrew/opt/llvm/bin/llvm-config \
  -DCIDX_BUILD_EXAMPLES=OFF -DCIDX_ASTGRAPH_SOUFFLE=OFF
ninja -C /tmp/build-prof cidx

# 2. Fresh, empty index database
export INDEXER_CACHE=/tmp/profrun            # index.db is created here
/tmp/build-prof/cidx import --db "$(pwd)/build" --name cpp-indexer

# 3. Index exactly ONE file, sampled by PID
/tmp/build-prof/cidx index src/query/exec.cpp --profile-json /tmp/profile.json &
sample $! 60 1 -mayDie -f /tmp/sample.txt

# 4. Derive the hotspot tables from the raw call graph
python3 docs/prefs/raw/analyze_sample.py /tmp/sample.txt
```

Attach **by PID**, not by process name: a first attempt with `sample cidx -wait`
latched onto an unrelated long-running `cidx ui open` process from another
session and produced a profile of the wrong program. That data was discarded.

### Analysis of the raw call graph

`sample` emits an indented call tree where each line is
`<count> <symbol> + <offset> [<addr>] <file:line>`. `raw/analyze_sample.py`
rebuilds the tree from the indentation column and computes:

- **self time** — a node's count minus the sum of its children's counts;
- **inclusive time, recursion-collapsed** — a symbol's count is added only when
  no ancestor frame carries the same symbol, so the deeply recursive
  `TraverseIfStmt` → `TraverseStmt` chain in `exec.cpp` is counted once instead
  of ~30 times;
- **hottest path** — greedy descent through the heaviest child from the root.

### Threats to validity

- **Host load.** An unrelated `cidx ui open` process from another session was
  pinned at 100 % CPU during the sampled run, and load rose further (555 % +
  95 %) before the repeat micro-benchmarks. The indexer is single-threaded on a
  10-core M4 so contention is limited, but absolute seconds carry that noise.
  Percentages and ratios are the trustworthy figures; the SQL micro-benchmark
  is therefore reported as a ratio (40–57×) across both load conditions.
- **Cold database.** With a warm database more lookups would *hit*, but the
  hot query scans regardless of hit or miss, so the finding holds — and the
  scan gets more expensive as the table grows.
- **One TU, one repo.** The 84 %-SQLite split is measured on this codebase's
  largest file. Header-heavy versus body-heavy TUs will shift the
  `body_extraction` / `pass.*.headers` balance.
- **`sample` is a wall-clock sampler**, so blocked time counts. Here the
  process was 98 % CPU-bound, so self time ≈ CPU time.

### Raw data (`raw/`)

| File | What it is |
| --- | --- |
| `sample-callgraph.txt.gz` | Verbatim `/usr/bin/sample` output — full call graph, per-symbol self ranking, binary images. `gunzip -c` to read (8.8 MB uncompressed) |
| `profile-json.json` | Verbatim `cidx index --profile-json` output: phase timings, fact-family counters, SQLite counters, per-TU record |
| `hotspots-derived.txt` | Tables derived from the call graph: self samples by binary image, verbatim per-function self ranking, inclusive per-frame ranking |
| `sql-evidence.txt` | `symbol` index definitions, `EXPLAIN QUERY PLAN` for the hot query in both forms, and the micro-benchmark runs |
| `analyze_sample.py` | The parser/aggregator used to produce `hotspots-derived.txt` |
| `environment.txt` | OS, kernel, CPU, RAM, compilers, LLVM and SQLite versions, repo commit, and the exact compile command for the profiled TU |
| `index-stdout.log` | stdout/stderr of the profiled `cidx index` invocation |
| `cmake-configure.log`, `ninja-build.log` | Configure and build logs for the profiling binary |

### Reproducing the analysis from the raw data

```bash
gunzip -c docs/prefs/raw/sample-callgraph.txt.gz > /tmp/sample.txt
python3 docs/prefs/raw/analyze_sample.py /tmp/sample.txt
```
