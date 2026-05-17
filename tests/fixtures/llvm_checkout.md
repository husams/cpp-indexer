# LLVM checkout acquisition checklist

Story: S21-m3-perf-gate
AC covered: AC-M3-3, AC-M3-4, AC-M3-5, AC-M3-10, AC-M3-11

This file documents how to obtain the LLVM source tree used as the performance
fixture for `benches/llvm_index.rs`.  The source tree is NOT committed to this
repository (≈ 4 GiB checkout).

## Requirements

- A 32-core Linux machine (for AC-M3-3/11 assertions).
- At least 40 GiB of free disk space (source + stage + target dirs).
- LLVM commit: any recent `release/18.x` tag is acceptable.

## Steps

1. Clone the LLVM monorepo (shallow clone to save space):

   ```bash
   git clone --depth 1 --branch llvmorg-18.1.8 \
       https://github.com/llvm/llvm-project.git \
       /data/llvm-project
   ```

2. Generate `compile_commands.json` using CMake:

   ```bash
   cmake -S /data/llvm-project/llvm \
         -B /data/llvm-project/build \
         -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
         -G Ninja
   # The build itself is NOT needed — only compile_commands.json.
   cp /data/llvm-project/build/compile_commands.json \
      /data/llvm-project/llvm/compile_commands.json
   ```

3. Build the indexer binary:

   ```bash
   cd /path/to/cpp-indexer
   cargo build --bin cxg-index
   ```

4. Run the perf gate:

   ```bash
   CXG_M3_LLVM_PATH=/data/llvm-project/llvm BENCH=1 \
       cargo bench --bench llvm_index
   ```

   Results are written to `target/bench/llvm-<unix-timestamp>.json`.

## Expected output (passing)

```
llvm_index: [AC-M3-3]  wall=XXXs  limit=900s  PASS=true
llvm_index: [AC-M3-4/5] rows=N throughput=Y rows/s  neo4j_ok=true  indradb_ok=true
llvm_index: [AC-M3-11] peak_rss=Z MiB  limit=16384 MiB  PASS=true
llvm_index: [AC-M3-10] wall=Ws  limit=60s  PASS=true
llvm_index: all AC thresholds passed.
```

## Notes

- The bench uses `--backend mock` so no live database is required for timing runs.
- To measure true sink throughput against Neo4j/IndraDB, run `cxg-index` manually
  with the appropriate `--backend` flag and compare the printed `nodes/s` figures.
- RSS measurement is only active on Linux (`getrusage(RUSAGE_CHILDREN)`);
  on macOS the AC-M3-11 check is skipped with a notice.
- The incremental run (AC-M3-10) touches
  `llvm/lib/Support/raw_ostream.cpp` to trigger one cache miss.
  If that file is absent in the chosen LLVM version, AC-M3-10 is skipped.
