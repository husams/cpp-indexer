# ADR-7: Parallel ingestion model — rayon + thread-local clang::Index

Status: accepted
Date: 2026-05-17
Resolves: M3-S1 parallel Phase 1; AC-M3-1..3

## Context

Phase 1 must scale Phase 1 to all available CPUs (AC-M3-1, AC-M3-3: LLVM in ≤15 min on 32 cores). Constraints:

- `clang::Index` (from the `clang` crate, wrapping libclang) is **not thread-safe across `Index` instances** in the same process — but a single `Index` may serve only one thread at a time. Practical pattern: one `Index` per worker thread.
- libclang occasionally segfaults on malformed TUs; one bad TU must not take down sibling workers (AC-M3-2, AC-M1-16).
- Workers must write Parquet shards without locking each other (see ADR-3).
- Cache hit checks must be O(1) per TU and not contend.

## Decision

Use `rayon` for CPU-bound parallelism with a custom thread pool sized by `[index].workers` (default `num_cpus::get()`).

### Worker-local libclang Index

- One `thread_local!` static per worker:

```rust
thread_local! {
    static CLANG: RefCell<Option<clang::Clang>> = const { RefCell::new(None) };
    static INDEX: RefCell<Option<&'static clang::Index<'static>>> = const { RefCell::new(None) };
}

fn with_thread_index<R>(f: impl FnOnce(&clang::Index) -> R) -> R {
    INDEX.with(|cell| {
        if cell.borrow().is_none() {
            CLANG.with(|c| {
                if c.borrow().is_none() {
                    *c.borrow_mut() = Some(clang::Clang::new().expect("libclang init"));
                }
            });
            // SAFETY: Clang/Index live for the worker's lifetime; rayon workers
            // outlive any single TU parse. We leak to 'static to satisfy the
            // clang crate's lifetime parameter, which is a documented pattern.
            let clang_ref: &'static clang::Clang = Box::leak(Box::new(
                CLANG.with(|c| c.borrow().as_ref().unwrap().clone())
            ));
            let idx: clang::Index<'static> =
                clang::Index::new(clang_ref, /*exclude_decls_from_pch=*/false, /*display_diagnostics=*/false);
            *cell.borrow_mut() = Some(Box::leak(Box::new(idx)));
        }
        f(cell.borrow().as_ref().unwrap())
    })
}
```

- Workers are long-lived (rayon's global pool). The leak is bounded by the worker count and the process lifetime; this is the standard pattern for tying a `'static`-bound C handle to a rayon worker.

### Per-TU isolation

- Each TU parse runs inside `std::panic::catch_unwind(AssertUnwindSafe(|| ...))`. On panic, increment `cxg_libclang_errors_total` and emit a diagnostic node (`partial: true`) for the TU. AC-M3-2, AC-M1-16.
- `signal` crate not used; libclang's own signal handlers are left in place. A SIGSEGV that escapes `catch_unwind` will still abort the process, but in practice libclang segfaults raise C++ aborts that `catch_unwind` does catch when libclang is built with exception support (true for upstream Debian builds).

### Work distribution

- `rayon::par_iter` over `Vec<TuEntry>` with `with_min_len(1)` so each TU is one work item (TUs are coarse enough that finer chunking is unnecessary).
- Cache hit check runs **before** `par_iter` dispatches to a worker, using a serial pre-pass over `manifest.json` that filters to changed TUs. Cache hits do not touch libclang at all (AC-M3-7).

### Shard writers

- Each worker owns a `stage::writer::ShardWriter` keyed by `thread_id`. No cross-worker coordination. Writers flush on TU completion and rotate at 256 MiB (ADR-3).

### Memory budget

- Phase 1 peak RSS target ≤16 GB (AC-M3-11). Workers default to `num_cpus`; on memory-constrained hosts the operator lowers `[index].workers`. Documented in runbook.
- USR map spill threshold (Phase 3) is set in ADR-8 at 8 GB (AC-M3-12).

### Progress reporting

- A separate `tokio::task` (in cxg-daemon) or a dedicated thread (in cxg-index) ticks every 5 s, reading atomic counters maintained by workers (`AtomicU64` for `tus_done`, `nodes_written`, `edges_written`). Stderr line includes nodes/sec, edges/sec, TUs done/total (AC-M3-13).

## Alternatives considered

- **tokio for Phase 1**: rejected. Parsing a TU is fully CPU-bound and synchronous; tokio adds task-switching overhead with no I/O concurrency benefit during the parse itself. tokio is used only in cxg-daemon (axum + sink async clients).
- **One global `clang::Index` behind a Mutex**: rejected. Serialises Phase 1 to a single thread; defeats parallelism.
- **Process-per-TU (matches Python CodexGraph)**: rejected. Heavy fork cost, no shared memory for the USR table, and the original plan calls it out as the bottleneck we are eliminating.
- **Crossbeam scoped threads instead of rayon**: rejected. Equivalent capability with more boilerplate; rayon's `par_iter` + work-stealing is well-suited to the uneven TU duration distribution.
- **`std::thread::scope` from std**: viable, but rayon's pool reuses workers across phases; std `scope` would force re-creation each phase.

## Consequences

Positive:
- Linear scaling to ≥32 cores demonstrated by the rayon work-stealing model.
- No cross-worker locks on the hot path; Parquet writes are independent.
- Bad TUs do not poison neighbours.

Negative:
- `Box::leak` for the `clang::Index<'static>` is unidiomatic; documented in module docstring with a `// SAFETY:` comment.
- TSAN cannot reason about libclang's internals; concurrency bugs in libclang itself are an external risk (pin version 18, log diagnostics).

Follow-ups:
- After M3 benchmark, profile worker idle time; if work-stealing imbalance > 20 %, switch to explicit chunked dispatch.
- Investigate `clang-sys` thread-safety flags in case future versions allow shared `Index` across threads.

Revisit if: TU parse time variance becomes very high (some TUs > 60 s) — at that point consider preempting with a per-TU timeout.

## References

- requirements.md AC-M3-1..3, AC-M1-16
- engineering plan v1.1 §Phase 1, §Risk register (clang::Index Send/Sync confusion)
- Cognee tags: `task:cpp-indexer role:architect`
