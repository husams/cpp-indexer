//! Pipeline orchestrator — wires Phase 0 → 0.5 → 1 → 3 → 4.
//!
//! `pipeline::run` is the single entry-point used by both `cxg-index` and
//! `cxg-daemon`. Phase 2 (decorate) and Phase 5 (cross-repo) are deferred to
//! later milestones; stubs are included so later stories can drop in without
//! changing call sites.
//!
//! # Phase map
//! | Phase | Module                         | Description                               |
//! |-------|--------------------------------|-------------------------------------------|
//! | 0     | `bootstrap::compile_commands`  | Parse + dedup `compile_commands.json`     |
//! | 0.5   | `bootstrap::autodetect`        | Upward walk to find cc.json               |
//! | 1     | `visit::shallow`               | libclang AST → Parquet shards             |
//! | 2     | _(stub, skipped for M1)_       | Decoration pass                           |
//! | 3     | `resolve::per_repo`            | In-mem USR map → `final-edges.parquet`   |
//! | 4     | `sink::*`                      | Bulk write to graph DB                    |
//!
//! S22 adds REPO node emission between Phase 3 and Phase 4: a single REPO node
//! is prepended to the node batch and one BELONGS_TO_REPO edge is appended per
//! non-REPO node in the edge batch (AC-M4-1, AC-M4-2).
//!
//! AC covered: AC-M1-25, AC-M1-26, AC-M1-27, AC-M4-1, AC-M4-2.

pub mod parallel;
pub mod progress;

use std::fs::File;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use clang::Clang;
use parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder;
use tracing::{info, warn};

use crate::bootstrap::{compile_commands, repo_meta, TuEntry};
use crate::resolve::per_repo::resolve_per_repo;
use crate::schema::arrow::{record_batch_to_edges, record_batch_to_nodes};
use crate::schema::edges::EdgeKind;
use crate::schema::nodes::NodeKind;
use crate::schema::version::{schema_version_attrs, SCHEMA_VERSION_TAG};
use crate::schema::{EdgeRecord, NodeRecord};
use crate::sink::GraphSink;
use crate::stage::manifest::{Manifest, ManifestEntry};
use crate::stage::writer::StageWriter;
use crate::visit::decorate;
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

    /// When `true`, the incremental content-hash cache is disabled and all TUs
    /// are re-parsed unconditionally.  Default: `false`.
    pub skip_cache: bool,

    /// When `true`, REPO node + BELONGS_TO_REPO edge emission is skipped.
    ///
    /// Set to `true` in tests that do not provide a git repository.  In
    /// production this is always `false` (the default).
    pub skip_repo_node: bool,
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

    // Retrieve libclang version once per run; used as a cache invalidation key
    // (AC-M3-9: any libclang version change triggers full re-parse).
    let libclang_version = clang::get_version();

    // Load the incremental manifest (or start fresh if missing / version-mismatched).
    let manifest_path = stage_dir.join("manifest.json");
    let mut manifest = if opts.skip_cache {
        Manifest::new()
    } else {
        Manifest::load_or_invalidate(&manifest_path)?
    };

    let mut writer = StageWriter::new(&stage_dir, 0)?;

    for entry in &tu_entries {
        // Strip the source file path from the compiler args before passing to
        // libclang. In compile_commands.json the source file often appears as
        // the last positional argument (e.g. `clang++ -std=c++14 -I. foo.cpp`).
        // libclang receives the file to parse separately via `parser(file_path)`,
        // so including it again in the args causes `AstDeserialization` errors.
        let filtered_args: Vec<String> = filter_compiler_args(&entry.file, &entry.args);

        // Compute content hashes for cache lookup (AC-M3-7, AC-M3-8).
        // source_hash: blake3 of the source file bytes (detects file edits).
        // args_hash: blake3 of the filtered arg list (detects flag changes).
        let source_hash = hash_source_file(&entry.file)?;
        let args_hash = hash_args(&filtered_args);

        // Cache pre-pass (AC-M3-7): skip TU if all four cache keys match.
        if !opts.skip_cache
            && manifest
                .cache_hit(&source_hash, &args_hash, &libclang_version)
                .is_some()
        {
            info!(
                "Phase 1: cache hit — skipping {:?}",
                entry.file.file_name().unwrap_or_default()
            );
            stats.cache_hits += 1;
            continue;
        }

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

        // Record a cache entry for this TU so the next run can skip it.
        if !opts.skip_cache {
            let shard_paths = writer.shards_written_since_last_cache_record();
            let cache_entry = ManifestEntry::new(
                source_hash,
                args_hash,
                libclang_version.clone(),
                shard_paths,
            );
            manifest.append_and_save(&manifest_path, cache_entry)?;
        }
    }
    writer.finish()?;
    info!(
        "Phase 1: complete ({} partial TU(s), {} cache hit(s))",
        stats.partial_tu_count, stats.cache_hits
    );

    // ── Phase 2: decoration (AC-M5-5, AC-M5-6) ───────────────────────────
    let decorated_count = decorate::run(&clang, &tu_entries, &stage_dir, opts.skip_phase2)?;
    if !opts.skip_phase2 {
        info!("Phase 2: {} node(s) decorated", decorated_count);
    }

    // ── Phase 3: in-memory USR resolve → final-edges.parquet ─────────────
    info!("Phase 3: resolving edges in {:?}", stage_dir);
    let _final_edges_path = resolve_per_repo(&stage_dir)?;
    info!("Phase 3: complete");

    // ── Phase 4: preflight + bulk write ──────────────────────────────────
    info!("Phase 4: writing to sink '{}'", sink.backend_name());
    sink.preflight().await?;
    sink.ensure_indexes().await?;

    // Write the SchemaVersion singleton node (ADR-9, AC-M6-6).
    // `CXG_INDEXER_COMMIT` is baked in by build.rs when available; falls back
    // to "unknown" so tests and local builds without a git repo still compile.
    let indexer_commit = option_env!("CXG_INDEXER_COMMIT").unwrap_or("unknown");
    let sv_attrs = schema_version_attrs(indexer_commit, &libclang_version);
    sink.write_schema_version(SCHEMA_VERSION_TAG, &sv_attrs)
        .await?;
    info!(
        "Phase 4: SchemaVersion node written (tag={})",
        SCHEMA_VERSION_TAG
    );

    // Load staged nodes and edges produced by Phases 1–3.
    let mut node_records = load_nodes_from_stage(&stage_dir)?;
    let mut edge_records = load_edges_from_stage(&stage_dir)?;

    // ── REPO node + BELONGS_TO_REPO edges (S22, AC-M4-1, AC-M4-2) ────────
    // Emit one REPO node for this repository and one BELONGS_TO_REPO edge for
    // every other node, unless the caller has opted out (e.g. in tests that
    // run without a git repository).
    if !opts.skip_repo_node {
        let meta = repo_meta::collect(&opts.repo_name, &opts.input_path)?;

        let repo_usr = format!("repo:{}", meta.name);

        // Build attrs_json for the REPO node.  These fields are not first-class
        // columns on NodeRecord; they live in attrs_json so that the existing
        // Parquet schema and sink MERGE queries remain stable.
        let attrs = serde_json::json!({
            "root_path": meta.root_path.to_string_lossy(),
            "commit_sha": meta.commit_sha,
            "commit_date": meta.commit_date,
            "sink": sink.backend_name(),
        });

        let repo_node = NodeRecord {
            usr: repo_usr.clone(),
            kind: NodeKind::Repo,
            name: meta.name.clone(),
            qualified_name: meta.name.clone(),
            mangled_name: None,
            file_path: meta.root_path.to_string_lossy().into_owned(),
            line: None,
            col: None,
            repo_name: meta.name.clone(),
            attrs_json: attrs.to_string(),
            partial: false,
            phase: 0,
            tu_hash: [0u8; 32],
        };

        // Prepend so the REPO node is written before the nodes that reference it.
        node_records.insert(0, repo_node);

        // One BELONGS_TO_REPO edge from every non-REPO node to the repo node.
        // We emit edges for all nodes that were already in the batch (index >= 1
        // after the insert above) so the REPO node itself is excluded.
        let belongs_edges: Vec<EdgeRecord> = node_records[1..]
            .iter()
            .map(|n| EdgeRecord {
                src_usr: n.usr.clone(),
                dst_usr: Some(repo_usr.clone()),
                dst_placeholder: None,
                kind: EdgeKind::BelongsToRepo,
                resolved: true,
                cross_repo_candidate: false,
                repo_name: meta.name.clone(),
                attrs_json: "{}".to_owned(),
                tu_hash: [0u8; 32],
            })
            .collect();

        edge_records.extend(belongs_edges);
        info!(
            "Phase 4 (S22): emitted REPO node '{}' + {} BELONGS_TO_REPO edges",
            repo_usr,
            node_records.len().saturating_sub(1)
        );
    }

    stats.nodes_written = node_records.len() as u64;
    if !node_records.is_empty() {
        sink.write_nodes(&node_records).await?;
    }

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
// Content-hash helpers (AC-M3-7, AC-M3-8)
// ---------------------------------------------------------------------------

/// Compute a Blake3 hex digest of the source file bytes.
///
/// Reading the file at parse time is intentional: it captures the state of the
/// file at the moment the TU is being considered, not the state recorded in
/// `compile_commands.json`.
fn hash_source_file(file: &Path) -> Result<String> {
    let bytes = std::fs::read(file)?;
    Ok(blake3::hash(&bytes).to_hex().to_string())
}

/// Compute a Blake3 hex digest of the (filtered) compiler argument list.
///
/// Arguments are joined with NUL bytes so that adjacent-arg boundary changes
/// are captured (e.g. `["-I", "dir"]` vs `["-Idir"]`).
fn hash_args(args: &[String]) -> String {
    let mut hasher = blake3::Hasher::new();
    for arg in args {
        hasher.update(arg.as_bytes());
        hasher.update(&[0u8]); // NUL separator
    }
    hasher.finalize().to_hex().to_string()
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

/// Load all node records from Phase 1 and Phase 2 Parquet shards, deduplicating
/// by USR and preferring `phase=2` records over `phase=1` records.
///
/// Phase 1 shards live in `worker-*/nodes-*.parquet`.
/// Phase 2 shards live in `<stage_dir>/phase2-nodes-*.parquet`.
///
/// When both phases have a record for the same USR the `phase=2` record is kept
/// so that decorated attrs_json is used in the sink write (AC-M5-5).
fn load_nodes_from_stage(stage_dir: &Path) -> Result<Vec<NodeRecord>> {
    use std::collections::HashMap;

    // key: USR → (phase, NodeRecord); we track phase so we can upgrade phase=1 → phase=2.
    let mut by_usr: HashMap<String, (u8, NodeRecord)> = HashMap::new();

    // Load Phase 1 shards from worker-* directories.
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
            for record in record_batch_to_nodes(&batch) {
                let phase = record.phase;
                let usr = record.usr.clone();
                by_usr
                    .entry(usr)
                    .and_modify(|(existing_phase, existing_record)| {
                        if phase > *existing_phase {
                            *existing_phase = phase;
                            *existing_record = record.clone();
                        }
                    })
                    .or_insert((phase, record));
            }
        }
    }

    // Load Phase 2 shards from the stage_dir root (phase2-nodes-*.parquet).
    for shard_path in collect_phase2_shards(stage_dir)? {
        let file = File::open(&shard_path)?;
        let reader = ParquetRecordBatchReaderBuilder::try_new(file)
            .map_err(|e| {
                Error::Schema(format!(
                    "open phase2 node shard {}: {e}",
                    shard_path.display()
                ))
            })?
            .build()
            .map_err(|e| {
                Error::Schema(format!("build reader for {}: {e}", shard_path.display()))
            })?;
        for batch_result in reader {
            let batch = batch_result.map_err(|e| Error::Schema(format!("read node batch: {e}")))?;
            for record in record_batch_to_nodes(&batch) {
                let phase = record.phase;
                let usr = record.usr.clone();
                // Phase 2 always wins over Phase 1 for the same USR.
                by_usr
                    .entry(usr)
                    .and_modify(|(existing_phase, existing_record)| {
                        if phase >= *existing_phase {
                            *existing_phase = phase;
                            *existing_record = record.clone();
                        }
                    })
                    .or_insert((phase, record));
            }
        }
    }

    Ok(by_usr.into_values().map(|(_, r)| r).collect())
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

/// Collect Phase 2 node shard paths (`phase2-nodes-*.parquet`) from `stage_dir` root.
fn collect_phase2_shards(stage_dir: &Path) -> Result<Vec<PathBuf>> {
    let mut paths = Vec::new();
    if !stage_dir.exists() {
        return Ok(paths);
    }
    for entry in std::fs::read_dir(stage_dir)? {
        let entry = entry?;
        let path = entry.path();
        let name = path.file_name().and_then(|n| n.to_str()).unwrap_or("");
        if path.is_file() && name.starts_with("phase2-nodes") && name.ends_with(".parquet") {
            paths.push(path);
        }
    }
    paths.sort();
    Ok(paths)
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
    /// Total number of translation units after dedup (parsed + skipped).
    pub tu_count: usize,
    /// Number of TUs skipped because the content-hash cache matched (AC-M3-7).
    pub cache_hits: u64,
    /// Number of TUs that had at least one libclang parse error.
    pub partial_tu_count: usize,
    /// Total nodes written to the sink.
    pub nodes_written: u64,
    /// Total edges written to the sink.
    pub edges_written: u64,
}
