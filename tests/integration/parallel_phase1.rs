//! Integration tests for S17: Rayon-parallel Phase 1 (AC-M3-1, AC-M3-2, AC-M3-3).
//!
//! ## Running
//! ```
//! DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib \
//!   cargo nextest run -p cpp_indexer pipeline::parallel
//! ```
//!
//! The wall-time speedup test is gated on `BENCH=1` because it requires ≥2 cores
//! and real source files; it is skipped in normal CI to avoid flakiness.

use std::path::PathBuf;
use std::time::Instant;

use cpp_indexer::{
    bootstrap::{compile_commands, TuEntry},
    pipeline::parallel::run_phase1_parallel,
};
use tempfile::TempDir;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

fn m1_fixture_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/m1_5file")
}

fn m1_cc_path() -> PathBuf {
    m1_fixture_dir().join("compile_commands.json")
}

/// Load TU entries from the m1_5file fixture.
fn load_m1_entries() -> Vec<TuEntry> {
    compile_commands::parse(&m1_cc_path()).expect("parse m1 compile_commands.json")
}

// ---------------------------------------------------------------------------
// Test: parallel run on m1 fixture produces Parquet shards (AC-M3-1)
// ---------------------------------------------------------------------------

#[test]
fn parallel_phase1_produces_parquet_shards() {
    let tmp = TempDir::new().unwrap();
    let stage_dir = tmp.path().join("stage");

    let entries = load_m1_entries();
    assert!(!entries.is_empty(), "fixture must have at least one TU");

    let stats = run_phase1_parallel(
        &entries,
        &stage_dir,
        "test-repo",
        /*skip_system_headers=*/ true,
        /*workers=*/ Some(2),
        /*allocator=*/ None,
        Default::default(),
    )
    .expect("run_phase1_parallel must not return Err");

    // Every TU in the m1 fixture is valid C++ — expect no errors.
    assert_eq!(
        stats.tu_error, 0,
        "no TU errors expected on valid m1 fixture"
    );
    assert_eq!(
        stats.tu_ok + stats.tu_partial,
        u64::try_from(entries.len()).unwrap(),
        "all TUs must be accounted for"
    );

    // At least one Parquet shard must have been written.
    let shard_count = glob::glob(
        stage_dir
            .join("worker-*")
            .join("nodes-*.parquet")
            .to_str()
            .unwrap(),
    )
    .unwrap()
    .filter_map(|r| r.ok())
    .count();
    assert!(
        shard_count >= 1,
        "at least one nodes shard must be present; got {shard_count}"
    );
}

// ---------------------------------------------------------------------------
// Test: workers=1 still works (sequential-via-rayon)
// ---------------------------------------------------------------------------

#[test]
fn parallel_phase1_with_one_worker_completes() {
    let tmp = TempDir::new().unwrap();
    let stage_dir = tmp.path().join("stage");

    let entries = load_m1_entries();
    let stats = run_phase1_parallel(
        &entries,
        &stage_dir,
        "test-repo",
        true,
        Some(1),
        None,
        Default::default(),
    )
    .expect("single-worker run must succeed");

    assert_eq!(stats.tu_error, 0);
    assert_eq!(
        stats.tu_ok + stats.tu_partial,
        u64::try_from(entries.len()).unwrap()
    );
}

// ---------------------------------------------------------------------------
// Test: wall-time speedup (gated on BENCH=1, requires ≥2 cores and libclang)
// ---------------------------------------------------------------------------
//
// This test is excluded from normal `cargo nextest run` to avoid CI flakiness.
// Run it explicitly:
//
//   BENCH=1 DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib \
//     cargo nextest run -p cpp_indexer --test parallel_phase1 speedup

#[test]
fn speedup_parallel_vs_sequential() {
    if std::env::var("BENCH").is_err() {
        eprintln!("speedup test skipped — set BENCH=1 to run");
        return;
    }

    let n_cpus = num_cpus::get();
    if n_cpus < 2 {
        eprintln!("speedup test skipped — only 1 CPU detected");
        return;
    }

    // Build a synthetic 50-TU fixture pointing at real source files.
    // We reuse the m1 fixture entries duplicated to ~50 TUs.
    let base_entries = load_m1_entries();
    let repeat = (50 / base_entries.len()) + 1;
    let entries: Vec<TuEntry> = base_entries
        .iter()
        .cloned()
        .cycle()
        .take(base_entries.len() * repeat)
        .collect();
    let entries = &entries[..50.min(entries.len())];

    let tmp_seq = TempDir::new().unwrap();
    let tmp_par = TempDir::new().unwrap();

    // Sequential: 1 worker.
    let t0 = Instant::now();
    run_phase1_parallel(
        entries,
        tmp_seq.path(),
        "bench-repo",
        true,
        Some(1),
        None,
        Default::default(),
    )
    .expect("sequential run");
    let sequential_dur = t0.elapsed();

    // Parallel: all CPUs.
    let t1 = Instant::now();
    run_phase1_parallel(
        entries,
        tmp_par.path(),
        "bench-repo",
        true,
        None,
        None,
        Default::default(),
    )
    .expect("parallel run");
    let parallel_dur = t1.elapsed();

    eprintln!(
        "speedup test: sequential={:.2?} parallel={:.2?} ratio={:.2}x",
        sequential_dur,
        parallel_dur,
        sequential_dur.as_secs_f64() / parallel_dur.as_secs_f64()
    );

    // Assert a loose speedup: parallel must be faster than sequential × 0.8.
    // (0.8 leaves headroom for scheduling overhead on a loaded CI box.)
    assert!(
        parallel_dur < sequential_dur.mul_f64(0.8),
        "parallel ({parallel_dur:.2?}) must be < sequential ({sequential_dur:.2?}) × 0.8"
    );
}
