//! Real end-to-end Phase-1 speed + memory measurement.
//!
//! Drives the REAL libclang pipeline (`run_phase1_parallel`) over an arbitrary
//! `compile_commands.json` and reports wall-clock time, TU throughput, and peak
//! process RSS (via `getrusage` — NOT `/usr/bin/time -l`, which strips
//! `DYLD_LIBRARY_PATH` on macOS and so cannot find libclang).
//!
//! Usage:
//!   cargo run --release --example measure_phase1 -- <compile_commands.json> \
//!       [workers] [alloc|noalloc] [skip_system:true|false]
//!
//! Requires LIBCLANG_PATH / DYLD_LIBRARY_PATH set (see AGENTS.md).

use std::path::PathBuf;
use std::sync::Arc;
use std::time::Instant;

use cpp_indexer::{
    bootstrap::compile_commands, pipeline::parallel::run_phase1_parallel,
    resolve::symbol_map::SymbolAllocator,
};

fn main() {
    let mut args = std::env::args().skip(1);
    let cc_path = PathBuf::from(args.next().expect("arg1: path to compile_commands.json"));
    let workers: Option<usize> = args.next().and_then(|s| s.parse().ok());
    let use_alloc = matches!(args.next().as_deref(), Some("alloc"));
    let skip_system = !matches!(args.next().as_deref(), Some("false") | Some("0"));

    let work = std::env::temp_dir().join(format!("cxg-phase1-{}", std::process::id()));
    let stage = work.join("stage");
    let db_path = work.join("cxg-symbols.db");
    let _ = std::fs::remove_dir_all(&work);
    std::fs::create_dir_all(&stage).expect("create stage dir");

    let entries = compile_commands::parse(&cc_path).expect("parse compile_commands.json");
    assert!(!entries.is_empty(), "compile_commands must have >=1 TU");

    let alloc = if use_alloc {
        Some(Arc::new(
            SymbolAllocator::open(&db_path, 10_000).expect("open allocator"),
        ))
    } else {
        None
    };

    let t0 = Instant::now();
    let stats = run_phase1_parallel(
        &entries,
        &stage,
        "bench-repo",
        skip_system,
        workers,
        alloc,
        Default::default(),
    )
    .expect("phase 1 must succeed");
    let elapsed = t0.elapsed();

    let total = stats.tu_ok + stats.tu_partial + stats.tu_error;
    let secs = elapsed.as_secs_f64();
    let peak_rss = max_rss_bytes();

    println!("\n════════════ Phase-1 real speed + memory ════════════");
    println!("compile_commands   : {}", cc_path.display());
    println!(
        "workers            : {}    symbol-id allocator: {}    skip_system_headers: {}",
        workers.map_or_else(
            || format!("{} (num_cpus)", num_cpus_get()),
            |w| w.to_string()
        ),
        if use_alloc {
            "ON (v6)"
        } else {
            "OFF (baseline)"
        },
        skip_system,
    );
    println!("──────────────────────────────────────────────────────");
    println!(
        "TUs                : {total} total  ({} ok / {} partial / {} error)",
        stats.tu_ok, stats.tu_partial, stats.tu_error
    );
    println!("wall-clock         : {secs:.3} s");
    println!(
        "throughput         : {:.1} TU/s",
        total as f64 / secs.max(1e-9)
    );
    println!(
        "mean latency / TU  : {:.1} ms",
        1000.0 * secs / total.max(1) as f64
    );
    println!(
        "peak RSS (getrusage): {} bytes  ({:.1} MiB)",
        peak_rss,
        peak_rss as f64 / (1024.0 * 1024.0)
    );
    println!("══════════════════════════════════════════════════════\n");

    let _ = std::fs::remove_dir_all(&work);
}

/// Peak RSS of this process in bytes. macOS `ru_maxrss` is bytes; Linux is KiB.
fn max_rss_bytes() -> u64 {
    let mut ru: libc::rusage = unsafe { std::mem::zeroed() };
    if unsafe { libc::getrusage(libc::RUSAGE_SELF, &mut ru) } != 0 {
        return 0;
    }
    let raw = ru.ru_maxrss as u64;
    if cfg!(target_os = "macos") {
        raw
    } else {
        raw * 1024
    }
}

fn num_cpus_get() -> usize {
    std::thread::available_parallelism().map_or(1, std::num::NonZeroUsize::get)
}
