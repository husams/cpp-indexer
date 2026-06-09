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

use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Instant;

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
use crate::stage::schema::{collect_shards, open_shard_reader, MissingDirPolicy};
use crate::visit::decorate;
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

    /// Number of Phase 1 worker threads. When `None`, rayon uses logical CPUs.
    pub workers: Option<usize>,

    /// When `true`, the incremental content-hash cache is disabled and all TUs
    /// are re-parsed unconditionally.  Default: `false`.
    pub skip_cache: bool,

    /// When `true`, REPO node + BELONGS_TO_REPO edge emission is skipped.
    ///
    /// Set to `true` in tests that do not provide a git repository.  In
    /// production this is always `false` (the default).
    pub skip_repo_node: bool,

    /// Override path for the per-repo SQLite symbol/file database.
    ///
    /// When `None` (default), the database is opened at `<stage_dir>/cxg-symbols.db`.
    /// Corresponds to the `CXG_SYMBOL_DB_PATH` env var / `--symbol-db-path` CLI flag.
    pub symbol_db_path: Option<PathBuf>,

    /// LRU cache size for the `SymbolAllocator` (number of entries).
    ///
    /// `0` disables the cache entirely; every allocation hits SQLite directly.
    /// Default: 100_000. Corresponds to `CXG_SYMBOL_CACHE_SIZE` / `--symbol-cache-size`.
    pub symbol_cache_size: usize,

    /// Phase 1 memory tuning (Issue 0002 Bug 1): arena-trim cadence + Index
    /// recycle interval.
    pub phase1_tuning: parallel::Phase1Tuning,

    /// Byte budget for the Phase 4 streaming write buffer (Issue 0002 Bug 2).
    ///
    /// `0` disables the byte budget (row-only flushing).  Default ~64 MiB.
    pub write_buffer_bytes: usize,

    /// When `true`, skip Phases 0–3 and run Phase 4 against the existing
    /// `stage_dir` (Issue 0002 Bug 2 resume).  Requires a populated
    /// `stage_dir` with `cxg-symbols.db` present.
    pub write_only: bool,
}

impl Default for RunOptions {
    fn default() -> Self {
        Self {
            input_path: PathBuf::new(),
            compile_commands: None,
            repo_name: "default".to_owned(),
            stage_dir: None,
            skip_phase2: false,
            skip_system_headers: true,
            workers: None,
            skip_cache: false,
            skip_repo_node: false,
            symbol_db_path: None,
            symbol_cache_size: crate::config::DEFAULT_SYMBOL_CACHE_SIZE,
            phase1_tuning: parallel::Phase1Tuning::default(),
            write_buffer_bytes: crate::config::DEFAULT_WRITE_BUFFER_BYTES,
            write_only: false,
        }
    }
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
    let run_started = Instant::now();
    let mut stats = PipelineStats::default();

    // ── Stage dir + allocator + libclang version: produced by either the
    //    normal Phases 0–3 path or the --write-only resume path ────────────
    let stage_dir: PathBuf;
    let allocator: Option<std::sync::Arc<crate::resolve::symbol_map::SymbolAllocator>>;
    let libclang_version: String;

    if opts.write_only {
        // ── Issue 0002 Bug 2 resume: skip Phases 0–3, run Phase 4 against an
        //    existing stage dir.  Reuse the staged Parquet shards + symbol map.
        let dir = opts.stage_dir.clone().ok_or_else(|| {
            Error::Schema(
                "--write-only requires --stage-dir pointing at a populated stage directory"
                    .to_owned(),
            )
        })?;
        if !dir.is_dir() {
            return Err(Error::Schema(format!(
                "--write-only stage dir {} does not exist; run Phases 0–3 first",
                dir.display()
            )));
        }

        // The symbol map must exist (Phase 4 needs it for the REPO node + ID
        // resolution).  Default path is <stage_dir>/cxg-symbols.db.
        let symbol_db_path = opts
            .symbol_db_path
            .clone()
            .unwrap_or_else(|| dir.join("cxg-symbols.db"));
        if !symbol_db_path.exists() {
            return Err(Error::Schema(format!(
                "--write-only requires the symbol map {}; the stage dir looks incomplete",
                symbol_db_path.display()
            )));
        }

        allocator = crate::resolve::symbol_map::SymbolAllocator::open(
            &symbol_db_path,
            opts.symbol_cache_size,
        )
        .map(std::sync::Arc::new)
        .ok();

        // Re-derive the libclang version for the SchemaVersion node attrs; no
        // parse is needed in write-only mode (just the version string).
        libclang_version = clang::get_version();
        stage_dir = dir;

        info!(
            "--write-only: skipping Phases 0–3; writing Phase 4 from {:?}",
            stage_dir
        );
    } else {
        // ── Phase 0.5: auto-detect compile_commands.json ─────────────────
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

        // ── Phase 0: parse + dedup compile_commands.json ─────────────────
        info!("Phase 0: parsing compile commands from {:?}", cc_path);
        let tu_entries: Vec<TuEntry> = filter_entries_to_input_scope(
            &cc_path,
            &opts.input_path,
            compile_commands::parse(&cc_path)?,
        )?;
        stats.tu_count = tu_entries.len();
        info!(
            "Phase 0: {} translation unit(s) after dedup",
            stats.tu_count
        );

        // ── Stage dir setup ───────────────────────────────────────────────
        // When no stage_dir is supplied, create a uniquely-named directory under
        // the OS temp dir. We do not use `tempfile::TempDir` here (dev-dependency
        // only) — callers that want automatic cleanup should provide a stage_dir
        // from a `tempfile::TempDir` themselves.
        let dir: PathBuf = match opts.stage_dir {
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
                let d = std::env::temp_dir().join(format!("cxg-stage-{pid}-{ts}"));
                std::fs::create_dir_all(&d)?;
                d
            }
        };

        // ── SymbolAllocator: construct before Phase 1 so Phase 4 can reuse it ──
        // Default db path: <stage_dir>/cxg-symbols.db.
        // Cache size from config (default 100_000); 0 disables cache.
        let symbol_db_path = opts
            .symbol_db_path
            .clone()
            .unwrap_or_else(|| dir.join("cxg-symbols.db"));
        let alloc: Option<std::sync::Arc<crate::resolve::symbol_map::SymbolAllocator>> =
            crate::resolve::symbol_map::SymbolAllocator::open(
                &symbol_db_path,
                opts.symbol_cache_size,
            )
            .map(std::sync::Arc::new)
            .ok(); // Non-fatal if SQLite fails; IDs will be 0.

        // ── Phase 1: libclang AST visitor → Parquet shards ───────────────
        info!(
            "Phase 1: visiting {} TU(s) into {:?}",
            tu_entries.len(),
            dir
        );
        let version = {
            let clang = crate::visit::shallow::global_clang();

            // Retrieve libclang version once per run; used as a cache invalidation key
            // (AC-M3-9: any libclang version change triggers full re-parse).
            let libclang_version = clang::get_version();

            // Load the incremental manifest (or start fresh if missing / version-mismatched).
            let manifest_path = dir.join("manifest.json");
            let mut manifest = if opts.skip_cache {
                Manifest::new()
            } else {
                Manifest::load_or_invalidate(&manifest_path)?
            };

            let mut changed_entries = Vec::new();
            let mut cache_entries = Vec::new();

            for entry in &tu_entries {
                let filtered_args: Vec<String> = filter_compiler_args(&entry.file, &entry.args);
                let source_hash = hash_source_file(&entry.file)?;
                let args_hash = hash_args(&filtered_args);

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

                changed_entries.push(entry.clone());
                cache_entries.push((source_hash, args_hash));
            }

            let parallel_stats = parallel::run_phase1_parallel(
                &changed_entries,
                &dir,
                &opts.repo_name,
                opts.skip_system_headers,
                opts.workers,
                alloc.clone(),
                opts.phase1_tuning,
            )?;
            stats.partial_tu_count = parallel_stats.tu_partial.try_into().unwrap_or(usize::MAX);
            stats.failed_tu_count = parallel_stats.tu_error.try_into().unwrap_or(usize::MAX);

            if !opts.skip_cache && parallel_stats.tu_error == 0 {
                for (source_hash, args_hash) in cache_entries {
                    let cache_entry = ManifestEntry::new(
                        source_hash,
                        args_hash,
                        libclang_version.clone(),
                        Vec::new(),
                    );
                    manifest.append_and_save(&manifest_path, cache_entry)?;
                }
            } else if !opts.skip_cache && parallel_stats.tu_error > 0 {
                warn!(
                    errors = parallel_stats.tu_error,
                    "Phase 1: not updating cache manifest because one or more TUs failed"
                );
            }
            info!(
                "Phase 1: complete ({} ok TU(s), {} partial TU(s), {} failed TU(s), {} cache hit(s))",
                parallel_stats.tu_ok,
                stats.partial_tu_count,
                parallel_stats.tu_error,
                stats.cache_hits
            );

            // ── Phase 2: decoration (AC-M5-5, AC-M5-6) ───────────────────
            let decorated_count = decorate::run(clang, &tu_entries, &dir, opts.skip_phase2)?;
            if !opts.skip_phase2 {
                info!("Phase 2: {} node(s) decorated", decorated_count);
            }

            libclang_version
        };

        // ── Phase 3: in-memory USR resolve → final-edges.parquet ─────────
        info!("Phase 3: resolving edges in {:?}", dir);
        let _final_edges_path = resolve_per_repo(&dir)?;
        info!("Phase 3: complete");

        stage_dir = dir;
        allocator = alloc;
        libclang_version = version;
    }

    // ── Phase 4: preflight + bulk write ──────────────────────────────────
    info!("Phase 4: writing to sink '{}'", sink.backend_name());
    sink.preflight().await?;
    sink.ensure_indexes().await?;

    // Write-path schema-version gate (Story 5, ADR-4, S6-SC-01/02).
    // Refuse to append v6 data to a pre-v6 graph without an explicit reset.
    // None (fresh DB / post-reset) → OK; matching tag → OK; stale tag → Error.
    crate::resolve::cross_repo::check_schema_version_for_write(&sink).await?;

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

    // ── Bounded two-pass streaming write (Issue 0002 Bug 2) ──────────────
    // Never holds the whole graph: pass 1 builds a tiny symbol_id→winning_phase
    // index; pass 2 re-scans the shards in record-batches, writing each winning
    // record through a bounded (row + byte) buffer.  Edges are streamed straight
    // from final-edges.parquet (already deduped in Phase 3; sinks drop unresolved
    // rows at the boundary).  The REPO node + inline BELONGS_TO_REPO edges are
    // emitted during the node stream.
    let repo_node = if opts.skip_repo_node {
        None
    } else {
        Some(build_repo_node(
            &opts.repo_name,
            &opts.input_path,
            sink.backend_name(),
            allocator.as_deref(),
        )?)
    };

    let stream_opts = StreamWriteOptions {
        batch_size: crate::config::DEFAULT_BATCH_SIZE,
        write_buffer_bytes: opts.write_buffer_bytes,
    };

    let (nodes_written, edges_written) =
        write_graph_streaming(sink.as_ref(), &stage_dir, repo_node, stream_opts).await?;
    stats.nodes_written = nodes_written;
    stats.edges_written = edges_written;

    info!(
        "Phase 4: complete — {} nodes, {} edges written to '{}'",
        stats.nodes_written,
        stats.edges_written,
        sink.backend_name()
    );

    record_pipeline_metrics(&stats, run_started.elapsed().as_secs_f64());

    Ok(stats)
}

// ---------------------------------------------------------------------------
// Phase 4 — bounded two-pass streaming sink write (Issue 0002 Bug 2)
// ---------------------------------------------------------------------------

/// Tuning for [`write_graph_streaming`]'s bounded in-flight buffer.
#[derive(Debug, Clone, Copy)]
struct StreamWriteOptions {
    /// Flush the buffer when it reaches this many rows.
    batch_size: usize,
    /// Flush the buffer when its accumulated record bytes reach this budget.
    /// `0` disables the byte budget (row-only flushing).
    write_buffer_bytes: usize,
}

/// Construct the REPO [`NodeRecord`] for this run (S22, AC-M4-1).
///
/// Mirrors the pre-streaming inline construction; the integer IDs come from the
/// reopened/active [`SymbolAllocator`] so `--write-only` produces an identical
/// REPO node to a single-shot run.
fn build_repo_node(
    repo_name: &str,
    input_path: &Path,
    backend_name: &str,
    allocator: Option<&crate::resolve::symbol_map::SymbolAllocator>,
) -> Result<NodeRecord> {
    let meta = repo_meta::collect(repo_name, input_path)?;
    let repo_usr = format!("repo:{}", meta.name);

    let attrs = serde_json::json!({
        "root_path": meta.root_path.to_string_lossy(),
        "commit_sha": meta.commit_sha,
        "commit_date": meta.commit_date,
        "sink": backend_name,
    });

    Ok(NodeRecord {
        usr: repo_usr.clone(),
        kind: NodeKind::Repo,
        name: meta.name.clone(),
        qualified_name: meta.name.clone(),
        file_path: meta.root_path.to_string_lossy().into_owned(),
        repo_name: meta.name.clone(),
        attrs_json: attrs.to_string(),
        phase: 0,
        symbol_id: allocator
            .and_then(|a| a.get_or_insert_symbol(&repo_usr).ok())
            .unwrap_or(0),
        file_id: allocator
            .and_then(|a| a.get_or_insert_file(&meta.root_path.to_string_lossy()).ok())
            .unwrap_or(0),
        ..Default::default()
    })
}

/// Build the BELONGS_TO_REPO edge from `node` to the REPO node (S22, AC-M4-2).
fn belongs_to_repo_edge(node: &NodeRecord, repo: &NodeRecord) -> EdgeRecord {
    EdgeRecord {
        src_usr: node.usr.clone(),
        dst_usr: Some(repo.usr.clone()),
        kind: EdgeKind::BelongsToRepo,
        resolved: true,
        repo_name: repo.repo_name.clone(),
        src_id: node.symbol_id,
        dst_id: if repo.symbol_id > 0 {
            Some(repo.symbol_id)
        } else {
            None
        },
        dst_repo_name: repo.repo_name.clone(),
        ..Default::default()
    }
}

/// Approximate the in-memory byte footprint of a [`NodeRecord`] for the
/// byte-budget flush.  Dominated by the `code` snippet (≤32 KiB, M8); the rest
/// is small.  Exact accounting is unnecessary — this only bounds the buffer.
fn node_record_bytes(n: &NodeRecord) -> usize {
    let mut b = std::mem::size_of::<NodeRecord>();
    b += n.usr.len() + n.name.len() + n.qualified_name.len() + n.file_path.len();
    b += n.repo_name.len() + n.attrs_json.len();
    b += n.mangled_name.as_ref().map_or(0, String::len);
    b += n.return_type.as_ref().map_or(0, String::len);
    b += n.signature.as_ref().map_or(0, String::len);
    b += n.code.as_ref().map_or(0, String::len);
    b
}

/// Approximate the in-memory byte footprint of an [`EdgeRecord`].
fn edge_record_bytes(e: &EdgeRecord) -> usize {
    let mut b = std::mem::size_of::<EdgeRecord>();
    b += e.src_usr.len() + e.repo_name.len() + e.attrs_json.len() + e.dst_repo_name.len();
    b += e.dst_usr.as_ref().map_or(0, String::len);
    b
}

/// Pass 1 of the node stream: build the `symbol_id → winning_phase` index over
/// all node shards (phase1 worker shards + phase2 shards).
///
/// Reproduces `load_nodes_from_stage`'s collapse semantics keyed on `symbol_id`
/// (v6): the highest `phase` seen for a `symbol_id` wins, so a decorated
/// phase=2 record supersedes its phase=1 counterpart.  The map is tiny (~one
/// `i64`+`u8` per distinct symbol), so peak memory here is one record batch.
fn build_node_winner_index(stage_dir: &Path) -> Result<std::collections::HashMap<i64, u8>> {
    use std::collections::HashMap;

    let mut winner: HashMap<i64, u8> = HashMap::new();

    let mut record_winners = |path: &Path| -> Result<()> {
        let reader = open_shard_reader(path, false)?;
        for batch_result in reader {
            let batch = batch_result.map_err(|e| Error::Schema(format!("read node batch: {e}")))?;
            // Read full records but keep only (symbol_id, phase); the batch is
            // dropped at the end of each iteration so peak stays at one batch.
            for record in record_batch_to_nodes(&batch) {
                winner
                    .entry(record.symbol_id)
                    .and_modify(|p| {
                        if record.phase > *p {
                            *p = record.phase;
                        }
                    })
                    .or_insert(record.phase);
            }
        }
        Ok(())
    };

    for shard_path in collect_shards(stage_dir, "nodes", MissingDirPolicy::EmptyOnMissing)? {
        record_winners(&shard_path)?;
    }
    for shard_path in collect_phase2_shards(stage_dir)? {
        record_winners(&shard_path)?;
    }

    Ok(winner)
}

/// Bounded two-pass streaming Phase 4 write.
///
/// Writes the optional REPO node first, then streams nodes (emitting one inline
/// BELONGS_TO_REPO edge per non-REPO node), then streams the resolved edges from
/// `final-edges.parquet`.  Peak memory is bounded by `opts.batch_size` rows or
/// `opts.write_buffer_bytes` bytes per in-flight buffer — never the whole graph.
///
/// Returns `(nodes_written, edges_written)` as reported by the sink.
async fn write_graph_streaming(
    sink: &dyn GraphSink,
    stage_dir: &Path,
    repo_node: Option<NodeRecord>,
    opts: StreamWriteOptions,
) -> Result<(u64, u64)> {
    let winner = build_node_winner_index(stage_dir)?;

    let mut nodes_written = 0u64;
    let mut edges_written = 0u64;

    // In-flight buffers (bounded by row count OR byte budget).
    let mut node_buf: Vec<NodeRecord> = Vec::new();
    let mut node_buf_bytes = 0usize;
    let mut edge_buf: Vec<EdgeRecord> = Vec::new();
    let mut edge_buf_bytes = 0usize;
    // Track symbol_ids already emitted so cross-shard duplicates collapse to one.
    let mut emitted: HashSet<i64> = HashSet::new();

    // ── REPO node first (so the nodes that reference it exist on write) ──
    if let Some(repo) = repo_node.as_ref() {
        emitted.insert(repo.symbol_id);
        nodes_written += sink
            .write_nodes(std::slice::from_ref(repo))
            .await?
            .nodes_written;
    }

    // ── Pass 2: stream nodes, emit inline BELONGS_TO_REPO edges ──
    let node_shards = collect_shards(stage_dir, "nodes", MissingDirPolicy::EmptyOnMissing)?
        .into_iter()
        .chain(collect_phase2_shards(stage_dir)?)
        .collect::<Vec<_>>();

    for shard_path in node_shards {
        let reader = open_shard_reader(&shard_path, false)?;
        for batch_result in reader {
            let batch = batch_result.map_err(|e| Error::Schema(format!("read node batch: {e}")))?;
            for record in record_batch_to_nodes(&batch) {
                // Winner selection: emit only the record whose phase matches the
                // winning phase for its symbol_id, and only once (cross-shard
                // collapse).  Defensive: a symbol_id absent from `winner` cannot
                // happen (the index is built from the same shards) but skip if so.
                let Some(&win_phase) = winner.get(&record.symbol_id) else {
                    continue;
                };
                if record.phase != win_phase || !emitted.insert(record.symbol_id) {
                    continue;
                }

                // Inline BELONGS_TO_REPO edge (REPO node already emitted above).
                if let Some(repo) = repo_node.as_ref() {
                    let e = belongs_to_repo_edge(&record, repo);
                    edge_buf_bytes += edge_record_bytes(&e);
                    edge_buf.push(e);
                    if buffer_full(edge_buf.len(), edge_buf_bytes, &opts) {
                        edges_written += flush_edges(sink, &mut edge_buf).await?;
                        edge_buf_bytes = 0;
                    }
                }

                node_buf_bytes += node_record_bytes(&record);
                node_buf.push(record);
                if buffer_full(node_buf.len(), node_buf_bytes, &opts) {
                    nodes_written += flush_nodes(sink, &mut node_buf).await?;
                    node_buf_bytes = 0;
                }
            }
        }
    }
    nodes_written += flush_nodes(sink, &mut node_buf).await?;
    node_buf_bytes = 0;
    let _ = node_buf_bytes;

    // ── Stream resolved edges from final-edges.parquet ──
    // Already deduped in Phase 3; the sink drops unresolved rows at write time.
    let final_path = stage_dir.join("final-edges.parquet");
    if final_path.exists() {
        let reader = open_shard_reader(&final_path, false)?;
        for batch_result in reader {
            let batch = batch_result.map_err(|e| Error::Schema(format!("read edge batch: {e}")))?;
            for edge in record_batch_to_edges(&batch) {
                edge_buf_bytes += edge_record_bytes(&edge);
                edge_buf.push(edge);
                if buffer_full(edge_buf.len(), edge_buf_bytes, &opts) {
                    edges_written += flush_edges(sink, &mut edge_buf).await?;
                    edge_buf_bytes = 0;
                }
            }
        }
    }
    edges_written += flush_edges(sink, &mut edge_buf).await?;

    Ok((nodes_written, edges_written))
}

/// Returns `true` when the buffer should flush: row count at cap OR (byte budget
/// enabled and) accumulated bytes at budget.  Always returns `false` for an
/// empty buffer; the caller never accumulates a zero-length buffer.
fn buffer_full(rows: usize, bytes: usize, opts: &StreamWriteOptions) -> bool {
    if rows == 0 {
        return false;
    }
    if rows >= opts.batch_size {
        return true;
    }
    opts.write_buffer_bytes != 0 && bytes >= opts.write_buffer_bytes
}

/// Flush a node buffer to the sink and clear it.  No-op for an empty buffer.
async fn flush_nodes(sink: &dyn GraphSink, buf: &mut Vec<NodeRecord>) -> Result<u64> {
    if buf.is_empty() {
        return Ok(0);
    }
    let stats = sink.write_nodes(buf).await?;
    buf.clear();
    Ok(stats.nodes_written)
}

/// Flush an edge buffer to the sink and clear it.  No-op for an empty buffer.
async fn flush_edges(sink: &dyn GraphSink, buf: &mut Vec<EdgeRecord>) -> Result<u64> {
    if buf.is_empty() {
        return Ok(0);
    }
    let stats = sink.write_edges(buf).await?;
    buf.clear();
    Ok(stats.nodes_written)
}

fn filter_entries_to_input_scope(
    cc_path: &Path,
    input_path: &Path,
    entries: Vec<TuEntry>,
) -> Result<Vec<TuEntry>> {
    let input = normalize_path(input_path);
    let filtered: Vec<TuEntry> = if input_path.is_file() {
        entries
            .into_iter()
            .filter(|entry| normalize_path(&entry.file) == input)
            .collect()
    } else if input_path.is_dir() {
        entries
            .into_iter()
            .filter(|entry| normalize_path(&entry.file).starts_with(&input))
            .collect()
    } else {
        entries
    };

    if filtered.is_empty() {
        return Err(Error::CompileCommands {
            path: cc_path.to_owned(),
            message: format!(
                "no translation units matched input scope `{}`",
                input_path.display()
            ),
        });
    }

    Ok(filtered)
}

fn normalize_path(path: &Path) -> PathBuf {
    path.canonicalize().unwrap_or_else(|_| path.to_path_buf())
}

fn record_pipeline_metrics(stats: &PipelineStats, elapsed_secs: f64) {
    crate::metrics::cxg_nodes_total().inc_by(stats.nodes_written);
    crate::metrics::cxg_edges_total().inc_by(stats.edges_written);

    let elapsed_secs = elapsed_secs.max(f64::EPSILON);
    crate::metrics::cxg_nodes_per_second().set(stats.nodes_written as f64 / elapsed_secs);
    crate::metrics::cxg_edges_per_second().set(stats.edges_written as f64 / elapsed_secs);

    let cache_hit_ratio = if stats.tu_count == 0 {
        0.0
    } else {
        stats.cache_hits as f64 / stats.tu_count as f64
    };
    crate::metrics::cxg_cache_hit_ratio().set(cache_hit_ratio);
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
    /// Number of TUs with a hard libclang error (AC-4).
    pub failed_tu_count: usize,
    /// Total nodes written to the sink.
    pub nodes_written: u64,
    /// Total edges written to the sink.
    pub edges_written: u64,
}

impl PipelineStats {
    /// Return the canonical closing summary line for `cxg-index` stderr output.
    ///
    /// Format (stable — snapshot-tested in AC-4):
    /// `cxg-index: done — <T> TUs | <P> partial | <F> failed | <N> nodes | <E> edges`
    pub fn closing_summary(&self) -> String {
        format!(
            "cxg-index: done \u{2014} {} TUs | {} partial | {} failed | {} nodes | {} edges",
            self.tu_count,
            self.partial_tu_count,
            self.failed_tu_count,
            self.nodes_written,
            self.edges_written,
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn closing_summary_format() {
        let stats = PipelineStats {
            tu_count: 10,
            partial_tu_count: 2,
            failed_tu_count: 1,
            nodes_written: 100,
            edges_written: 50,
            ..Default::default()
        };
        assert_eq!(
            stats.closing_summary(),
            "cxg-index: done \u{2014} 10 TUs | 2 partial | 1 failed | 100 nodes | 50 edges"
        );
    }

    fn tu_entry(file: PathBuf) -> TuEntry {
        TuEntry {
            hash: blake3::hash(file.as_os_str().as_encoded_bytes()),
            file,
            args: vec!["clang++".to_owned()],
        }
    }

    #[test]
    fn scope_filter_keeps_only_entries_under_directory() {
        let tmp = tempfile::tempdir().unwrap();
        let src = tmp.path().join("src");
        let other = tmp.path().join("other");
        std::fs::create_dir_all(&src).unwrap();
        std::fs::create_dir_all(&other).unwrap();
        let kept = src.join("kept.cpp");
        let skipped = other.join("skipped.cpp");
        std::fs::write(&kept, "int kept;").unwrap();
        std::fs::write(&skipped, "int skipped;").unwrap();

        let filtered = filter_entries_to_input_scope(
            &tmp.path().join("compile_commands.json"),
            &src,
            vec![tu_entry(kept.clone()), tu_entry(skipped)],
        )
        .unwrap();

        assert_eq!(filtered.len(), 1);
        assert_eq!(normalize_path(&filtered[0].file), normalize_path(&kept));
    }

    #[test]
    fn scope_filter_keeps_only_exact_file() {
        let tmp = tempfile::tempdir().unwrap();
        let kept = tmp.path().join("kept.cpp");
        let skipped = tmp.path().join("skipped.cpp");
        std::fs::write(&kept, "int kept;").unwrap();
        std::fs::write(&skipped, "int skipped;").unwrap();

        let filtered = filter_entries_to_input_scope(
            &tmp.path().join("compile_commands.json"),
            &kept,
            vec![tu_entry(kept.clone()), tu_entry(skipped)],
        )
        .unwrap();

        assert_eq!(filtered.len(), 1);
        assert_eq!(normalize_path(&filtered[0].file), normalize_path(&kept));
    }

    #[test]
    fn scope_filter_errors_when_no_entries_match() {
        let tmp = tempfile::tempdir().unwrap();
        let requested = tmp.path().join("requested.cpp");
        let other = tmp.path().join("other.cpp");
        std::fs::write(&requested, "int requested;").unwrap();
        std::fs::write(&other, "int other;").unwrap();

        let err = filter_entries_to_input_scope(
            &tmp.path().join("compile_commands.json"),
            &requested,
            vec![tu_entry(other)],
        )
        .unwrap_err();

        assert!(
            err.to_string().contains("no translation units matched"),
            "unexpected error: {err}"
        );
    }

    // -----------------------------------------------------------------------
    // Bug 2 — bounded two-pass streaming write
    // -----------------------------------------------------------------------

    use crate::schema::arrow::nodes_to_record_batch;
    use crate::sink::mock::{MockCall, MockSink};
    use crate::stage::schema::writer_properties;
    use crate::stage::writer::StageWriter;
    use std::sync::Arc;

    /// Build a `NodeRecord` for tests with the given symbol_id, phase, and an
    /// optional `code` payload (drives the byte-budget flush test).
    fn node(symbol_id: i64, phase: u8, code: Option<String>) -> NodeRecord {
        NodeRecord {
            usr: format!("c:@F@sym{symbol_id}"),
            kind: NodeKind::Function,
            name: format!("sym{symbol_id}"),
            qualified_name: format!("ns::sym{symbol_id}"),
            file_path: "/repo/src/f.cpp".to_owned(),
            line: Some(1),
            col: Some(1),
            repo_name: "repo".to_owned(),
            attrs_json: if phase == 2 {
                "{\"decorated\":true}"
            } else {
                "{}"
            }
            .to_owned(),
            phase,
            code,
            symbol_id,
            file_id: symbol_id,
            ..Default::default()
        }
    }

    /// Write a `phase2-nodes-0.parquet` shard at the stage-dir root.
    fn write_phase2_shard(stage_dir: &Path, records: &[NodeRecord]) {
        let schema = std::sync::Arc::new(crate::schema::arrow::node_schema());
        let batch = nodes_to_record_batch(records).expect("nodes_to_record_batch");
        let path = stage_dir.join("phase2-nodes-0.parquet");
        let file = std::fs::File::create(&path).unwrap();
        let mut writer =
            parquet::arrow::ArrowWriter::try_new(file, schema, Some(writer_properties())).unwrap();
        writer.write(&batch).unwrap();
        writer.close().unwrap();
    }

    #[test]
    fn winner_index_picks_phase2_over_phase1() {
        let dir = tempfile::tempdir().unwrap();
        let stage = dir.path().join("stage");

        // Phase 1 worker shard: symbol 1 (phase 1) + symbol 2 (phase 1).
        let mut w = StageWriter::new(&stage, 0).unwrap();
        w.write_nodes(&[node(1, 1, None), node(2, 1, None)])
            .unwrap();
        w.finish().unwrap();

        // Phase 2 shard: symbol 1 decorated (phase 2).
        write_phase2_shard(&stage, &[node(1, 2, None)]);

        let winner = build_node_winner_index(&stage).unwrap();
        assert_eq!(winner.get(&1), Some(&2), "phase 2 must win for symbol 1");
        assert_eq!(winner.get(&2), Some(&1), "symbol 2 stays phase 1");
    }

    #[tokio::test]
    async fn stream_collapses_cross_shard_duplicate_symbols_to_one_emission() {
        let dir = tempfile::tempdir().unwrap();
        let stage = dir.path().join("stage");

        // The same symbol 1 appears in two worker shards (cross-shard duplicate),
        // plus a phase-2 decorated copy.  Symbol 2 appears once.
        let mut w0 = StageWriter::new(&stage, 0).unwrap();
        w0.write_nodes(&[node(1, 1, None), node(2, 1, None)])
            .unwrap();
        w0.finish().unwrap();
        let mut w1 = StageWriter::new(&stage, 1).unwrap();
        w1.write_nodes(&[node(1, 1, None)]).unwrap();
        w1.finish().unwrap();
        write_phase2_shard(&stage, &[node(1, 2, None)]);

        let sink = Arc::new(MockSink::default());
        let opts = StreamWriteOptions {
            batch_size: crate::config::DEFAULT_BATCH_SIZE,
            write_buffer_bytes: 0,
        };
        let (nodes_written, _edges) = write_graph_streaming(sink.as_ref(), &stage, None, opts)
            .await
            .unwrap();

        // Exactly two distinct symbols emitted, each once.
        assert_eq!(nodes_written, 2, "cross-shard duplicates must collapse");

        // Verify symbol 1 was emitted as the phase-2 (decorated) record.
        let mut emitted: Vec<(i64, u8)> = Vec::new();
        for call in sink.calls() {
            if let MockCall::WriteNodes(batch) = call {
                for n in batch {
                    emitted.push((n.symbol_id, n.phase));
                }
            }
        }
        emitted.sort();
        assert_eq!(
            emitted,
            vec![(1, 2), (2, 1)],
            "symbol 1 must be the phase-2 winner"
        );
    }

    #[tokio::test]
    async fn byte_budget_flushes_before_row_cap() {
        let dir = tempfile::tempdir().unwrap();
        let stage = dir.path().join("stage");

        // 4 nodes, each carrying a ~10 KiB code snippet.  Row cap is huge so it
        // never triggers; the byte budget (~16 KiB) must force a flush roughly
        // every two records.
        let big = "x".repeat(10 * 1024);
        let mut w = StageWriter::new(&stage, 0).unwrap();
        w.write_nodes(&[
            node(1, 1, Some(big.clone())),
            node(2, 1, Some(big.clone())),
            node(3, 1, Some(big.clone())),
            node(4, 1, Some(big)),
        ])
        .unwrap();
        w.finish().unwrap();

        let sink = Arc::new(MockSink::default());
        let opts = StreamWriteOptions {
            batch_size: 1_000_000,         // row cap never reached
            write_buffer_bytes: 16 * 1024, // ~16 KiB byte budget
        };
        let (nodes_written, _edges) = write_graph_streaming(sink.as_ref(), &stage, None, opts)
            .await
            .unwrap();
        assert_eq!(nodes_written, 4);

        // Count node-write calls: with a 16 KiB budget and ~10 KiB records, each
        // buffer admits ~1 record before crossing the budget, so we expect
        // multiple write_nodes calls — definitively more than one (proving the
        // byte budget, not the row cap, drove the flushing).
        let node_write_calls = sink
            .calls()
            .into_iter()
            .filter(|c| matches!(c, MockCall::WriteNodes(_)))
            .count();
        assert!(
            node_write_calls >= 2,
            "byte budget must force multiple flushes; got {node_write_calls}"
        );
    }

    #[tokio::test]
    async fn streaming_emits_repo_node_first_with_belongs_edges() {
        let dir = tempfile::tempdir().unwrap();
        let stage = dir.path().join("stage");

        let mut w = StageWriter::new(&stage, 0).unwrap();
        w.write_nodes(&[node(1, 1, None), node(2, 1, None)])
            .unwrap();
        w.finish().unwrap();

        let repo = node(99, 0, None);
        let mut repo = repo;
        repo.kind = NodeKind::Repo;
        repo.usr = "repo:repo".to_owned();

        let sink = Arc::new(MockSink::default());
        let opts = StreamWriteOptions {
            batch_size: crate::config::DEFAULT_BATCH_SIZE,
            write_buffer_bytes: 0,
        };
        let (nodes_written, edges_written) =
            write_graph_streaming(sink.as_ref(), &stage, Some(repo.clone()), opts)
                .await
                .unwrap();

        // REPO node + 2 symbols = 3 nodes; 2 BELONGS_TO_REPO edges.
        assert_eq!(nodes_written, 3);
        assert_eq!(
            edges_written, 2,
            "one BELONGS_TO_REPO edge per non-repo node"
        );

        // The very first write_nodes call must contain the REPO node only.
        let first_node_call = sink
            .calls()
            .into_iter()
            .find_map(|c| match c {
                MockCall::WriteNodes(b) => Some(b),
                _ => None,
            })
            .expect("at least one write_nodes call");
        assert_eq!(first_node_call.len(), 1, "REPO node written first, alone");
        assert_eq!(first_node_call[0].kind, NodeKind::Repo);
    }
}
