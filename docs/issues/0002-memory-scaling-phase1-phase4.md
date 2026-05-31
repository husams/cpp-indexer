# Issue 0002 — Memory scaling: Phase 1 glibc arena retention + Phase 4 whole-graph materialization

- **Status:** design (do NOT implement from this doc alone; it is implementation-ready for a follow-up PR)
- **Date:** 2026-05-30
- **Repo HEAD:** `2f6d8b8` (schema v6)
- **Binary:** `cxg-index` (`src/bin/index.rs`)
- **Found by:** real `grpc` C++ index run — 2517 TUs, 160,580 functions, 15 GiB Linux VM, sink = IndraDB
- **Scope:** exactly two OOM bugs. No feature work. See [Deferred](#deferred).

---

## Summary

Two independent OOM bugs were observed on a single large run. Both are confirmed in source.

| Bug | Phase | Confirmed root cause (file:line) | Chosen fix |
|-----|-------|----------------------------------|------------|
| 1 | Phase 1 (parallel libclang parse) | No `#[global_allocator]` → libclang ASTs allocate through **system glibc malloc**, which spins up one arena per rayon worker thread and never returns freed AST memory to the OS. Secondary: per-thread `clang::Index` is `Box::leak`'d once and reused for every TU (`src/visit/shallow.rs:90-98`), accumulating libclang Index-level state. | glibc-side, Linux-cfg-gated: `mallopt(M_ARENA_MAX, 2)` at top of `main`, periodic `malloc_trim(0)`, and recycle the per-thread `Index` every N TUs. macOS untouched. **A jemalloc global-allocator swap does NOT fix this** (see Bug 1). |
| 2 | Phase 4 (sink write) | `load_nodes_from_stage` / `load_edges_from_stage` build the **whole graph** into `Vec`s (`src/pipeline/mod.rs:289-291, 565-652`), then the IndraDB sink expands every node into a `Vec<BulkInsertItem>` sized **`batch.len() * 17`** (`src/sink/indradb.rs:355`) — a second, ~17× copy — before chunking. Whole `Vec<NodeRecord>` + 17× item expansion + per-chunk `.to_vec()`+`.clone()` are resident simultaneously: the 11→16 GiB jump. | Stream staged Parquet in bounded record-batches; convert→write→drop per batch; never hold the full graph or full item-Vec. Preserve dedup with a bounded two-pass. Add `--write-only` resume. |

Both confirmed against source; two of my reviewer's a-priori hypotheses were **corrected** by the code (see [Where the code contradicted the hypotheses](#where-the-code-contradicted-the-hypotheses)).

---

## Evidence (empirical, 2026-05-30 grpc run)

### Bug 1 — Phase 1
- 8 rayon workers, **default glibc malloc**: RSS climbed monotonically 7 → 12.7 GiB in ~5 min, OOM-killed — even though small TUs completed ~1/sec and their ASTs were freed.
- Setting env `MALLOC_ARENA_MAX=2` (same 8 workers) made RSS **plateau at ~6 GiB**. Proof the leak is glibc arena retention, not a true leak.
- Even with the arena cap, RSS still crept ~6→11 GiB across all of Phase 1 — consistent with Index-level accumulation in the reused per-thread `clang::Index`.

### Bug 2 — Phase 4
- Phases 1–3 completed in ~1h49m, RSS stable ~11 GiB (arena cap on).
- At Phase 4 ("writing to sink 'indradb'") RSS jumped 11 → 16 GiB in ~2 min; OOM-killed (kernel: anon-rss 15.87 GiB + 11.6 GiB swap, total-vm 39.7 GB, SIGKILL). **Nothing was persisted** — the entire ~1h49m of Phases 1–3 was lost because there is no resume.

### Related prior incident (wiki)
`[[pages/research/cpp-indexer-issue-0001-baseline-2026-05-20]]` records a **Phase 4 Neo4j JVM OOM on 200k-edge batches**. That is a *server-side* (Neo4j JVM heap) OOM and is **distinct** from Bug 2 here, which is *client-side* `cxg-index` RSS. Streaming smaller transactions helps both, but they are different failure surfaces; this issue fixes the client side.

---

## Bug 1 — Phase 1: glibc per-thread arena retention (+ Index accumulation)

### Root cause confirmed in code

1. **No custom global allocator.** `grep -rn "global_allocator\|jemalloc"` over `src/` → nothing. `Cargo.toml` has no `tikv-jemallocator`/`mimalloc`. On Linux the process therefore uses glibc malloc.
2. **libclang allocates through the system libc, not the Rust allocator.** The transient AST heap (7→12.7 GiB) is created by `libclang.so` C++ `operator new`/`malloc`, resolved against glibc at libclang's own link time. With 8 worker threads each doing huge transient allocations, glibc opens up to `8 × MALLOC_ARENA_MAX_default` arenas and retains freed chunks per-arena rather than returning them to the OS — classic many-thread arena fragmentation/retention. `MALLOC_ARENA_MAX=2` plateauing RSS is the proof.
3. **Per-thread `Index` reuse (secondary, confirmed).** `src/visit/shallow.rs:77-98`:
   ```rust
   static THREAD_INDEX: OnceCell<&'static Index<'static>> = const { OnceCell::new() };
   pub fn with_thread_index<R>(f: impl FnOnce(&Index<'static>) -> R) -> R {
       // Box::leak(Box::new(Index::new(&GLOBAL_CLANG, ...)))  — once per thread, never disposed
   }
   ```
   `src/pipeline/parallel.rs:178, 198, 211` call `with_thread_index` for every TU. The `Index` is created once per rayon worker and reused across all TUs that worker handles for the whole of Phase 1. libclang `Index` objects are known to accumulate memory across thousands of parses — explains the residual ~6→11 GiB creep even with the arena cap.

### Why the obvious Rust fix does NOT work
`#[global_allocator] = tikv_jemallocator::Jemalloc` only redirects **Rust-side** allocations (Vecs, Arrow buffers). libclang's ASTs never route through a Rust `GlobalAlloc`, so a global-allocator swap leaves the Phase-1 libclang heap on glibc and **does not fix Bug 1**. (It *would* help the Phase-4 Rust-side buffers — noted under Bug 2, but Bug 2 has a more direct structural fix.) The only way jemalloc helps libclang is `LD_PRELOAD=libjemalloc.so`, which is a launch-wrapper, not a code change.

### Options

| # | Option | Effect on libclang heap | Portability | Verdict |
|---|--------|-------------------------|-------------|---------|
| a | `#[global_allocator]` → `tikv-jemallocator` / mimalloc / tcmalloc | **None** on libclang (Rust-only): a Rust `GlobalAlloc` does not intercept libclang's C++ `new`/`malloc`. Same verdict for tcmalloc as a global allocator. | All platforms. | ❌ Does not address Bug 1. |
| b | `mallopt(M_ARENA_MAX, 2)` at top of `main()` via `libc` crate | Caps glibc arenas process-wide *including* libclang. Reliable in-process equivalent of the env var. | glibc/Linux only → cfg-gate. | ✅ **Primary.** |
| b′ | `std::env::set_var("MALLOC_ARENA_MAX","2")` in `main` | Unreliable: glibc reads this tunable at first malloc, which the Rust runtime may already have triggered before `main`. | — | ❌ Use `mallopt` instead. |
| c | Periodic `malloc_trim(0)` between batches | Returns freed top-of-arena pages to the OS; counters the residual creep. | glibc/Linux only → cfg-gate. | ✅ **Combine with (b).** |
| d | Recycle per-thread `clang::Index` every N TUs | Bounds Index-level accumulation (the ~6→11 GiB residual). | All platforms. | ✅ **Combine.** Safe only because each parse is fully consumed inside the `with_thread_index` closure (no live `TranslationUnit` borrow survives recreation). |
| e | Bound worker count | Fewer arenas, but linear throughput loss; band-aid. | All platforms. | ⚪ Keep as a documented config lever, not the fix. |
| f | `LD_PRELOAD=libjemalloc.so` launch wrapper | Best fragmentation behavior for libclang too. | Linux op-side only. | ⚪ Document as heavier alternative; not the default code change. |

### Recommendation — combine (b) + (c) + (d), all `cfg(target_os = "linux")`-gated

- **(b)** `mallopt(M_ARENA_MAX, 2)` as the very first statement in `main()` (before any libclang load/alloc). Make the cap configurable via `[index].malloc_arena_max` / `--malloc-arena-max` (default 2, `0`/absent = leave glibc default). This is the load-bearing fix — replicates the proven env-var result in-process.
- **(c)** Call `malloc_trim(0)` from the Phase-1 driver on a cadence (e.g. once every `trim_interval` TUs per worker, default 64) to counter residual creep.
- **(d)** In `with_thread_index`, track a per-thread parse counter; when it crosses `index_recycle_interval` (default 256, configurable), drop and recreate the leaked `Index`. Because the old `&'static Index` was `Box::leak`'d, recycling must **explicitly free** the previous box (replace `OnceCell` with `RefCell<Option<Box<Index>>>` or `RefCell<Index>` and `Box::leak` only the live one, or hold the box owned in the thread-local and pass `&*box`). Guard: assert no `TranslationUnit` borrow outlives the closure — already true (each `visit_tu_with_index` / `parse_module_tu` consumes its TU inside the closure body).

macOS uses a different allocator (the arena issue is glibc-specific); **(b)** and **(c)** are `cfg`-gated out on macOS, **(d)** is portable and harmless. No macOS regression.

### Affected files / functions
- `src/bin/index.rs` `main()` — add `cfg(linux)` `mallopt` call first; new CLI flag `--malloc-arena-max`.
- `src/visit/shallow.rs` `with_thread_index` (l.90-98), `THREAD_INDEX` (l.77-79) — own the `Index` in the thread-local, add recycle counter.
- `src/pipeline/parallel.rs` `run_phase1_parallel` (l.101-259) — pass `trim_interval`/`index_recycle_interval`; call `malloc_trim` on cadence.
- `src/config/mod.rs` `[index]` (l.85 `workers` neighborhood) — add `malloc_arena_max`, `trim_interval`, `index_recycle_interval`.
- New module `src/mem/glibc.rs` — `cfg(linux)` safe wrappers `set_arena_max(n)` and `trim()` around `libc::mallopt` / `libc::malloc_trim`; no-op stubs elsewhere. (`libc` is already a dev-dependency at `Cargo.toml:69`; promote to a normal dependency.)

### Implementation plan (Bug 1)
1. Add `libc` to `[dependencies]`. Add `src/mem/glibc.rs` with cfg-gated `set_arena_max`/`trim` (unsafe FFI isolated here, documented `# Safety`).
2. Call `cpp_indexer::mem::glibc::set_arena_max(cfg.malloc_arena_max)` as the first line of `main()`.
3. Refactor `with_thread_index` to own its `Index` and recycle after N parses; keep the public signature.
4. Thread `trim_interval` into `run_phase1_parallel`; call `glibc::trim()` from the worker hot path on cadence (cheap; `malloc_trim` is a no-op when nothing to return).
5. Wire the three config knobs (CLI > env > file > default) using the existing precedence helpers in `src/config/mod.rs`.

### Tests (Bug 1)
- **Unit:** `glibc::set_arena_max`/`trim` are callable and return Ok on Linux, no-op on non-Linux (cfg-gated test). Index-recycle counter resets correctly (inject a fake parse via the existing `run_with_parser` harness; assert recreate fires every N).
- **Integration (`cfg(linux)`, `#[ignore]` + gated on `BENCH=1` + libclang):** parse a large synthetic corpus (N generated `.cpp` TUs) at 8 workers with `malloc_arena_max=2`; sample peak RSS via **`getrusage(RUSAGE_SELF).ru_maxrss`** (NOT `/usr/bin/time` — DYLD stripped on macOS, and we want in-process anyway). Assert peak RSS < cap (e.g. < 8 GiB for the synthetic size).
  - **Note on AC-1.2 (no monotonic climb):** `ru_maxrss` is peak-only/monotonic and CANNOT show a *climbing curve*. Prove AC-1.2 with either (i) time-sampled `VmHWM`/`/proc/self/statm` over the run, or (ii) the capped-vs-uncapped peak **A/B delta** (`--malloc-arena-max 0` reproduces the old climb to OOM; the capped run plateaus). Use the A/B delta as the assert; the time-sample as a diagnostic.

### Risks (Bug 1)
- **Index recycle must not free a live borrow.** Recreating the per-thread `Index` while a `TranslationUnit` borrowed from it is alive is UB. Safe here because each parse is fully consumed inside the `with_thread_index` closure — enforce with a debug assert / doc-comment and never expose the `Index` across the closure boundary.
- **`mallopt` ordering.** `M_ARENA_MAX` must be set before any worker thread spawns or libclang loads/allocates; placing it as the first statement in `main()` guarantees this. If set late, already-opened arenas persist.
- **`malloc_trim` cost.** Calling too frequently adds syscall/contention overhead; the `trim_interval` (default 64 TUs) keeps it cheap. `malloc_trim` is a no-op when nothing is returnable.
- **Lower arena count can reduce alloc throughput** under heavy contention; `--malloc-arena-max 0` restores the default for A/B if a regression appears.

### Acceptance criteria (Bug 1)
- AC-1.1: With defaults, Phase 1 on grpc (2517 TUs, 8 workers) holds peak RSS ≤ the `MALLOC_ARENA_MAX=2` baseline (~6 GiB) with **no env var set** by the operator.
- AC-1.2: Residual creep across all of Phase 1 is bounded (no monotonic climb to OOM); Index recycling demonstrably caps it.
- AC-1.3: macOS build unaffected: no `mallopt`/`malloc_trim` compiled in; full test suite green on macOS.
- AC-1.4: New config knobs documented; `--malloc-arena-max 0` restores prior (uncapped) behavior for A/B.

---

## Bug 2 — Phase 4: whole-graph materialization in the sink write

### Root cause confirmed in code
1. **Loaders return the whole graph.** `src/pipeline/mod.rs:289-291`:
   ```rust
   let mut node_records = load_nodes_from_stage(&stage_dir)?;   // Vec<NodeRecord> — ALL nodes
   let mut edge_records = load_edges_from_stage(&stage_dir)?;   // Vec<EdgeRecord> — ALL edges
   ```
   `load_nodes_from_stage` (l.565-632) builds a `HashMap<String,(u8,NodeRecord)>` keyed by USR over **every** node in every shard (phase1 worker shards + phase2 shards), then `.collect()`s to a `Vec`. `load_edges_from_stage` (l.635-652) `extend`s a `Vec` with every row of `final-edges.parquet`.
2. **REPO-node fan-out doubles node-count.** `src/pipeline/mod.rs:357-383` builds `belongs_edges: Vec<EdgeRecord>` with one edge per node (≈160k+ more records held at once).
3. **The sink expands ~17× on top.** `src/sink/indradb.rs:355`:
   ```rust
   let mut all_items: Vec<BulkInsertItem> = Vec::with_capacity(batch.len() * 17);
   ```
   For 160k nodes that is up to ~2.7M `BulkInsertItem`s, each owning a `Json` property clone, built **before** any chunking — while `node_records` is still alive. Then per chunk `item_chunk.to_vec()` (l.496) **and** `items.clone()` inside the retry task (l.503) make two more copies of each in-flight chunk. The Neo4j sink does the analogous full `all_rows: Vec<BoltType>` up-front (`src/sink/neo4j.rs:617, 666`).
4. **The per-record hog is `code`.** `NodeRecord.code` is a ≤32 KiB snippet (M8). 160k × up-to-32 KiB ≈ multi-GB just in node payload — which is why even the deduped node `Vec` is large, and why row-count batching alone is insufficient.

Net: whole `Vec<NodeRecord>` + ~17× `BulkInsertItem` expansion + per-chunk double-copy resident at the same time = the 11→16 GiB jump. **Hypothesis confirmed and located.**

### What MUST be preserved (do not naively stream)
`load_nodes_from_stage`'s dedup is load-bearing:
- **phase1→phase2 merge** (`src/pipeline/mod.rs:587-624`): phase2 record replaces phase1. **Verified safe to stream**: `src/visit/decorate.rs:114-120` shows Phase 2 reads the full Phase-1 `NodeRecord`, patches `attrs_json`, sets `phase=2`, and writes the **complete** record back — it is a superset, not a delta. So an upsert that sees phase1 then phase2 loses nothing *content-wise*. BUT IndraDB skips `None` fields (`src/sink/indradb.rs:400+`), and only Function/Method nodes get phase2 shards, so ordering still matters → keep an explicit winner-selection rather than relying on upsert order.
- **cross-shard USR collapse**: the same header symbol emits a node in *every* TU's shard. Dropping dedup would push **millions** of upserts instead of ~160k — large write-amplification and a likely new throughput bottleneck. Must keep dedup.

### Design — bounded two-pass streaming sink write

Replace the load-everything-then-write block (`src/pipeline/mod.rs:289-403`) with a streaming writer that never holds the whole graph.

**Nodes — two pass (bounded):**
- **Pass 1 (index build, tiny):** scan all node shards reading only the key columns (`symbol_id`/`usr`, `phase`) and build `HashMap<i64 symbol_id, u8 winning_phase>` plus an emitted-`HashSet<i64>`. For 160k symbols this map is ~1.5 MB — negligible. (Key on `symbol_id` per v6; USR string no longer needed in the graph.)
- **Pass 2 (stream + write):** re-scan shards in **record-batches**; for each record whose `phase == winning_phase[symbol_id]` and not yet emitted, accumulate into a bounded in-flight buffer; when the buffer hits the byte/row cap, hand it to `sink.write_nodes(&buf)`, await, `buf.clear()`. Emit the REPO node first; generate each node's `BELONGS_TO_REPO` edge **inline** during the node stream (no separate `belongs_edges` Vec).
- Peak node memory = one batch (cap), not the graph.

**Edges — single pass:** stream `final-edges.parquet` in record-batches straight to `sink.write_edges(&buf)`. `dedupe_edges_for_sink` (`src/pipeline/mod.rs:417`) does **two** things: (a) collapse duplicate sink keys, and (b) drop unresolved edges. **Split them — do not move the whole function:**
- **(a) dedup → Phase 3** (`resolve_per_repo`, `src/pipeline/mod.rs:264`): collapse duplicate sink keys when writing `final-edges.parquet`, so Phase 4 needs zero in-memory edge dedup.
- **(b) unresolved-filter → leave at the sink boundary.** The sinks **already** filter unresolved edges before chunking (`src/sink/neo4j.rs:1358` test `unresolved_edges_filtered_before_chunking`; IndraDB `edge_to_bolt`/`filter_map` path), so Phase 4 needs no unresolved filter at all.

**Do NOT strip unresolved edges from `final-edges.parquet`.** Verified: Phase 5 cross-repo (`cxg-resolve-cross-repo`) reads `final-edges.parquet` and depends on exactly the unresolved rows — `src/resolve/cross_repo.rs:19-23, 410-431` scan for `cross_repo_candidate=true` edges, which carry `resolved=false`, to materialize `EXTERNAL_REF`. Stripping them in Phase 3 would silently break cross-repo. Keeping the persisted file intact (deduped but with unresolved rows retained) preserves both cross-repo and `--write-only`, since the sink drops unresolved at write time anyway.

**Batch sizing — byte-aware, not row-only:**
- Current `DEFAULT_BATCH_SIZE = 10_000` rows (`src/config/mod.rs:228`) is row-based and ignores the 32 KiB `code` payload. Add a **byte budget** `[sink].write_buffer_bytes` (default ~64 MiB): flush when *either* row count ≥ `batch_size` *or* accumulated record bytes ≥ budget. This bounds the worst case where a buffer is full of large `code` snippets.
- Inside the IndraDB sink, the `batch.len() * 17` expansion (`src/sink/indradb.rs:355`) now operates on a bounded buffer (≤ budget), so peak there is bounded too. Additionally **remove the double-copy**: `item_chunk.to_vec()` (l.496) is needed to move into the task, but `items.clone()` on every attempt (l.503) clones on the happy path — restructure so the clone happens **only on actual retry** (own `items`, clone lazily inside the retry branch). Same review for Neo4j `chunk.to_vec()` (l.642, 687).

**Sink-impl interaction & transaction boundaries:**
- The `GraphSink` trait (`src/sink/mod.rs:97-100`) `write_nodes(&[NodeRecord]) -> WriteStats` is **unchanged** — Phase 4 now calls it many times with bounded slices instead of once with the whole graph. Both impls already chunk internally; the only impl change is the byte-aware buffer + lazy-retry-clone, which lives in the impls.
- **IndraDB** transaction boundary = each `bulk_insert` chunk (already idempotent via deterministic `symbol_id_to_uuid`, `src/sink/indradb.rs:358`). Streaming changes nothing about idempotency.
- **Neo4j** boundary = each UNWIND-MERGE chunk; node MERGE keyed on `(symbol_id, repo_name)`, edge MERGE on `(src_id, dst_id, kind)` (`src/sink/neo4j.rs:74, 104`) — idempotent, so re-running a partially-written Phase 4 is safe (server-side OOM caveat is the wiki's separate Neo4j-JVM issue; smaller txns help it too).
- **Back-pressure:** the bounded buffer + `await` on each `write_nodes`/`write_edges` is the back-pressure — the loader cannot run ahead of the sink. Sink-internal `sessions` (default 16, `src/config/mod.rs:231`) still bounds in-flight chunks per call.
- **Idempotency / partial failure:** a crash mid-Phase-4 leaves a partially-written graph; because all writes are MERGE/upsert, re-running Phase 4 (via `--write-only`, below) converges to the same state. No "nothing persisted" data loss path.

### Resume / `--write-only` (strongly recommended)
A killed Phase 4 today forces redoing Phases 1–3 (~1h49m). The staged artifacts already persist on disk:
- node/edge Parquet shards in `<stage_dir>/worker-*/`, `phase2-nodes-*.parquet`, `final-edges.parquet`;
- the symbol map `<stage_dir>/cxg-symbols.db`.

Add `--write-only` (and `[index].write_only`) that **skips Phases 0–3** and runs Phase 4 from an existing `--stage-dir`. Phase 4's only two in-memory dependencies that don't naturally survive a fresh process:
- **`SymbolAllocator`** — reopen from SQLite: `SymbolAllocator::open(&symbol_db_path, cache_size)` already exists (`src/resolve/symbol_map.rs:211`). The REPO node still needs `get_or_insert_symbol/file` (l.223/231) which work on the reopened handle.
- **`libclang_version`** — used only for the SchemaVersion node attrs (`src/pipeline/mod.rs:181-187, 281`). Re-derive cheaply with `clang::get_version()` in write-only mode (no parse needed), or persist it into the stage dir during Phase 1 and read it back. Re-derive is simpler.

This is low-risk: it reuses existing open paths and the existing `--stage-dir` flag. Guard: error clearly if `--write-only` is given without a populated `--stage-dir`.

### Affected files / functions (Bug 2)
- `src/pipeline/mod.rs`:
  - Replace l.289-403 with a streaming writer (new `fn write_graph_streaming(sink, stage_dir, opts, allocator) -> Result<(u64,u64)>`).
  - `load_nodes_from_stage` (l.565-632) → split into `build_node_winner_index` (pass 1) + a batch-yielding `stream_nodes` (pass 2). Keep `collect_shards`/`collect_phase2_shards`.
  - `load_edges_from_stage` (l.635-652) → `stream_edges` (batch iterator).
  - Move `dedupe_edges_for_sink` (l.417) into Phase 3 (`src/resolve/per_repo.rs`).
  - REPO node + inline BELONGS_TO_REPO generation moved into the node stream.
  - `run` (l.110) → branch on `write_only` to skip Phases 0–3 and reopen the allocator / re-derive libclang version.
- `src/sink/indradb.rs` `write_nodes`/`write_edges` (l.337, 533) — byte-aware not required here (caller bounds the buffer) but **fix lazy-retry-clone** (l.496/503). 
- `src/sink/neo4j.rs` `write_nodes`/`write_edges` (l.606, 664) — same lazy-clone review (l.642, 687).
- `src/config/mod.rs` — add `[sink].write_buffer_bytes`, `[index].write_only`.
- `src/bin/index.rs` — add `--write-only`; pass `write_buffer_bytes`.

### Implementation plan (Bug 2)
1. Split `dedupe_edges_for_sink`: move the **dedup** half into Phase 3 (`resolve_per_repo`) so `final-edges.parquet` is pre-deduped; leave the **unresolved-filter** at the sink boundary (already present). Do **not** strip unresolved rows from the persisted file (cross-repo depends on them — `src/resolve/cross_repo.rs:410-431`). Delete the in-memory `dedupe_edges_for_sink` call from Phase 4.
2. Implement `build_node_winner_index` (pass 1, key columns only) and `stream_nodes`/`stream_edges` batch iterators over the existing shard sets.
3. Implement `write_graph_streaming`: REPO node first, stream nodes (emit inline BELONGS_TO_REPO), then stream edges; bounded buffer flush by row-or-bytes.
4. Add byte budget to the buffer flush; add `write_buffer_bytes` config.
5. Fix the per-chunk lazy-retry-clone in both sink impls.
6. Add `--write-only`: skip Phases 0–3, reopen `SymbolAllocator` from `cxg-symbols.db`, re-derive libclang version, run `write_graph_streaming`.

### Tests (Bug 2)
- **Unit:** `build_node_winner_index` picks phase2 over phase1 for the same symbol_id; duplicate-USR-across-shards collapses to one emission; byte-budget flush triggers on a buffer of large `code` records before the row cap; pre-deduped edges produce no duplicate sink keys (reuse the existing `dedupe_edges_removes_duplicate_sink_keys_and_unresolved_edges` assertions, moved to Phase 3).
- **Integration — memory-bounded (the headline test, `#[ignore]` + env-gated):** requires a real IndraDB server, so mark `#[ignore]`/env-gated (like the Bug 1 `BENCH=1` test) — CI without an IndraDB instance must not fail on it. Generate a large synthetic corpus → stage shards → run `write_graph_streaming` against a **throwaway IndraDB server** (the local VM `indradb-server` per the project's gcc-test workflow), NOT MockSink — because the ~17× `BulkInsertItem` expansion lives in the *real sink impl* and a counting mock won't exercise it. Sample peak RSS via `getrusage(RUSAGE_SELF).ru_maxrss`; assert peak stays under a cap (e.g. < 3 GiB) independent of corpus size, and that a 2× corpus does **not** ~2× peak RSS (the streaming invariant). Verify row counts in IndraDB match the deduped expectation.
- **Resume:** stage a corpus, run `--write-only` against a fresh process + reopened SQLite; assert the graph matches a single-shot run (idempotent re-run also matches).
- **Parity:** existing `sink_parity` / `indradb_properties` / `neo4j_indexes` tests must stay green (streaming must not change written content).

### Risks (Bug 2)
- **Edge dedup relocation must not strip unresolved rows.** Moving the whole `dedupe_edges_for_sink` into Phase 3 would drop `cross_repo_candidate`/unresolved edges that Phase 5 cross-repo reads from `final-edges.parquet` (`src/resolve/cross_repo.rs:410-431`). Split: dedup→Phase 3, unresolved-filter stays at the sink boundary, persisted file keeps unresolved rows.
- **Byte-budget under-sizing.** A single `code` record can approach 32 KiB; if `write_buffer_bytes` is set below the largest single record the flush could stall. Guard: always admit at least one record per buffer even if it exceeds the byte budget.
- **`--write-only` against a stale/partial stage dir.** Re-running against a stage dir from a different schema/run could write inconsistent data. Guard: validate the stage dir's schema-magic and `cxg-symbols.db` presence; error clearly if absent. Idempotent MERGE/upsert means a re-run over a partially-written graph converges, but the input stage must match the run that produced it.
- **Streaming must not change written content.** Parity tests (`sink_parity`, `indradb_properties`, `neo4j_indexes`) gate this; the two-pass winner selection must reproduce the old phase2-wins + cross-shard-collapse semantics exactly.

### Acceptance criteria (Bug 2)
- AC-2.1: Phase 4 on grpc (160,580 functions + all nodes/edges) completes on the 15 GiB VM with peak `cxg-index` RSS bounded by `write_buffer_bytes × small_constant` — no whole-graph residency, no 11→16 GiB jump.
- AC-2.2: Peak Phase-4 RSS is ~flat as corpus grows (streaming invariant), proven by the doubling test.
- AC-2.3: Written node/edge content is byte-identical to the pre-change whole-graph write (parity tests green; dedup + phase2-wins preserved).
- AC-2.4: `--write-only` reproduces a full Phase-4 from an existing stage dir without re-running Phases 1–3; idempotent on re-run.
- AC-2.5: Both sinks no longer clone each chunk on the happy path (lazy-retry-clone).

---

## Cross-cutting concerns

- **Cross-platform:** Bug 1 fixes (`mallopt`, `malloc_trim`) are `cfg(target_os = "linux")`-gated in `src/mem/glibc.rs`; macOS gets no-op stubs and is untouched (the arena bug is glibc-specific). Index recycling (Bug 1d) and the entire Bug 2 streaming rework are platform-agnostic.
- **Two distinct Phase-4 OOMs:** this issue fixes **client RSS** (Bug 2). The wiki's Neo4j-JVM server-heap OOM on 200k-edge batches is separate; smaller streamed transactions reduce server-side peak too, but tuning Neo4j heap / `dbms.memory.*` is out of scope here.
- **Config knobs added** (all CLI > env > file > default via existing `src/config/mod.rs` precedence helpers): `[index].malloc_arena_max` (default 2), `[index].trim_interval` (64), `[index].index_recycle_interval` (256), `[index].write_only` (false), `[sink].write_buffer_bytes` (~64 MiB). `--malloc-arena-max 0` and the un-bounded buffer restore prior behavior for A/B.
- **Allocator dependency:** promote `libc` from dev-dependency (`Cargo.toml:69`) to a normal dependency.
- **`jemalloc` note:** a `tikv-jemallocator` global allocator is explicitly **not** the Bug-1 fix; it would only touch Rust-side buffers. `LD_PRELOAD=libjemalloc.so` is documented as a heavier op-side alternative for libclang fragmentation but is not the default code change.

## Rollout / validation plan (re-run grpc to prove the fix)

1. Build on the 15 GiB Linux VM; pre-flight `free -m && nproc && df -h` (per project build-constraint rule — ensure ≥4 GiB free / ≥10 GiB disk or use a remote builder).
2. **Phase 1 proof:** run `cxg-index <grpc> --stage-dir /data/grpc-stage` with **no `MALLOC_ARENA_MAX` env var**; watch RSS (via `getrusage` sampling / the process's own peak log). Expect plateau ≤ ~6 GiB through Phase 1, no monotonic climb. A/B with `--malloc-arena-max 0` to reproduce the old climb.
3. **Phase 4 proof:** with Phases 1–3 staged, run `cxg-index --write-only --stage-dir /data/grpc-stage --backend indradb`. Expect peak RSS bounded by the buffer budget (single-digit GiB), no 11→16 jump, IndraDB populated (160,580 functions + edges).
4. **Resume proof:** `kill -9` mid-Phase-4, re-run `--write-only`; assert the graph converges (idempotent) and Phases 1–3 are not re-run.
5. **Regression:** full `cargo test` (incl. `sink_parity`, `indradb_properties`, `neo4j_indexes`) green on Linux and macOS.

## Deferred

- LD_PRELOAD jemalloc launch wrapper for libclang fragmentation (op-side; only if `mallopt`+`malloc_trim` prove insufficient at larger scale).
- Neo4j JVM server-side OOM tuning (separate surface; the wiki's 200k-edge incident).
- Code-snippet off-loading (the `code` ≤32 KiB hog) — tracked under `[[pages/planning/cpp-indexer-compact-ingest-path]]`; reduces node payload further but is a schema/ingest change beyond these two bugs.
- A generic disk-backed Phase-3 `final-nodes.parquet` consolidation (cleaner than the two-pass but wider scope) — noted as an alternative to the bounded two-pass; not chosen to keep scope tight.

---

## Where the code contradicted the hypotheses

- **Bug 1 — jemalloc would NOT have fixed it.** The brief lists a global-allocator swap as option (a). libclang allocates through glibc (not Rust's `GlobalAlloc`), so jemalloc-as-global-allocator leaves the Phase-1 AST heap on glibc. The operator's own `MALLOC_ARENA_MAX=2` result is the proof, and confirms the fix must be glibc-side (`mallopt`/`malloc_trim`), Linux-gated.
- **Bug 2 — the dedup HashMap is load-bearing, not incidental.** The hypothesis ("stream batch-by-batch") is right in spirit but a *naive* stream would (1) write-amplify millions of cross-shard duplicate USRs instead of ~160k, and (2) risk IndraDB's skip-None upsert ordering. Verified mitigant: Phase 2 writes **full** records (`src/visit/decorate.rs:114-120`), so content isn't lost — but dedup must be **preserved** via a bounded two-pass, not dropped.
- **Bug 2 — the real RSS multiplier is in the sink, not the loader.** Beyond the whole-graph `Vec`, the IndraDB impl's `batch.len() * 17` `BulkInsertItem` expansion (`src/sink/indradb.rs:355`) plus per-chunk `.to_vec()`+`.clone()` (l.496/503) is the dominant peak. A MockSink memory test would miss it — the bounded-RSS integration test must hit the real sink impl.
- **Confirmed as stated:** the per-thread `Index` reuse (`src/visit/shallow.rs:90-98`), the on-disk staged Parquet (so Phase 4 *can* stream), and the absence of any resume/`--write-only` path (only `--skip-phase2` exists, `src/bin/index.rs:112-113`).
