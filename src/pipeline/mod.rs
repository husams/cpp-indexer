//! Pipeline orchestrator — wires Phase 0 → 0.5 → 1 → 3 → 4 (M1 scope).
//!
//! `pipeline::run` is the single entry-point used by both `cxg-index` and
//! `cxg-daemon`. Phase 2 (decorate) and Phase 5 (cross-repo) are deferred to
//! later milestones; stubs are included so later stories can drop in without
//! changing call sites.
//!
//! # Phase map (M1)
//! | Phase | Module                         | Description                       |
//! |-------|--------------------------------|-----------------------------------|
//! | 0     | `bootstrap::compile_commands`  | Parse + dedup `compile_commands.json` |
//! | 0.5   | `bootstrap::autodetect`        | Upward walk to find cc.json       |
//! | 1     | `visit::shallow`               | libclang AST → Parquet shards     |
//! | 2     | _(stub, skipped for M1)_       | Decoration pass                   |
//! | 3     | `resolve::per_repo`            | In-mem USR map → `final-edges.parquet` |
//! | 4     | `sink::*`                      | Bulk write to graph DB            |
//!
//! AC covered: AC-M1-25, AC-M1-26, AC-M1-27.

pub mod parallel;
pub mod progress;

use std::fs::File;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use clang::Clang;
use parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder;
use tracing::{info, warn};

use crate::bootstrap::{compile_commands, TuEntry};
use crate::resolve::per_repo::resolve_per_repo;
use crate::schema::arrow::{record_batch_to_edges, record_batch_to_nodes};
use crate::schema::{EdgeRecord, NodeRecord};
use crate::sink::GraphSink;
use crate::stage::writer::StageWriter;
use crate::visit::shallow::{visit_tu, VisitOptions};
use crate::{Error, Result};

// ---------------------------------------------------------------------------
// Public options type
// ---------------------------------------------------------------------------

/// Configuration passed to [`run`] by the CLI or daemon job dispatcher.
pub struct RunOptions {
    /// The input path to index (file, directory, or repo root).
    pub input_path: PathBuf,

    /// Explicit path to `compile_commands.json`. When `None`, auto-detection
    /// (Phase 0.5) is used.
    pub compile_commands: Option<PathBuf>,

    /// Repository name embedded in every node/edge record.
    pub repo_name: String,

    /// Staging directory for Parquet shards. When `None`, a temporary
    /// directory is created and removed after the run.
    pub stage_dir: Option<PathBuf>,

    /// When `true`, Phase 2 decoration is skipped (always true for M1).
    pub skip_phase2: bool,

    /// When `true`, nodes/edges whose spelling location is in a system header
    /// are excluded from Parquet output (AC-M2-14).  Default: `true`.
    pub skip_system_headers: bool,
}

// ---------------------------------------------------------------------------
// Pipeline entry point
// ---------------------------------------------------------------------------

/// Run the full indexing pipeline (Phases 0 → 4) for one repository.
///
/// # Errors
/// Propagates errors from any phase. Soft libclang parse errors (Phase 1)
/// are NOT surfaced as `Err` — they are logged and counted.
pub async fn run(sink: Arc<dyn GraphSink>, opts: RunOptions) -> Result<PipelineStats> {
    let mut stats = PipelineStats::default();

    // ── Phase 0.5: auto-detect compile_commands.json ─────────────────────
    let cc_path = match opts.compile_commands {
        Some(p) => p,
        None => {
            info!(
                "Phase 0.5: auto-detecting compile_commands.json from {:?}",
                opts.input_path
            );
            crate::bootstrap::autodetect::find_compile_commands(&opts.input_path)?
        }
    };
    info!("Phase 0.5: resolved compile_commands.json at {:?}", cc_path);

    // ── Phase 0: parse + dedup compile_commands.json ─────────────────────
    info!("Phase 0: parsing compile commands from {:?}", cc_path);
    let tu_entries: Vec<TuEntry> = compile_commands::parse(&cc_path)?;
    stats.tu_count = tu_entries.len();
    info!(
        "Phase 0: {} translation unit(s) after dedup",
        stats.tu_count
    );

    // ── Stage dir setup ───────────────────────────────────────────────────
    // When no stage_dir is supplied, create a uniquely-named directory under
    // the OS temp dir. We do not use `tempfile::TempDir` here (dev-dependency
    // only) — callers that want automatic cleanup should provide a stage_dir
    // from a `tempfile::TempDir` themselves.
    let stage_dir: PathBuf = match opts.stage_dir {
        Some(ref p) => {
            std::fs::create_dir_all(p)?;
            p.clone()
        }
        None => {
            // Generate a unique subdir under temp using process ID + timestamp.
            let pid = std::process::id();
            let ts = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap_or_default()
                .as_nanos();
            let dir = std::env::temp_dir().join(format!("cxg-stage-{pid}-{ts}"));
            std::fs::create_dir_all(&dir)?;
            dir
        }
    };

    // ── Phase 1: libclang AST visitor → Parquet shards ───────────────────
    info!(
        "Phase 1: visiting {} TU(s) into {:?}",
        tu_entries.len(),
        stage_dir
    );
    let clang = Clang::new().map_err(|e| Error::Clang(format!("Clang::new failed: {e}")))?;
    let mut writer = StageWriter::new(&stage_dir, 0)?;

    for entry in &tu_entries {
        // Strip the source file path from the compiler args before passing to
        // libclang. In compile_commands.json the source file often appears as
        // the last positional argument (e.g. `clang++ -std=c++14 -I. foo.cpp`).
        // libclang receives the file to parse separately via `parser(file_path)`,
        // so including it again in the args causes `AstDeserialization` errors.
        let filtered_args: Vec<String> = filter_compiler_args(&entry.file, &entry.args);
        let vo = VisitOptions {
            repo_name: &opts.repo_name,
            tu_hash: *entry.hash.as_bytes(),
            file_path: &entry.file,
            args: &filtered_args,
            skip_system_headers: opts.skip_system_headers,
        };
        let partial = visit_tu(&clang, &vo, &mut writer)?;
        if partial {
            stats.partial_tu_count += 1;
            warn!("Phase 1: partial parse for {:?}", entry.file);
        }
    }
    writer.finish()?;
    info!(
        "Phase 1: complete ({} partial TU(s))",
        stats.partial_tu_count
    );

    // ── Phase 2: decoration (stub — deferred to later milestone) ─────────
    if !opts.skip_phase2 {
        info!("Phase 2: decoration pass is not yet implemented; skipping");
    }

    // ── Phase 3: in-memory USR resolve → final-edges.parquet ─────────────
    info!("Phase 3: resolving edges in {:?}", stage_dir);
    let _final_edges_path = resolve_per_repo(&stage_dir)?;
    info!("Phase 3: complete");

    // ── Phase 4: preflight + bulk write ──────────────────────────────────
    info!("Phase 4: writing to sink '{}'", sink.backend_name());
    sink.preflight().await?;
    sink.ensure_indexes().await?;

    // Write all node records from the staged shards.
    let node_records = load_nodes_from_stage(&stage_dir)?;
    stats.nodes_written = node_records.len() as u64;
    if !node_records.is_empty() {
        sink.write_nodes(&node_records).await?;
    }

    // Write all resolved edge records from final-edges.parquet.
    let edge_records = load_edges_from_stage(&stage_dir)?;
    stats.edges_written = edge_records.len() as u64;
    if !edge_records.is_empty() {
        sink.write_edges(&edge_records).await?;
    }

    info!(
        "Phase 4: complete — {} nodes, {} edges written to '{}'",
        stats.nodes_written,
        stats.edges_written,
        sink.backend_name()
    );

    Ok(stats)
}

// ---------------------------------------------------------------------------
// Compiler-arg filter
// ---------------------------------------------------------------------------

/// Remove compiler-driver tokens from `args` that libclang should not see.
///
/// Specifically removes:
/// - The source file path itself (first positional arg that matches `file`).
/// - The compiler executable name at position 0 (e.g. `clang++`, `g++`).
///   libclang does not want the argv[0] compiler name.
///
/// libclang receives the file to parse through `Index::parser(file_path)`;
/// including the file path again in args causes `AstDeserialization` errors.
pub(crate) fn filter_compiler_args(file: &Path, args: &[String]) -> Vec<String> {
    let file_str = file.to_string_lossy();
    args.iter()
        .enumerate()
        .filter(|(i, arg)| {
            // Skip the compiler executable at position 0.
            if *i == 0 {
                // Heuristic: if the first arg looks like a compiler name (no
                // leading `-` and no `/` in a relative position), drop it.
                let a = arg.as_str();
                if !a.starts_with('-')
                    && (a == "clang++"
                        || a == "clang"
                        || a == "g++"
                        || a == "gcc"
                        || a == "cc"
                        || a == "c++")
                {
                    return false;
                }
            }
            // Skip the source file path.
            arg.as_str() != file_str.as_ref()
        })
        .map(|(_, arg)| arg.clone())
        .collect()
}

// ---------------------------------------------------------------------------
// Stage-reader helpers
// ---------------------------------------------------------------------------

/// Load all node records from `worker-*/nodes-*.parquet` shards.
fn load_nodes_from_stage(stage_dir: &Path) -> Result<Vec<NodeRecord>> {
    let mut records = Vec::new();
    for shard_path in collect_shards(stage_dir, "nodes")? {
        let file = File::open(&shard_path)?;
        let reader = ParquetRecordBatchReaderBuilder::try_new(file)
            .map_err(|e| Error::Schema(format!("open node shard {}: {e}", shard_path.display())))?
            .build()
            .map_err(|e| {
                Error::Schema(format!("build reader for {}: {e}", shard_path.display()))
            })?;
        for batch_result in reader {
            let batch = batch_result.map_err(|e| Error::Schema(format!("read node batch: {e}")))?;
            records.extend(record_batch_to_nodes(&batch));
        }
    }
    Ok(records)
}

/// Load all resolved edge records from `<stage_dir>/final-edges.parquet`.
fn load_edges_from_stage(stage_dir: &Path) -> Result<Vec<EdgeRecord>> {
    let final_path = stage_dir.join("final-edges.parquet");
    if !final_path.exists() {
        return Ok(Vec::new());
    }
    let file = File::open(&final_path)?;
    let reader = ParquetRecordBatchReaderBuilder::try_new(file)
        .map_err(|e| Error::Schema(format!("open final-edges: {e}")))?
        .build()
        .map_err(|e| Error::Schema(format!("build reader for final-edges: {e}")))?;

    let mut records = Vec::new();
    for batch_result in reader {
        let batch = batch_result.map_err(|e| Error::Schema(format!("read edge batch: {e}")))?;
        records.extend(record_batch_to_edges(&batch));
    }
    Ok(records)
}

/// Collect shard paths for `prefix` (`"nodes"` or `"edges"`) from `worker-*/` dirs.
fn collect_shards(stage_dir: &Path, prefix: &str) -> Result<Vec<PathBuf>> {
    let mut paths = Vec::new();
    if !stage_dir.exists() {
        return Ok(paths);
    }
    for entry in std::fs::read_dir(stage_dir)? {
        let entry = entry?;
        let path = entry.path();
        let name = path.file_name().and_then(|n| n.to_str()).unwrap_or("");
        if !path.is_dir() || !name.starts_with("worker-") {
            continue;
        }
        for shard in std::fs::read_dir(&path)? {
            let shard = shard?;
            let sp = shard.path();
            let sn = sp.file_name().and_then(|n| n.to_str()).unwrap_or("");
            if sn.starts_with(prefix) && sn.ends_with(".parquet") {
                paths.push(sp);
            }
        }
    }
    paths.sort();
    Ok(paths)
}

// ---------------------------------------------------------------------------
// Pipeline statistics
// ---------------------------------------------------------------------------

/// Counters produced by a successful pipeline run.
#[derive(Debug, Default, Clone)]
pub struct PipelineStats {
    /// Total number of translation units parsed (after dedup).
    pub tu_count: usize,
    /// Number of TUs that had at least one libclang parse error.
    pub partial_tu_count: usize,
    /// Total nodes written to the sink.
    pub nodes_written: u64,
    /// Total edges written to the sink.
    pub edges_written: u64,
}
