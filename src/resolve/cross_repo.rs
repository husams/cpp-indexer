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
//! ## Canonicalisation (deferred)
//!
//! System-header USR canonicalisation per ADR-4 is implemented in S24
//! (`resolve::canonical`).  S23 emits `EXTERNAL_REF` edges using raw USRs.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Duration;

use parquet::file::reader::SerializedFileReader;
use tracing::{info, warn};

use crate::error::{Error, Result};
use crate::schema::version::SCHEMA_VERSION_TAG;
use crate::schema::{
    arrow::{record_batch_to_edges, record_batch_to_nodes},
    EdgeKind, EdgeRecord, NodeKind,
};
use crate::sink::GraphSink;

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
}

impl Default for Phase5Options {
    fn default() -> Self {
        Self {
            stage_dirs: Vec::new(),
            lock_ttl: Duration::from_secs(600),
            batch_size: 10_000,
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
    let global_map = build_global_usr_map(&opts.stage_dirs)?;
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

pub(crate) async fn check_schema_version(sink: &Arc<dyn GraphSink>) -> Result<()> {
    match sink.read_schema_version().await? {
        None => {
            // No SchemaVersion node yet (fresh DB or not yet written by Phase 4) → OK.
            info!("phase5: no SchemaVersion node found; skipping version check");
            Ok(())
        }
        Some(actual) => {
            if actual != SCHEMA_VERSION_TAG {
                Err(Error::Schema(format!(
                    "schema version mismatch: binary expects '{SCHEMA_VERSION_TAG}', \
                     DB reports '{actual}'; re-index with a matching binary"
                )))
            } else {
                info!("phase5: schema version OK ({actual})");
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
        let node_shards = collect_shards(stage_dir, "nodes")?;
        for shard_path in &node_shards {
            let file = std::fs::File::open(shard_path)?;
            let reader = SerializedFileReader::new(file)
                .map_err(|e| Error::Schema(format!("Parquet open: {e}")))?;

            let arrow_reader =
                parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder::try_new(
                    std::fs::File::open(shard_path)?,
                )
                .map_err(|e| Error::Schema(format!("build reader: {e}")))?
                .build()
                .map_err(|e| Error::Schema(format!("build batch reader: {e}")))?;

            drop(reader);

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

fn build_global_usr_map(stage_dirs: &[PathBuf]) -> Result<HashMap<String, UsrEntry>> {
    let mut map: HashMap<String, UsrEntry> = HashMap::new();

    for stage_dir in stage_dirs {
        let node_shards = collect_shards(stage_dir, "nodes")?;
        for shard_path in &node_shards {
            let arrow_reader =
                parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder::try_new(
                    std::fs::File::open(shard_path)?,
                )
                .map_err(|e| Error::Schema(format!("build reader: {e}")))?
                .build()
                .map_err(|e| Error::Schema(format!("build batch reader: {e}")))?;

            for batch_result in arrow_reader {
                let batch = batch_result.map_err(|e| Error::Schema(format!("batch read: {e}")))?;
                let nodes = record_batch_to_nodes(&batch);
                for node in nodes {
                    // First-indexed-wins for duplicate USRs (canonicalisation in S24).
                    map.entry(node.usr).or_insert_with(|| UsrEntry {
                        repo_name: node.repo_name,
                    });
                }
            }
        }
    }

    Ok(map)
}

// ── 5. Materialise EXTERNAL_REF edges ─────────────────────────────────────────

async fn materialise_external_refs(
    sink: &Arc<dyn GraphSink>,
    stage_dirs: &[PathBuf],
    global_map: &HashMap<String, UsrEntry>,
    batch_size: usize,
) -> Result<Phase5Stats> {
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

        let arrow_reader = parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder::try_new(
            std::fs::File::open(&final_edges_path)?,
        )
        .map_err(|e| Error::Schema(format!("build reader: {e}")))?
        .build()
        .map_err(|e| Error::Schema(format!("build batch reader: {e}")))?;

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

                match global_map.get(dst_usr.as_str()) {
                    Some(entry) if entry.repo_name != edge.repo_name => {
                        // Found in a different repo — emit EXTERNAL_REF.
                        let via_json = format!(r#"{{"via":"{}"}}"#, edge.kind.as_str());
                        pending.push(EdgeRecord {
                            src_usr: edge.src_usr.clone(),
                            dst_usr: Some(dst_usr.clone()),
                            dst_placeholder: None,
                            kind: EdgeKind::ExternalRef,
                            resolved: true,
                            cross_repo_candidate: false,
                            repo_name: edge.repo_name.clone(),
                            attrs_json: via_json,
                            tu_hash: edge.tu_hash,
                        });
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

// ── Shard collection helpers ───────────────────────────────────────────────────

/// Collect `worker-*/nodes-*.parquet` or `worker-*/edges-*.parquet` shards.
pub(crate) fn collect_shards(stage_dir: &Path, prefix: &str) -> Result<Vec<PathBuf>> {
    let mut shards = Vec::new();

    if !stage_dir.is_dir() {
        return Ok(shards);
    }

    for entry in std::fs::read_dir(stage_dir)? {
        let entry = entry?;
        let path = entry.path();
        if !path.is_dir() {
            continue;
        }
        let dir_name = path
            .file_name()
            .and_then(|n| n.to_str())
            .unwrap_or_default();
        if !dir_name.starts_with("worker-") {
            continue;
        }
        for w_entry in std::fs::read_dir(&path)? {
            let w_entry = w_entry?;
            let shard_path = w_entry.path();
            let file_name = shard_path
                .file_name()
                .and_then(|n| n.to_str())
                .unwrap_or_default();
            if file_name.starts_with(prefix) && file_name.ends_with(".parquet") {
                shards.push(shard_path);
            }
        }
    }

    shards.sort();
    Ok(shards)
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
        let result = collect_shards(tmp.path(), "nodes").expect("no error on empty dir");
        assert!(result.is_empty());
    }

    #[test]
    fn collect_shards_ignores_non_worker_dirs() {
        let tmp = tempfile::tempdir().expect("tempdir");
        let other = tmp.path().join("output");
        std::fs::create_dir_all(&other).unwrap();
        std::fs::write(other.join("nodes-0.parquet"), b"x").unwrap();
        let result = collect_shards(tmp.path(), "nodes").expect("no error");
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
        let nodes = collect_shards(tmp.path(), "nodes").expect("ok");
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
}
