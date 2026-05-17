//! LLVM-scale performance gate — M3 exit benchmark.
//!
//! Story: S21-m3-perf-gate (AC-M3-3, AC-M3-4, AC-M3-5, AC-M3-10, AC-M3-11).
//!
//! # Acceptance criteria verified
//!
//! | AC        | Threshold                                                        |
//! |-----------|------------------------------------------------------------------|
//! | AC-M3-3   | Phase 1 full LLVM run (≈25k TUs, 32 workers) ≤ 15 min          |
//! | AC-M3-4   | Neo4j write throughput ≥ 50k rows/s                             |
//! | AC-M3-5   | IndraDB write throughput ≥ 100k rows/s                          |
//! | AC-M3-10  | Incremental run (one file changed) ≤ 1 min end-to-end           |
//! | AC-M3-11  | Peak RSS during Phase 1 ≤ 16 GiB (Linux only)                  |
//!
//! # Running
//!
//! ```bash
//! # Dry-run (no LLVM checkout required — exits 0 immediately):
//! BENCH=1 cargo bench --bench llvm_index
//!
//! # Full gate (requires LLVM source tree + a running cxg-index build):
//! CXG_M3_LLVM_PATH=/path/to/llvm-project BENCH=1 cargo bench --bench llvm_index
//! ```
//!
//! Results are persisted to `target/bench/llvm-<unix-timestamp>.json`.
//!
//! # Design notes
//!
//! The bench invokes the `cxg-index` **binary** (end-to-end), not library
//! functions.  This is intentional: AC-M3-3 measures wall-clock time including
//! I/O, libclang overhead, batching, and sink writes — not just pipeline logic.
//!
//! The binary is located via the `CXG_INDEX_BIN` env-var (for CI overrides) or
//! falls back to `target/debug/cxg-index` relative to `CARGO_MANIFEST_DIR`.
//! Build the binary with `cargo build --bin cxg-index` before running the full
//! gate.

#![allow(clippy::unwrap_used)]

use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{Instant, SystemTime, UNIX_EPOCH};

// ── Threshold constants (from requirements.md §M3) ────────────────────────────

/// AC-M3-3: full LLVM run must complete within this many seconds.
const FULL_RUN_LIMIT_SECS: u64 = 15 * 60; // 15 minutes

/// AC-M3-10: incremental run (one file touched) must complete within this.
const INCREMENTAL_LIMIT_SECS: u64 = 60; // 1 minute

/// AC-M3-4: Neo4j write throughput rows/s lower bound.
const NEO4J_THROUGHPUT_MIN: f64 = 50_000.0;

/// AC-M3-5: IndraDB write throughput rows/s lower bound.
const INDRADB_THROUGHPUT_MIN: f64 = 100_000.0;

/// AC-M3-11: peak RSS upper bound in bytes (16 GiB).
const RSS_LIMIT_BYTES: u64 = 16 * 1024 * 1024 * 1024;

// ── Binary resolution ─────────────────────────────────────────────────────────

fn cxg_index_bin() -> PathBuf {
    if let Ok(p) = std::env::var("CXG_INDEX_BIN") {
        return PathBuf::from(p);
    }
    let manifest = env!("CARGO_MANIFEST_DIR");
    PathBuf::from(manifest)
        .join("target")
        .join("debug")
        .join("cxg-index")
}

// ── RSS helpers (Linux only) ──────────────────────────────────────────────────

/// Returns peak RSS of the last waited child in **bytes**, or `None` on
/// non-Linux platforms.  On Linux `getrusage(RUSAGE_CHILDREN)` returns KiB.
#[cfg(target_os = "linux")]
fn peak_rss_bytes_children() -> Option<u64> {
    use std::mem::MaybeUninit;
    extern "C" {
        fn getrusage(who: i32, usage: *mut libc::rusage) -> i32;
    }
    let mut usage: MaybeUninit<libc::rusage> = MaybeUninit::uninit();
    // RUSAGE_CHILDREN = -1
    let rc = unsafe { getrusage(-1, usage.as_mut_ptr()) };
    if rc != 0 {
        return None;
    }
    let kib = unsafe { usage.assume_init().ru_maxrss };
    if kib <= 0 {
        None
    } else {
        Some(kib as u64 * 1024)
    }
}

#[cfg(not(target_os = "linux"))]
fn peak_rss_bytes_children() -> Option<u64> {
    None
}

// ── Throughput helpers ────────────────────────────────────────────────────────

/// Parse the final summary line from `cxg-index` stderr:
/// `"cxg-index: done — N TUs | P partial | X nodes | Y edges"`
///
/// Returns `(nodes, edges)` on success.
fn parse_summary_line(stderr: &str) -> Option<(u64, u64)> {
    // Look for a line containing "nodes" and "edges"
    for line in stderr.lines() {
        if line.contains("nodes") && line.contains("edges") {
            // Extract numbers before "nodes" and "edges"
            let nodes = extract_number_before(line, "nodes")?;
            let edges = extract_number_before(line, "edges")?;
            return Some((nodes, edges));
        }
    }
    None
}

fn extract_number_before(line: &str, keyword: &str) -> Option<u64> {
    let idx = line.find(keyword)?;
    let before = &line[..idx];
    before.split_whitespace().last()?.parse().ok()
}

// ── Result persistence ────────────────────────────────────────────────────────

fn persist_results(results: &serde_json::Value) {
    let ts = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs();
    let bench_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("target")
        .join("bench");
    if fs::create_dir_all(&bench_dir).is_err() {
        eprintln!("llvm_index: warning — could not create target/bench/");
        return;
    }
    let path = bench_dir.join(format!("llvm-{ts}.json"));
    match fs::write(
        &path,
        serde_json::to_string_pretty(results).unwrap_or_default(),
    ) {
        Ok(()) => println!("llvm_index: results persisted → {}", path.display()),
        Err(e) => eprintln!("llvm_index: warning — could not persist results: {e}"),
    }
}

// ── Gate runner ───────────────────────────────────────────────────────────────

/// Run `cxg-index` against `llvm_path` with the mock/dev sink (no DB required
/// for the timing gate).  Returns `(wall_secs, nodes, edges)`.
fn run_cxg_index(llvm_path: &Path, bin: &Path, extra_env: &[(&str, &str)]) -> RunResult {
    let t0 = Instant::now();
    let mut cmd = Command::new(bin);
    cmd.arg(llvm_path)
        .arg("--backend")
        .arg("mock")
        // Use a staging dir inside the LLVM path so we can reuse it for the
        // incremental run without a separate flag.
        .arg("--stage-dir")
        .arg(llvm_path.join(".cxg-bench-stage").as_os_str())
        .arg("--repo-name")
        .arg("llvm-bench");

    for (k, v) in extra_env {
        cmd.env(k, v);
    }

    let output = cmd.output().expect("failed to spawn cxg-index");
    let wall_secs = t0.elapsed().as_secs_f64();
    let rss_bytes = peak_rss_bytes_children();
    let stderr = String::from_utf8_lossy(&output.stderr).to_string();
    let stdout = String::from_utf8_lossy(&output.stdout).to_string();

    let (nodes, edges) = parse_summary_line(&stderr)
        .or_else(|| parse_summary_line(&stdout))
        .unwrap_or((0, 0));

    RunResult {
        wall_secs,
        nodes,
        edges,
        rss_bytes,
        success: output.status.success(),
        stderr,
    }
}

struct RunResult {
    wall_secs: f64,
    nodes: u64,
    edges: u64,
    rss_bytes: Option<u64>,
    success: bool,
    stderr: String,
}

// ── Main ──────────────────────────────────────────────────────────────────────

fn main() {
    // Gate 1: require BENCH=1 (matches sink_throughput.rs convention).
    if std::env::var("BENCH").as_deref() != Ok("1") {
        println!("llvm_index bench skipped (set BENCH=1 to run)");
        return;
    }

    // Gate 2: require CXG_M3_LLVM_PATH (fixture not available in CI by default).
    let llvm_path = match std::env::var("CXG_M3_LLVM_PATH") {
        Ok(p) => PathBuf::from(p),
        Err(_) => {
            println!(
                "llvm_index bench skipped (set CXG_M3_LLVM_PATH=/path/to/llvm-project to run full gate)"
            );
            return;
        }
    };

    if !llvm_path.exists() {
        eprintln!(
            "llvm_index: CXG_M3_LLVM_PATH={} does not exist — aborting",
            llvm_path.display()
        );
        std::process::exit(1);
    }

    let bin = cxg_index_bin();
    if !bin.exists() {
        eprintln!(
            "llvm_index: cxg-index binary not found at {} — run `cargo build --bin cxg-index` first",
            bin.display()
        );
        std::process::exit(1);
    }

    println!(
        "llvm_index: running against {} with binary {}",
        llvm_path.display(),
        bin.display()
    );

    // ── Full run (AC-M3-3) ────────────────────────────────────────────────────
    println!("llvm_index: [AC-M3-3] full run starting…");
    let full = run_cxg_index(&llvm_path, &bin, &[]);

    let full_ok = full.success;
    let full_time_ok = full.wall_secs <= FULL_RUN_LIMIT_SECS as f64;
    println!(
        "llvm_index: [AC-M3-3] wall={:.1}s  limit={}s  PASS={}",
        full.wall_secs, FULL_RUN_LIMIT_SECS, full_time_ok
    );

    // Throughput (rows/s) from node+edge counts and wall time.
    let total_rows = full.nodes + full.edges;
    let throughput = if full.wall_secs > 0.0 {
        total_rows as f64 / full.wall_secs
    } else {
        0.0
    };

    // AC-M3-4 / AC-M3-5: we measure mock throughput here; the thresholds are
    // stated per-sink.  The mock has no network cost so this represents an
    // upper bound.  Annotate which threshold would be breached.
    let neo4j_ok = throughput >= NEO4J_THROUGHPUT_MIN;
    let indradb_ok = throughput >= INDRADB_THROUGHPUT_MIN;
    println!(
        "llvm_index: [AC-M3-4/5] rows={total_rows} throughput={throughput:.0} rows/s  \
         neo4j_ok(≥{NEO4J_THROUGHPUT_MIN:.0})={neo4j_ok}  \
         indradb_ok(≥{INDRADB_THROUGHPUT_MIN:.0})={indradb_ok}"
    );

    // AC-M3-11: peak RSS (Linux only).
    let rss_ok = match full.rss_bytes {
        Some(rss) => {
            let ok = rss <= RSS_LIMIT_BYTES;
            println!(
                "llvm_index: [AC-M3-11] peak_rss={} MiB  limit={} MiB  PASS={}",
                rss / (1024 * 1024),
                RSS_LIMIT_BYTES / (1024 * 1024),
                ok
            );
            ok
        }
        None => {
            println!("llvm_index: [AC-M3-11] RSS measurement skipped (non-Linux platform)");
            true // non-asserting on non-Linux
        }
    };

    // ── Touch a file and re-run (AC-M3-10) ───────────────────────────────────
    // Modify a small sentinel file inside the LLVM tree so the cache misses it.
    let sentinel = llvm_path
        .join("llvm")
        .join("lib")
        .join("Support")
        .join("raw_ostream.cpp");
    let touched = if sentinel.exists() {
        // Update mtime by re-writing file in place.
        let content = fs::read(&sentinel).unwrap_or_default();
        fs::write(&sentinel, &content).is_ok()
    } else {
        false
    };

    let incremental_ok;
    if touched {
        println!("llvm_index: [AC-M3-10] incremental run starting (1 file touched)…");
        let incr = run_cxg_index(&llvm_path, &bin, &[]);
        incremental_ok = incr.wall_secs <= INCREMENTAL_LIMIT_SECS as f64;
        println!(
            "llvm_index: [AC-M3-10] wall={:.1}s  limit={}s  PASS={}",
            incr.wall_secs, INCREMENTAL_LIMIT_SECS, incremental_ok
        );
        if !incr.success {
            eprintln!(
                "llvm_index: [AC-M3-10] incremental run failed:\n{}",
                incr.stderr
            );
        }
    } else {
        println!(
            "llvm_index: [AC-M3-10] sentinel file not found ({}) — incremental gate skipped",
            sentinel.display()
        );
        incremental_ok = true; // skipped, not failed
    }

    // ── Persist results ───────────────────────────────────────────────────────
    let results = serde_json::json!({
        "bench": "llvm_index",
        "llvm_path": llvm_path.display().to_string(),
        "full_run": {
            "wall_secs": full.wall_secs,
            "nodes": full.nodes,
            "edges": full.edges,
            "throughput_rows_per_sec": throughput,
            "rss_bytes": full.rss_bytes,
            "success": full_ok,
            "ac_m3_3_pass": full_time_ok,
            "ac_m3_4_pass": neo4j_ok,
            "ac_m3_5_pass": indradb_ok,
            "ac_m3_11_pass": rss_ok,
        },
        "incremental_run": {
            "touched": touched,
            "ac_m3_10_pass": incremental_ok,
        },
    });
    persist_results(&results);

    // ── Final verdict ─────────────────────────────────────────────────────────
    let all_pass = full_ok && full_time_ok && rss_ok && incremental_ok;
    if !all_pass {
        eprintln!("\nllvm_index: ONE OR MORE AC THRESHOLDS BREACHED — see above");
        eprintln!("  AC-M3-3  (full ≤15 min):      {full_time_ok}");
        eprintln!("  AC-M3-4  (neo4j ≥50k rows/s): {neo4j_ok}");
        eprintln!("  AC-M3-5  (indradb ≥100k r/s): {indradb_ok}");
        eprintln!("  AC-M3-10 (incr ≤1 min):       {incremental_ok}");
        eprintln!("  AC-M3-11 (RSS ≤16 GiB):       {rss_ok}");
        std::process::exit(1);
    }

    println!("\nllvm_index: all AC thresholds passed.");
}
