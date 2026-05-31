//! Real before/after graph-size measurement for the graph-symbol-ids feature.
//!
//! Runs the REAL pipeline (libclang visit + per-repo SymbolAllocator) over a
//! fixture and tallies, from the emitted Parquet records, the identity-field
//! bytes the feature removes from the graph vs. the integer-ID bytes it adds,
//! plus the actual on-disk SQLite sidecar cost. No live Neo4j/IndraDB needed:
//! every record carries BOTH representations at the sink-write point.
//!
//!   before_graph = Σ_nodes(len(usr)+len(file_path)) + Σ_edges(len(src_usr)+len(dst_usr))
//!   after_graph  = 16·nodes + Σ_edges(8 + [8 if dst_id] + len(dst_repo_name))
//!   sqlite       = on-disk size of cxg-symbols.db (each USR/path stored once)
//!   net win      = before_graph − after_graph − sqlite
//!
//! Usage:  cargo run --release --example graph_size_bench -- [cache_size] [fixture_dir]
//! RSS:    /usr/bin/time -l ./target/release/examples/graph_size_bench 1000
//! (requires LIBCLANG_PATH / DYLD_LIBRARY_PATH set — see AGENTS.md)

use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use arrow::record_batch::RecordBatch;
use cpp_indexer::{
    bootstrap::compile_commands,
    pipeline::parallel::run_phase1_parallel,
    resolve::symbol_map::SymbolAllocator,
    schema::arrow::{record_batch_to_edges, record_batch_to_nodes},
};
use parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder;

fn main() {
    let mut args = std::env::args().skip(1);
    let cache_size: usize = args.next().and_then(|s| s.parse().ok()).unwrap_or(10_000);
    let fixture_dir: PathBuf = args.next().map(PathBuf::from).unwrap_or_else(|| {
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/m1_5file")
    });
    // 3rd arg: "false"/"0" indexes system headers too → balloons the graph to
    // thousands of nodes, exercising the at-scale regime past SQLite break-even.
    let skip_system = !matches!(args.next().as_deref(), Some("false") | Some("0"));
    let cc_path = fixture_dir.join("compile_commands.json");

    // Unique temp workspace (no tempfile dev-dep available to examples).
    let work = std::env::temp_dir().join(format!("cxg-size-bench-{}", std::process::id()));
    let stage = work.join("stage");
    let db_path = work.join("cxg-symbols.db");
    let _ = std::fs::remove_dir_all(&work);
    std::fs::create_dir_all(&stage).expect("create stage dir");

    let entries = compile_commands::parse(&cc_path).expect("parse compile_commands.json");
    assert!(!entries.is_empty(), "fixture must have at least one TU");

    let alloc = Arc::new(SymbolAllocator::open(&db_path, cache_size).expect("open allocator"));
    let stats = run_phase1_parallel(
        &entries,
        &stage,
        "bench-repo",
        skip_system,
        None,
        Some(alloc),
        Default::default(),
    )
    .expect("phase 1 must succeed");

    let nodes = read_records(&stage, "nodes-*.parquet", record_batch_to_nodes);
    let edges = read_records(&stage, "edges-*.parquet", record_batch_to_edges);

    // ── tally identity bytes both ways ──────────────────────────────────────
    let mut before: u64 = 0;
    let mut after: u64 = 0;
    let mut uniq: HashSet<String> = HashSet::new();
    let mut uniq_paths: HashSet<String> = HashSet::new();

    for n in &nodes {
        before += n.usr.len() as u64 + n.file_path.len() as u64;
        after += 16; // symbol_id (i64) + file_id (i64)
        uniq.insert(n.usr.clone());
        uniq_paths.insert(n.file_path.clone());
    }
    for e in &edges {
        before += e.src_usr.len() as u64 + e.dst_usr.as_deref().map_or(0, str::len) as u64;
        after += 8 // src_id
            + if e.dst_id.is_some() { 8 } else { 0 }
            + e.dst_repo_name.len() as u64;
        uniq.insert(e.src_usr.clone());
        if let Some(d) = &e.dst_usr {
            uniq.insert(d.clone());
        }
    }

    let peak_rss = max_rss_bytes();
    // SQLite cost, two ways: (a) real on-disk file (page-granular, btree+index
    // overhead, 20KB floor); (b) apples-to-apples payload = each unique string once.
    let sqlite_file = std::fs::metadata(&db_path).map(|m| m.len()).unwrap_or(0);
    let sqlite_payload: u64 = uniq.iter().map(|s| s.len() as u64).sum::<u64>()
        + uniq_paths.iter().map(|s| s.len() as u64).sum::<u64>();
    let gross = before as i64 - after as i64;
    let net_file = gross - sqlite_file as i64;
    let net_payload = gross - sqlite_payload as i64;
    // ρ = USR-repetition: total node+edge identity refs / unique strings.
    let total_refs = nodes.len() + edges.len() * 2; // each edge has src + (maybe) dst
    let rho = total_refs as f64 / uniq.len().max(1) as f64;
    let pct = |part: i64, whole: u64| {
        if whole == 0 {
            0.0
        } else {
            100.0 * part as f64 / whole as f64
        }
    };

    println!("\n════════════ graph-symbol-ids: real size measurement ════════════");
    println!("fixture            : {}", fixture_dir.display());
    println!("cache_size         : {cache_size}    skip_system_headers: {skip_system}");
    println!(
        "TUs parsed         : {} ok / {} partial / {} error",
        stats.tu_ok, stats.tu_partial, stats.tu_error
    );
    println!("nodes / edges      : {} / {}", nodes.len(), edges.len());
    println!(
        "unique USRs        : {}   ρ (USR repetition) = {:.2}",
        uniq.len(),
        rho
    );
    println!("unique file paths  : {}", uniq_paths.len());
    println!("──────────────────────────────────────────────────────────────────");
    println!("graph identity bytes BEFORE (USR+path strings) : {before:>10}");
    println!("graph identity bytes AFTER  (integer IDs)       : {after:>10}");
    println!(
        "  gross graph saving                            : {gross:>10}  ({:.1}% of before)",
        pct(gross, before)
    );
    println!("SQLite cost — on-disk file (real, w/ overhead)  : {sqlite_file:>10}");
    println!("SQLite cost — payload (unique strings, apples)  : {sqlite_payload:>10}");
    println!("──────────────────────────────────────────────────────────────────");
    println!(
        "NET vs file    (gross − sqlite_file)            : {net_file:>10}  ({:.1}% of before)",
        pct(net_file, before)
    );
    println!(
        "NET vs payload (gross − sqlite_payload)         : {net_payload:>10}  ({:.1}% of before)",
        pct(net_payload, before)
    );
    println!("  (payload net is the apples-to-apples figure; both are CONSERVATIVE —");
    println!("   graph side ignores DB per-property + 4 M8 covering-index overhead on usr)");
    println!("──────────────────────────────────────────────────────────────────");
    println!(
        "peak process RSS (getrusage, whole run)         : {peak_rss:>10}  ({:.1} MiB)",
        peak_rss as f64 / (1024.0 * 1024.0)
    );
    println!("════════════════════════════════════════════════════════════════════\n");

    let _ = std::fs::remove_dir_all(&work);
}

/// Peak resident set size of this process, in bytes. macOS `ru_maxrss` is bytes
/// (Linux reports KiB — normalised here so the number is always bytes).
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

fn read_records<T>(stage: &Path, shard: &str, conv: fn(&RecordBatch) -> Vec<T>) -> Vec<T> {
    let pattern = stage
        .join("worker-*")
        .join(shard)
        .to_string_lossy()
        .into_owned();
    let mut out = Vec::new();
    for entry in glob::glob(&pattern).expect("glob").flatten() {
        let file = std::fs::File::open(&entry).expect("open parquet");
        let reader = ParquetRecordBatchReaderBuilder::try_new(file)
            .expect("reader builder")
            .build()
            .expect("batch reader");
        for batch in reader.flatten() {
            out.extend(conv(&batch));
        }
    }
    out
}
