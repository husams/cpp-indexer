//! Phase 5 — Cross-repo `EXTERNAL_REF` edge materialisation.
//!
//! Stories: S23-cross-repo-resolver-bin (AC-M4-4, AC-M4-5, AC-M4-6, AC-M4-3, AC-M6-7).
//! ADR: ADR-2 §lock, ADR-9 (schema version refuse).
//!
//! ## Algorithm
//!
//! 1. **Acquire advisory lock** via `GraphSink::acquire_phase5_lock` (AC-M4-6).  A
//!    concurrent invocation will fail with an error.
//! 2. **Schema-version gate** — read `SchemaVersion` from the DB; if the tag
//!    mismatches the current `SCHEMA_VERSION_TAG`, refuse with an error naming
//!    both the expected and actual tag (AC-M6-7).
//! 3. **Mixed-backend gate** — refuse if the `sink` attribute of any two REPO
//!    nodes differs (AC-M4-3).  We infer the backend name from the live sink.
//! 4. **Global USR map** — walk each repo's `worker-*/nodes-*.parquet` shards
//!    to build a `HashMap<Usr, RepoEntry>`.  USRs present in multiple repos are
//!    resolved by first-come-first-served (canonicalisation deferred to S24).
//! 5. **EXTERNAL_REF materialisation** — for each edge in each repo's
//!    `final-edges.parquet` where `cross_repo_candidate=true`:
//!    - Look up `dst_usr` in the global map.
//!    - If found in a *different* repo → write an `EXTERNAL_REF` edge with
//!      `attrs_json = {"via": "<orig_edge_kind>"}` and mark `resolved=true`.
//!    - If not found → retain `resolved=false` (unresolved external reference).
//! 6. **Release lock** explicitly before returning.
//!
//! ## Parquet-based resolution
//!
//! Phase 5 reads from the staged Parquet files produced by Phase 3, not from the
//! DB. This keeps the `GraphSink` trait stable and avoids a trait extension that
//! would affect all backends. Surfaced as a deviation from design.md §Phase 5's
//! "querying the configured DB" phrasing; tagged `sr-dev` in implementation-notes.md.
//!
//! ## Canonicalisation (S24)
//!
//! System-header USR canonicalisation per ADR-4 is implemented in `resolve::canonical`.
//! The global USR map built in step 4 applies `canonical::canonical_repo` to each
//! node's `file_path`; nodes whose path matches a system-header or vendored prefix
//! are attributed to a synthetic `system:*` / `repo:vendored:*` REPO instead of the
//! indexing repo.  Override rules can be passed via `Phase5Options::canonical_overrides`.

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Arc;
use std::time::Duration;

use tracing::{info, warn};

use crate::error::{Error, Result};
use crate::resolve::canonical::{canonical_repo, CanonicalOverride};
use crate::schema::version::SCHEMA_VERSION_TAG;
use crate::schema::{
    arrow::{record_batch_to_edges, record_batch_to_nodes},
    EdgeKind, EdgeRecord, NodeKind,
};
use crate::sink::GraphSink;
use crate::stage::schema::{collect_shards, open_shard_reader, MissingDirPolicy};

// ── Public types ──────────────────────────────────────────────────────────────

/// Options for a Phase 5 cross-repo resolution run.
#[derive(Debug, Clone)]
pub struct Phase5Options {
    /// Stage directories produced by Phase 3 for each indexed repo.
    ///
    /// Each entry must contain `final-edges.parquet` and `worker-*/nodes-*.parquet`
    /// files written by Phase 1 and Phase 3 respectively.
    pub stage_dirs: Vec<PathBuf>,

    /// TTL for the Phase 5 advisory lock (default: 10 minutes).
    pub lock_ttl: Duration,

    /// Maximum records per `write_edges` call (default 10 000).
    pub batch_size: usize,

    /// User-supplied canonical path-prefix overrides (ADR-4 §Rule set, rule 3).
    ///
    /// These are evaluated before the built-in system-header and vendored rules.
    /// An empty list (the default) means only the static ADR-4 rule table is used.
    pub canonical_overrides: Vec<CanonicalOverride>,
}

impl Default for Phase5Options {
    fn default() -> Self {
        Self {
            stage_dirs: Vec::new(),
            lock_ttl: Duration::from_secs(600),
            batch_size: 10_000,
            canonical_overrides: Vec::new(),
        }
    }
}

/// Statistics from a completed Phase 5 run.
#[derive(Debug, Default, Clone)]
pub struct Phase5Stats {
    /// Total cross-repo candidate edges examined.
    pub candidates_examined: u64,
    /// EXTERNAL_REF edges written to the sink.
    pub external_refs_written: u64,
    /// Cross-repo candidates that remained unresolved.
    pub unresolved: u64,
}

impl std::fmt::Display for Phase5Stats {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "candidates={} external_refs={} unresolved={}",
            self.candidates_examined, self.external_refs_written, self.unresolved
        )
    }
}

// ── Entry in the global USR map ────────────────────────────────────────────────

#[derive(Debug, Clone)]
struct UsrEntry {
    /// Canonical REPO name: may be a synthetic `system:*` / `repo:vendored:*` name
    /// produced by ADR-4 canonicalisation, or the original `repo_name` from the node
    /// record when no canonical rule matched.
    repo_name: String,
}

// ── Public entry point ─────────────────────────────────────────────────────────

/// Run Phase 5 cross-repo resolution against `sink`.
///
/// # Errors
///
/// - `Error::Sink` if advisory lock acquisition, schema-version read, or edge
///   writes fail.
/// - `Error::Schema` if any repo's schema version mismatches `SCHEMA_VERSION_TAG`,
///   or if mixed backends are detected.
/// - `Error::Io` / `Error::Schema` on Parquet read failures.
pub async fn run(sink: Arc<dyn GraphSink>, opts: Phase5Options) -> Result<Phase5Stats> {
    // ── 1. Acquire Phase 5 advisory lock ──────────────────────────────────────
    info!("phase5: acquiring advisory lock (ttl={:?})", opts.lock_ttl);
    let mut lock = sink.acquire_phase5_lock(opts.lock_ttl).await?;

    let result = do_phase5(&sink, &opts).await;

    // ── 6. Release lock ────────────────────────────────────────────────────────
    if let Err(e) = lock.release().await {
        warn!("phase5: failed to release advisory lock: {e}");
    }

    result
}

async fn do_phase5(sink: &Arc<dyn GraphSink>, opts: &Phase5Options) -> Result<Phase5Stats> {
    // ── 2. Schema-version gate ─────────────────────────────────────────────────
    check_schema_version(sink).await?;

    // ── 3. Mixed-backend gate ──────────────────────────────────────────────────
    let backend = sink.backend_name();
    let detected_backends = detect_repo_backends(&opts.stage_dirs)?;
    check_backend_homogeneity(backend, &detected_backends)?;

    // ── 4. Build global USR map ────────────────────────────────────────────────
    info!(
        "phase5: building global USR map from {} stage dir(s)",
        opts.stage_dirs.len()
    );
    let global_map = build_global_usr_map(&opts.stage_dirs, &opts.canonical_overrides)?;
    info!("phase5: global USR map has {} entries", global_map.len());

    // ── 5. Materialise EXTERNAL_REF edges ─────────────────────────────────────
    let stats =
        materialise_external_refs(sink, &opts.stage_dirs, &global_map, opts.batch_size).await?;

    info!(
        "phase5: done — {} candidates, {} EXTERNAL_REF written, {} unresolved",
        stats.candidates_examined, stats.external_refs_written, stats.unresolved
    );
    Ok(stats)
}

// ── 2. Schema-version check ────────────────────────────────────────────────────

pub async fn check_schema_version(sink: &Arc<dyn GraphSink>) -> Result<()> {
    check_schema_version_inner(
        sink,
        "phase5: no SchemaVersion node found; skipping version check",
        "phase5: schema version OK",
        |actual| {
            Error::Schema(format!(
                "schema version mismatch: binary expects '{SCHEMA_VERSION_TAG}', \
                 DB reports '{actual}'; re-index with a matching binary"
            ))
        },
    )
    .await
}

/// Write-path schema-version gate (ADR-4, adr-2, S6-SC-01/02).
///
/// Called at the start of Phase 4 before any data is written to the sink.
/// If the existing graph carries a tag that does not match the current binary's
/// `SCHEMA_VERSION_TAG`, the write is refused with an actionable error message
/// that instructs the operator to stop writers, run `reset`, and re-index.
///
/// The `None` branch (fresh DB or post-reset state) is intentionally allowed so
/// that a reset graph (which has no `SchemaVersion` node) and a completely fresh
/// graph both proceed without error.  Auto-migration is explicitly NOT performed
/// (ADR-4 decision pt 1).
pub async fn check_schema_version_for_write(sink: &Arc<dyn GraphSink>) -> Result<()> {
    check_schema_version_inner(
        sink,
        "write-path: no existing SchemaVersion node; proceeding",
        "write-path: schema version OK",
        |actual| {
            Error::Schema(format!(
                "write refused: existing graph has schema tag '{actual}' but this binary \
                 writes '{SCHEMA_VERSION_TAG}'. To upgrade: (1) stop all writers, \
                 (2) run `POST /v1/reset` (or `sink.reset(ResetTarget::All)`), \
                 (3) re-index all repos with this binary. \
                 Auto-migration is not supported (ADR-4)."
            ))
        },
    )
    .await
}

/// Shared implementation for both schema-version gates.
///
/// Reads the schema version from the sink and compares it against
/// `SCHEMA_VERSION_TAG`.  On mismatch, the `make_err` closure is called to
/// build a context-appropriate error message.
async fn check_schema_version_inner(
    sink: &Arc<dyn GraphSink>,
    none_msg: &str,
    ok_msg: &str,
    make_err: impl Fn(&str) -> Error,
) -> Result<()> {
    match sink.read_schema_version().await? {
        None => {
            info!("{none_msg}");
            Ok(())
        }
        Some(actual) => {
            if actual != SCHEMA_VERSION_TAG {
                Err(make_err(&actual))
            } else {
                info!("{ok_msg} ({actual})");
                Ok(())
            }
        }
    }
}

// ── 3. Mixed-backend detection ─────────────────────────────────────────────────

/// Extract the set of distinct `sink` attribute values from REPO-kind node
/// records across all stage dirs.
fn detect_repo_backends(stage_dirs: &[PathBuf]) -> Result<Vec<String>> {
    let mut backends: Vec<String> = Vec::new();

    for stage_dir in stage_dirs {
        let node_shards = collect_shards(stage_dir, "nodes", MissingDirPolicy::EmptyOnMissing)?;
        for shard_path in &node_shards {
            let arrow_reader = open_shard_reader(shard_path, false)?;
            for batch_result in arrow_reader {
                let batch =
                    batch_result.map_err(|e| Error::Schema(format!("record batch read: {e}")))?;
                let nodes = record_batch_to_nodes(&batch);
                for node in nodes {
                    if node.kind == NodeKind::Repo {
                        if let Ok(v) = serde_json::from_str::<serde_json::Value>(&node.attrs_json) {
                            if let Some(s) = v.get("sink").and_then(|s| s.as_str()) {
                                let s = s.to_owned();
                                if !backends.contains(&s) {
                                    backends.push(s);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Ok(backends)
}

pub(crate) fn check_backend_homogeneity(live_backend: &str, detected: &[String]) -> Result<()> {
    let mut distinct: Vec<&str> = detected.iter().map(|s| s.as_str()).collect();
    distinct.sort_unstable();
    distinct.dedup();

    if distinct.len() > 1 {
        return Err(Error::Schema(format!(
            "mixed-backend attribution detected: repos were indexed with sinks {:?}; \
             Phase 5 requires all repos to share the same backend",
            distinct
        )));
    }

    if let Some(&attr_backend) = distinct.first() {
        if attr_backend != live_backend {
            return Err(Error::Schema(format!(
                "sink attribution mismatch: REPO nodes carry sink='{attr_backend}' \
                 but the live sink is '{live_backend}'"
            )));
        }
    }

    Ok(())
}

// ── 4. Build global USR map ────────────────────────────────────────────────────

fn build_global_usr_map(
    stage_dirs: &[PathBuf],
    overrides: &[CanonicalOverride],
) -> Result<HashMap<String, UsrEntry>> {
    let mut map: HashMap<String, UsrEntry> = HashMap::new();

    for stage_dir in stage_dirs {
        let node_shards = collect_shards(stage_dir, "nodes", MissingDirPolicy::EmptyOnMissing)?;
        for shard_path in &node_shards {
            let arrow_reader = open_shard_reader(shard_path, false)?;
            for batch_result in arrow_reader {
                let batch = batch_result.map_err(|e| Error::Schema(format!("batch read: {e}")))?;
                let nodes = record_batch_to_nodes(&batch);
                for node in nodes {
                    // Apply ADR-4 canonicalisation: if the node's file_path matches a
                    // system-header or vendored prefix, attribute it to the canonical
                    // synthetic REPO rather than the indexing repo.
                    let effective_repo =
                        canonical_repo(&node.file_path, overrides).unwrap_or(node.repo_name);

                    // First-indexed-wins for duplicate USRs within the same canonical repo.
                    map.entry(node.usr).or_insert_with(|| UsrEntry {
                        repo_name: effective_repo,
                    });
                }
            }
        }
    }

    Ok(map)
}

// ── 5. Materialise EXTERNAL_REF edges ─────────────────────────────────────────

/// Build a `repo_name → stage_dir` index by reading one REPO-kind node from each stage dir.
fn build_repo_stage_map(stage_dirs: &[PathBuf]) -> HashMap<String, PathBuf> {
    use crate::schema::arrow::record_batch_to_nodes;
    use crate::schema::NodeKind;

    let mut map: HashMap<String, PathBuf> = HashMap::new();
    for stage_dir in stage_dirs {
        let shards = match collect_shards(stage_dir, "nodes", MissingDirPolicy::EmptyOnMissing) {
            Ok(s) => s,
            Err(_) => continue,
        };
        'outer: for shard_path in &shards {
            let arrow_reader = match open_shard_reader(shard_path, false) {
                Ok(r) => r,
                Err(_) => continue,
            };
            for batch_result in arrow_reader.flatten() {
                for node in record_batch_to_nodes(&batch_result) {
                    if node.kind == NodeKind::Repo || !node.repo_name.is_empty() {
                        map.entry(node.repo_name.clone())
                            .or_insert_with(|| stage_dir.clone());
                        if node.kind == NodeKind::Repo {
                            break 'outer;
                        }
                    }
                }
            }
        }
    }
    map
}

async fn materialise_external_refs(
    sink: &Arc<dyn GraphSink>,
    stage_dirs: &[PathBuf],
    global_map: &HashMap<String, UsrEntry>,
    batch_size: usize,
) -> Result<Phase5Stats> {
    use crate::resolve::symbol_map::SymbolAllocator;
    use std::collections::hash_map::Entry;

    // Build repo_name → stage_dir index for ID resolution.
    let repo_stage_map = build_repo_stage_map(stage_dirs);
    // Cache of opened read-only handles: repo_name → ReadOnlyHandle.
    let mut handle_cache: HashMap<String, crate::resolve::symbol_map::ReadOnlyHandle> =
        HashMap::new();

    let mut stats = Phase5Stats::default();
    let mut pending: Vec<EdgeRecord> = Vec::with_capacity(batch_size);

    for stage_dir in stage_dirs {
        let final_edges_path = stage_dir.join("final-edges.parquet");
        if !final_edges_path.exists() {
            warn!(
                "phase5: no final-edges.parquet in {:?}; skipping",
                stage_dir
            );
            continue;
        }

        let arrow_reader = open_shard_reader(&final_edges_path, false)?;

        for batch_result in arrow_reader {
            let batch = batch_result.map_err(|e| Error::Schema(format!("batch read: {e}")))?;
            let edges = record_batch_to_edges(&batch);

            for edge in edges {
                if !edge.cross_repo_candidate {
                    continue;
                }
                stats.candidates_examined += 1;

                let dst_usr = match &edge.dst_usr {
                    Some(u) => u,
                    None => {
                        stats.unresolved += 1;
                        continue;
                    }
                };

                // Closure to build an EXTERNAL_REF EdgeRecord for this candidate.
                // M8: Mirrors USES classification onto EXTERNAL_REF (ADR-13, S43).
                let make_ref = |src_id: i64,
                                dst_id: Option<i64>,
                                dst_repo_name: String,
                                dst_usr: &str,
                                via_json: String| {
                    EdgeRecord {
                        src_usr: edge.src_usr.clone(),
                        dst_usr: Some(dst_usr.to_owned()),
                        kind: EdgeKind::ExternalRef,
                        resolved: true,
                        repo_name: edge.repo_name.clone(),
                        attrs_json: via_json,
                        tu_hash: edge.tu_hash,
                        source_association_type: edge.source_association_type.clone(),
                        target_association_type: edge.target_association_type.clone(),
                        src_id,
                        dst_id,
                        dst_repo_name,
                        ..Default::default()
                    }
                };

                match global_map.get(dst_usr.as_str()) {
                    Some(entry) if entry.repo_name != edge.repo_name => {
                        // Found in a different repo — emit EXTERNAL_REF.
                        let dst_repo = entry.repo_name.clone();

                        // Resolve integer IDs via per-repo SQLite maps.
                        // src_id: source repo (current stage_dir).
                        // dst_id: destination repo (entry.repo_name's stage_dir).
                        let src_id = {
                            let src_repo = &edge.repo_name;
                            let h = match handle_cache.entry(src_repo.clone()) {
                                Entry::Occupied(o) => o.into_mut(),
                                Entry::Vacant(v) => {
                                    let db_path = repo_stage_map
                                        .get(src_repo)
                                        .map(|d| d.join("cxg-symbols.db"))
                                        .unwrap_or_else(|| stage_dir.join("cxg-symbols.db"));
                                    match SymbolAllocator::open_readonly(&db_path, 1000) {
                                        Ok(h) => v.insert(h),
                                        Err(_) => {
                                            // SQLite unavailable for this repo — emit with src_id=0.
                                            let via_json =
                                                format!(r#"{{"via":"{}"}}"#, edge.kind.as_str());
                                            pending.push(make_ref(
                                                0,
                                                None,
                                                dst_repo.clone(),
                                                dst_usr,
                                                via_json,
                                            ));
                                            stats.external_refs_written += 1;
                                            continue;
                                        }
                                    }
                                }
                            };
                            h.lookup_symbol(&edge.src_usr).unwrap_or(None).unwrap_or(0)
                        };

                        let dst_id = {
                            let h = match handle_cache.entry(dst_repo.clone()) {
                                Entry::Occupied(o) => o.into_mut(),
                                Entry::Vacant(v) => {
                                    let db_path = repo_stage_map
                                        .get(&dst_repo)
                                        .map(|d| d.join("cxg-symbols.db"))
                                        .unwrap_or_else(|| {
                                            // Synthetic repo (system:*) — no db.
                                            PathBuf::from("/dev/null")
                                        });
                                    match SymbolAllocator::open_readonly(&db_path, 1000) {
                                        Ok(h) => v.insert(h),
                                        Err(_) => {
                                            // Destination db unavailable — emit with dst_id=None.
                                            let via_json =
                                                format!(r#"{{"via":"{}"}}"#, edge.kind.as_str());
                                            pending.push(make_ref(
                                                src_id,
                                                None,
                                                dst_repo.clone(),
                                                dst_usr,
                                                via_json,
                                            ));
                                            stats.external_refs_written += 1;
                                            continue;
                                        }
                                    }
                                }
                            };
                            h.lookup_symbol(dst_usr).unwrap_or(None)
                        };

                        let via_json = format!(r#"{{"via":"{}"}}"#, edge.kind.as_str());
                        pending.push(make_ref(src_id, dst_id, dst_repo, dst_usr, via_json));
                        stats.external_refs_written += 1;
                    }
                    Some(_) => {
                        // Found in the same repo — should have been Phase 3 resolved.
                        stats.unresolved += 1;
                    }
                    None => {
                        stats.unresolved += 1;
                    }
                }

                if pending.len() >= batch_size {
                    sink.write_edges(&pending).await?;
                    pending.clear();
                }
            }
        }
    }

    // Flush remainder.
    if !pending.is_empty() {
        sink.write_edges(&pending).await?;
    }

    Ok(stats)
}

// ── Unit tests ─────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn phase5_stats_display() {
        let s = Phase5Stats {
            candidates_examined: 10,
            external_refs_written: 3,
            unresolved: 7,
        };
        let out = s.to_string();
        assert!(out.contains("candidates=10"));
        assert!(out.contains("external_refs=3"));
        assert!(out.contains("unresolved=7"));
    }

    #[test]
    fn collect_shards_empty_dir_returns_empty() {
        let tmp = tempfile::tempdir().expect("tempdir");
        let result = collect_shards(tmp.path(), "nodes", MissingDirPolicy::EmptyOnMissing)
            .expect("no error on empty dir");
        assert!(result.is_empty());
    }

    #[test]
    fn collect_shards_ignores_non_worker_dirs() {
        let tmp = tempfile::tempdir().expect("tempdir");
        let other = tmp.path().join("output");
        std::fs::create_dir_all(&other).unwrap();
        std::fs::write(other.join("nodes-0.parquet"), b"x").unwrap();
        let result = collect_shards(tmp.path(), "nodes", MissingDirPolicy::EmptyOnMissing)
            .expect("no error");
        assert!(result.is_empty(), "non-worker dirs must be ignored");
    }

    #[test]
    fn collect_shards_finds_worker_shards() {
        let tmp = tempfile::tempdir().expect("tempdir");
        let worker = tmp.path().join("worker-000");
        std::fs::create_dir_all(&worker).unwrap();
        std::fs::write(worker.join("nodes-0.parquet"), b"x").unwrap();
        std::fs::write(worker.join("edges-0.parquet"), b"y").unwrap();
        std::fs::write(worker.join("other.txt"), b"z").unwrap();
        let nodes =
            collect_shards(tmp.path(), "nodes", MissingDirPolicy::EmptyOnMissing).expect("ok");
        assert_eq!(nodes.len(), 1);
        assert!(nodes[0].to_str().unwrap().contains("nodes-0.parquet"));
    }

    #[test]
    fn check_backend_homogeneity_single_backend_ok() {
        let detected = vec!["neo4j".to_owned()];
        assert!(check_backend_homogeneity("neo4j", &detected).is_ok());
    }

    #[test]
    fn check_backend_homogeneity_mixed_fails() {
        let detected = vec!["neo4j".to_owned(), "indradb".to_owned()];
        let err = check_backend_homogeneity("neo4j", &detected).expect_err("must fail");
        let msg = err.to_string();
        assert!(msg.contains("mixed-backend"), "got: {msg}");
    }

    #[test]
    fn check_backend_homogeneity_attr_mismatch_fails() {
        let detected = vec!["indradb".to_owned()];
        let err = check_backend_homogeneity("neo4j", &detected).expect_err("must fail");
        let msg = err.to_string();
        assert!(msg.contains("mismatch"), "got: {msg}");
    }

    #[test]
    fn check_backend_homogeneity_no_attrs_ok() {
        // Empty detected list → no REPO nodes had a sink attr → skip check.
        assert!(check_backend_homogeneity("neo4j", &[]).is_ok());
    }

    #[tokio::test]
    async fn schema_version_none_is_ok() {
        use crate::sink::mock::MockSink;
        let sink: Arc<dyn GraphSink> = Arc::new(MockSink::default());
        // MockSink returns None for read_schema_version → must be OK.
        assert!(check_schema_version(&sink).await.is_ok());
    }

    // ── Story 5: write-path version gate tests ────────────────────────────

    /// S6-SC-01: write-path gate with a fresh DB (None) must succeed.
    #[tokio::test]
    async fn write_path_version_gate_none_is_ok() {
        use crate::sink::mock::MockSink;
        let sink: Arc<dyn GraphSink> = Arc::new(MockSink::default());
        // MockSink returns None for read_schema_version (no version written yet).
        assert!(
            check_schema_version_for_write(&sink).await.is_ok(),
            "fresh DB must pass the write-path gate"
        );
    }

    /// S6-SC-01: write-path gate matching the current tag must succeed.
    #[tokio::test]
    async fn write_path_version_gate_matching_tag_is_ok() {
        use crate::sink::mock::MockSink;
        let sink = Arc::new(MockSink::default());
        // Write the current tag so read_schema_version returns it.
        sink.write_schema_version(SCHEMA_VERSION_TAG, "{}")
            .await
            .unwrap();
        let sink: Arc<dyn GraphSink> = sink;
        assert!(
            check_schema_version_for_write(&sink).await.is_ok(),
            "matching schema version must pass the write-path gate"
        );
    }

    /// S6-SC-01: write-path gate with a stale tag must return Error::Schema
    /// naming both the expected and actual tags.
    #[tokio::test]
    async fn write_path_version_gate_stale_tag_is_refused() {
        use crate::error::Result as CxgResult;
        use crate::schema::{EdgeRecord, NodeRecord};
        use crate::sink::lock::Phase5LockGuard;
        use crate::sink::{HealthInfo, ResetTarget, WriteStats};
        use async_trait::async_trait;

        struct StaleSink;

        #[async_trait]
        impl GraphSink for StaleSink {
            fn backend_name(&self) -> &'static str {
                "mock-stale"
            }
            async fn preflight(&self) -> CxgResult<()> {
                Ok(())
            }
            async fn ensure_indexes(&self) -> CxgResult<()> {
                Ok(())
            }
            async fn write_nodes(&self, _b: &[NodeRecord]) -> CxgResult<WriteStats> {
                unimplemented!()
            }
            async fn write_edges(&self, _b: &[EdgeRecord]) -> CxgResult<WriteStats> {
                unimplemented!()
            }
            async fn reset(&self, _t: ResetTarget) -> CxgResult<()> {
                Ok(())
            }
            async fn acquire_phase5_lock(
                &self,
                _ttl: std::time::Duration,
            ) -> CxgResult<Box<dyn Phase5LockGuard>> {
                unimplemented!()
            }
            async fn read_schema_version(&self) -> CxgResult<Option<String>> {
                // Simulates a v5 graph on disk.
                Ok(Some("cxg-schema-v5".to_owned()))
            }
            async fn write_schema_version(&self, _tag: &str, _attrs: &str) -> CxgResult<()> {
                Ok(())
            }
            async fn health(&self) -> CxgResult<HealthInfo> {
                Ok(HealthInfo {
                    status: "ok".to_owned(),
                    latency: std::time::Duration::ZERO,
                })
            }
        }

        let sink: Arc<dyn GraphSink> = Arc::new(StaleSink);
        let err = check_schema_version_for_write(&sink)
            .await
            .expect_err("stale tag must be refused");
        let msg = err.to_string();
        assert!(
            msg.contains("cxg-schema-v5"),
            "error must name the actual (stale) tag; got: {msg}"
        );
        assert!(
            msg.contains(SCHEMA_VERSION_TAG),
            "error must name the expected tag; got: {msg}"
        );
        assert!(
            msg.contains("reset") || msg.contains("re-index"),
            "error must instruct operator to reset/re-index; got: {msg}"
        );
    }

    /// S6-SC-02: the write path errors rather than silently overwriting when no reset
    /// has been performed (stale graph is not auto-migrated).
    #[tokio::test]
    async fn write_path_errors_without_reset_not_auto_migrated() {
        use crate::error::Result as CxgResult;
        use crate::schema::{EdgeRecord, NodeRecord};
        use crate::sink::lock::Phase5LockGuard;
        use crate::sink::{HealthInfo, ResetTarget, WriteStats};
        use async_trait::async_trait;
        use std::sync::{Arc as StdArc, Mutex};

        struct TrackedSink {
            write_called: Mutex<bool>,
        }

        #[async_trait]
        impl GraphSink for TrackedSink {
            fn backend_name(&self) -> &'static str {
                "mock-tracked"
            }
            async fn preflight(&self) -> CxgResult<()> {
                Ok(())
            }
            async fn ensure_indexes(&self) -> CxgResult<()> {
                Ok(())
            }
            async fn write_nodes(&self, _b: &[NodeRecord]) -> CxgResult<WriteStats> {
                *self.write_called.lock().unwrap() = true;
                Ok(WriteStats {
                    nodes_written: 0,
                    retries: 0,
                    elapsed: std::time::Duration::ZERO,
                })
            }
            async fn write_edges(&self, _b: &[EdgeRecord]) -> CxgResult<WriteStats> {
                *self.write_called.lock().unwrap() = true;
                Ok(WriteStats {
                    nodes_written: 0,
                    retries: 0,
                    elapsed: std::time::Duration::ZERO,
                })
            }
            async fn reset(&self, _t: ResetTarget) -> CxgResult<()> {
                Ok(())
            }
            async fn acquire_phase5_lock(
                &self,
                _ttl: std::time::Duration,
            ) -> CxgResult<Box<dyn Phase5LockGuard>> {
                unimplemented!()
            }
            async fn read_schema_version(&self) -> CxgResult<Option<String>> {
                Ok(Some("cxg-schema-v5".to_owned()))
            }
            async fn write_schema_version(&self, _tag: &str, _attrs: &str) -> CxgResult<()> {
                *self.write_called.lock().unwrap() = true;
                Ok(())
            }
            async fn health(&self) -> CxgResult<HealthInfo> {
                Ok(HealthInfo {
                    status: "ok".to_owned(),
                    latency: std::time::Duration::ZERO,
                })
            }
        }

        let inner = StdArc::new(TrackedSink {
            write_called: Mutex::new(false),
        });
        let sink: Arc<dyn GraphSink> = inner.clone();

        // The write-path gate must refuse before any write occurs.
        let result = check_schema_version_for_write(&sink).await;
        assert!(result.is_err(), "stale v5 graph must be refused");
        assert!(
            !*inner.write_called.lock().unwrap(),
            "no write must have occurred before the gate is cleared"
        );
    }
}
