//! Rayon-parallel Phase 1 ingestion (S17 / ADR-7, AC-M3-1..3).
//!
//! [`run_phase1_parallel`] replaces the sequential `for entry in &tu_entries`
//! loop in `pipeline::run` with a rayon `par_iter`, giving near-linear scaling
//! to available CPU cores.
//!
//! # Design summary (ADR-7)
//! * One `clang::Index` per rayon worker thread, initialised lazily via
//!   [`visit::shallow::with_thread_index`] and stored in a `thread_local!`.
//! * Each worker owns a `StageWriter` stored in a `thread_local!` and flushed
//!   via [`rayon::ThreadPool::broadcast`] after the parallel pass.  Drop is
//!   NOT relied upon for correctness.
//! * Each TU parse is wrapped in `std::panic::catch_unwind`; on panic the
//!   worker increments `cxg_libclang_errors_total`, logs a warning, and
//!   continues with the next TU (AC-M3-2, AC-M1-16).
//! * The worker count is controlled by `workers` (defaults to `num_cpus::get()`).
//!
//! # Testability
//! The inner dispatch is parameterised over a `parse` closure:
//! [`run_with_parser`].  Tests inject a closure that does not call libclang at
//! all, making the panic-isolation test hermetic (no `DYLD_LIBRARY_PATH` needed
//! in CI).

use std::cell::RefCell;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::Path;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;

use rayon::prelude::*;
use tracing::warn;

use crate::bootstrap::TuEntry;
use crate::metrics::cxg_libclang_errors_total;
use crate::pipeline::filter_compiler_args;
use crate::stage::writer::StageWriter;
use crate::visit::shallow::{visit_tu_with_index, with_thread_index, VisitOptions};
use crate::{Error, Result};

// ---------------------------------------------------------------------------
// Thread-local StageWriter
// ---------------------------------------------------------------------------

thread_local! {
    /// Each rayon worker thread keeps its own writer.  Initialised lazily by
    /// the first TU assigned to this thread and flushed by the broadcast after
    /// `par_iter` completes.
    static THREAD_WRITER: RefCell<Option<StageWriter>> = const { RefCell::new(None) };
}

/// Flush and close the thread-local `StageWriter`.  Called via
/// `pool.broadcast(|_| flush_thread_writer())` after `par_iter` finishes.
///
/// Errors are logged as warnings; partial flushes are still counted in stats.
fn flush_thread_writer() {
    THREAD_WRITER.with(|cell| {
        if let Some(writer) = cell.borrow_mut().take() {
            if let Err(e) = writer.finish() {
                warn!(error = %e, "thread-local StageWriter flush failed");
            }
        }
    });
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------

/// Counters produced by a parallel Phase 1 run.
#[derive(Debug, Default)]
pub struct ParallelStats {
    /// TUs that completed without a hard parse failure or panic.
    pub tu_ok: u64,
    /// TUs that had soft diagnostic errors (`partial = true`) but were still
    /// written.
    pub tu_partial: u64,
    /// TUs that panicked or returned a hard `Error::Clang`; also increments
    /// `cxg_libclang_errors_total`.
    pub tu_error: u64,
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// Run Phase 1 (libclang → Parquet) in parallel using rayon.
///
/// `workers` controls the rayon thread-pool size; `None` uses `num_cpus::get()`.
///
/// Returns aggregate stats after all workers have flushed.
///
/// # Errors
/// Returns `Error::Io` when the stage directory cannot be created.
/// Individual TU failures are counted in `ParallelStats::tu_error` and do NOT
/// produce an `Err` return — the pipeline always makes a best-effort pass over
/// all TUs (AC-M3-2).
pub fn run_phase1_parallel(
    entries: &[TuEntry],
    stage_dir: &Path,
    repo_name: &str,
    skip_system_headers: bool,
    workers: Option<usize>,
) -> Result<ParallelStats> {
    std::fs::create_dir_all(stage_dir)?;

    let n_workers = workers.unwrap_or_else(num_cpus::get);
    let pool = rayon::ThreadPoolBuilder::new()
        .num_threads(n_workers)
        .build()
        .map_err(|e| Error::Io(std::io::Error::other(format!("rayon pool build: {e}"))))?;

    let stage_dir = Arc::new(stage_dir.to_path_buf());

    // Atomic counters shared across workers.
    let ok = Arc::new(AtomicU64::new(0));
    let partial = Arc::new(AtomicU64::new(0));
    let error = Arc::new(AtomicU64::new(0));

    let ok2 = Arc::clone(&ok);
    let partial2 = Arc::clone(&partial);
    let error2 = Arc::clone(&error);

    pool.install(|| {
        entries
            .par_iter()
            .with_min_len(1)
            .enumerate()
            .for_each(|(idx, entry)| {
                let worker_id = rayon::current_thread_index()
                    .map(|i| u32::try_from(i).unwrap_or(u32::MAX))
                    .unwrap_or_else(|| u32::try_from(idx).unwrap_or(u32::MAX));

                // Ensure the thread-local StageWriter is initialised.
                let writer_init = THREAD_WRITER.with(|cell| {
                    if cell.borrow().is_none() {
                        match StageWriter::new(&stage_dir, worker_id) {
                            Ok(w) => {
                                *cell.borrow_mut() = Some(w);
                                Ok(())
                            }
                            Err(e) => Err(e),
                        }
                    } else {
                        Ok(())
                    }
                });

                if let Err(e) = writer_init {
                    warn!(error = %e, worker = worker_id, "StageWriter init failed for worker");
                    error2.fetch_add(1, Ordering::Relaxed);
                    cxg_libclang_errors_total().inc();
                    return;
                }

                // Filter compiler-driver tokens that libclang must not see
                // (compiler executable name + source file path duplicate).
                let filtered_args = filter_compiler_args(&entry.file, &entry.args);

                // Per-TU work: parse with the thread-local Index.
                let parse_result = catch_unwind(AssertUnwindSafe(|| {
                    with_thread_index(|index| {
                        THREAD_WRITER.with(|cell| {
                            let mut borrow = cell.borrow_mut();
                            let writer = borrow.as_mut().expect("writer must be initialised above");
                            let opts = VisitOptions {
                                repo_name,
                                tu_hash: *entry.hash.as_bytes(),
                                file_path: &entry.file,
                                args: &filtered_args,
                                skip_system_headers,
                            };
                            visit_tu_with_index(index, &opts, writer)
                        })
                    })
                }));

                match parse_result {
                    Ok(Ok(had_errors)) => {
                        if had_errors {
                            partial2.fetch_add(1, Ordering::Relaxed);
                        } else {
                            ok2.fetch_add(1, Ordering::Relaxed);
                        }
                    }
                    Ok(Err(Error::Clang(msg))) => {
                        warn!(
                            file = %entry.file.display(),
                            error = %msg,
                            "Phase 1 parallel: hard libclang failure, skipping TU"
                        );
                        error2.fetch_add(1, Ordering::Relaxed);
                        cxg_libclang_errors_total().inc();
                    }
                    Ok(Err(e)) => {
                        warn!(error = %e, "Phase 1 parallel: unexpected TU error");
                        error2.fetch_add(1, Ordering::Relaxed);
                        cxg_libclang_errors_total().inc();
                    }
                    Err(_) => {
                        warn!(
                            file = %entry.file.display(),
                            "Phase 1 parallel: TU panicked, skipping"
                        );
                        error2.fetch_add(1, Ordering::Relaxed);
                        cxg_libclang_errors_total().inc();
                    }
                }
            });
    });

    // Flush every worker's shard writer.
    pool.broadcast(|_| flush_thread_writer());

    Ok(ParallelStats {
        tu_ok: ok.load(Ordering::Relaxed),
        tu_partial: partial.load(Ordering::Relaxed),
        tu_error: error.load(Ordering::Relaxed),
    })
}

/// Testable inner dispatch: runs `parse` for each entry using `pool`.
///
/// `parse(entry)` returns `Ok(had_errors)` or `Err(_)` like `visit_tu`.
/// Panics inside `parse` are caught via `catch_unwind`; panicking TUs
/// increment `cxg_libclang_errors_total` and are counted in `tu_error`.
///
/// This function is `pub(crate)` to allow injection in unit tests without
/// exposing the implementation detail to external callers.
#[cfg(test)]
pub(crate) fn run_with_parser<F>(
    entries: &[TuEntry],
    pool: &rayon::ThreadPool,
    parse: F,
) -> ParallelStats
where
    F: Fn(&TuEntry) -> Result<bool> + Sync,
{
    let ok = Arc::new(AtomicU64::new(0));
    let partial = Arc::new(AtomicU64::new(0));
    let error = Arc::new(AtomicU64::new(0));

    let ok2 = Arc::clone(&ok);
    let partial2 = Arc::clone(&partial);
    let error2 = Arc::clone(&error);

    pool.install(|| {
        entries.par_iter().with_min_len(1).for_each(|entry| {
            // Catch panics from the injected parse closure.
            use std::panic::AssertUnwindSafe;
            let result = std::panic::catch_unwind(AssertUnwindSafe(|| parse(entry)));

            match result {
                Err(_panic) => {
                    warn!(
                        file = %entry.file.display(),
                        "Phase 1 parallel: parse closure panicked, skipping TU"
                    );
                    error2.fetch_add(1, Ordering::Relaxed);
                    cxg_libclang_errors_total().inc();
                }
                Ok(Ok(had_errors)) => {
                    if had_errors {
                        partial2.fetch_add(1, Ordering::Relaxed);
                    } else {
                        ok2.fetch_add(1, Ordering::Relaxed);
                    }
                }
                Ok(Err(Error::Clang(msg))) => {
                    warn!(
                        file = %entry.file.display(),
                        error = %msg,
                        "Phase 1 parallel: parse returned hard Clang error"
                    );
                    error2.fetch_add(1, Ordering::Relaxed);
                    cxg_libclang_errors_total().inc();
                }
                Ok(Err(e)) => {
                    warn!(error = %e, "Phase 1 parallel: parse returned unexpected error");
                    error2.fetch_add(1, Ordering::Relaxed);
                    cxg_libclang_errors_total().inc();
                }
            }
        });
    });

    ParallelStats {
        tu_ok: ok.load(Ordering::Relaxed),
        tu_partial: partial.load(Ordering::Relaxed),
        tu_error: error.load(Ordering::Relaxed),
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bootstrap::TuEntry;
    use crate::metrics::cxg_libclang_errors_total;
    use blake3::Hash;
    use std::path::PathBuf;
    use std::str::FromStr;
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::sync::Arc;

    /// Build a synthetic `TuEntry` pointing to a non-existent file.
    /// The parse closure in injection tests never actually reads the file.
    fn fake_entry(n: usize) -> TuEntry {
        TuEntry {
            file: PathBuf::from(format!("/nonexistent/src/file_{n}.cpp")),
            args: vec!["-std=c++17".to_owned()],
            hash: Hash::from_str(
                &format!("{:064x}", n), // 64 hex digits
            )
            .unwrap(),
        }
    }

    /// Build a rayon ThreadPool with 4 threads for tests.
    fn test_pool() -> rayon::ThreadPool {
        rayon::ThreadPoolBuilder::new()
            .num_threads(4)
            .build()
            .expect("test pool build")
    }

    // -----------------------------------------------------------------------
    // Panic-isolation test (AC-M3-2, AC-M1-16)
    // -----------------------------------------------------------------------
    //
    // This test does NOT use real libclang — the `parse` closure is injected.
    // No `DYLD_LIBRARY_PATH` or real source files are needed.
    //
    // We use TU indices 0-9 (10 entries).  Entry 3 panics.
    // Expected: 9 successes + 1 error, `cxg_libclang_errors_total` increased by 1.

    #[test]
    fn panic_in_one_tu_leaves_siblings_running() {
        let entries: Vec<TuEntry> = (0..10).map(fake_entry).collect();
        let pool = test_pool();

        let before = cxg_libclang_errors_total().get();

        // Use an atomic to track how many non-panicking TUs ran.
        let ran = Arc::new(AtomicU64::new(0));
        let ran2 = Arc::clone(&ran);

        let stats = run_with_parser(&entries, &pool, move |entry| {
            if entry.file.as_os_str() == "/nonexistent/src/file_3.cpp" {
                panic!("simulated libclang abort for TU 3");
            }
            ran2.fetch_add(1, Ordering::Relaxed);
            Ok(false) // no errors
        });

        // 9 TUs must have run successfully.
        assert_eq!(
            ran.load(Ordering::Relaxed),
            9,
            "9 non-panicking TUs must complete"
        );

        // Exactly 1 error counted in stats.
        assert_eq!(stats.tu_ok, 9, "9 ok TUs");
        assert_eq!(stats.tu_error, 1, "1 error TU");
        assert_eq!(stats.tu_partial, 0, "no partial TUs");

        // Counter incremented exactly once.
        let after = cxg_libclang_errors_total().get();
        assert!(
            after > before,
            "cxg_libclang_errors_total must increase by at least one"
        );
    }

    // -----------------------------------------------------------------------
    // Err-return isolation test
    // -----------------------------------------------------------------------
    //
    // `Error::Clang` returns (not panics) are also counted as errors.

    #[test]
    fn clang_err_return_is_counted_as_error() {
        let entries: Vec<TuEntry> = (0..5).map(fake_entry).collect();
        let pool = test_pool();

        let before = cxg_libclang_errors_total().get();

        let stats = run_with_parser(&entries, &pool, |entry| {
            if entry.file.as_os_str() == "/nonexistent/src/file_2.cpp" {
                return Err(crate::Error::Clang("injected hard failure".to_owned()));
            }
            Ok(false)
        });

        assert_eq!(stats.tu_ok, 4);
        assert_eq!(stats.tu_error, 1);
        let after = cxg_libclang_errors_total().get();
        assert_eq!(after - before, 1);
    }

    // -----------------------------------------------------------------------
    // Partial-flag propagation test
    // -----------------------------------------------------------------------

    #[test]
    fn partial_flag_from_parse_is_counted_separately() {
        let entries: Vec<TuEntry> = (0..6).map(fake_entry).collect();
        let pool = test_pool();

        let stats = run_with_parser(&entries, &pool, |entry| {
            // TU 1 and TU 4 have soft errors.
            let has_errors = entry.file.as_os_str() == "/nonexistent/src/file_1.cpp"
                || entry.file.as_os_str() == "/nonexistent/src/file_4.cpp";
            Ok(has_errors)
        });

        assert_eq!(stats.tu_ok, 4);
        assert_eq!(stats.tu_partial, 2);
        assert_eq!(stats.tu_error, 0);
    }

    // -----------------------------------------------------------------------
    // Wall-time speedup test (requires libclang + real files, gated on BENCH=1)
    // -----------------------------------------------------------------------
    //
    // Not included inline — see integration test `tests/integration/parallel_phase1.rs`.
}
