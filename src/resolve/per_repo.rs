/// Phase 3 — in-memory USR-map resolver for a single repo (AC-M1-18, AC-M1-19, AC-M2-13).
///
/// ## Algorithm
///
/// 1. Walk `<stage_dir>/worker-*/nodes-*.parquet` shards and load every node USR into a
///    [`crate::resolve::spill::UsrMap`].  When the map's estimated footprint exceeds the
///    spill threshold (default 8 GiB, AC-M3-12) it transparently migrates to RocksDB under
///    `<stage_dir>/.cxg-cache/usr_map.rocks`.
///
/// 2. Walk `<stage_dir>/worker-*/edges-*.parquet` shards. For each edge record:
///    - `dst_usr = Some(u)`, `u` in the map  → write with `resolved=true`,
///      `cross_repo_candidate=false`.
///    - `dst_usr = Some(u)`, `u` not in map  → write with `resolved=false`,
///      `cross_repo_candidate=true`.
///    - `dst_usr = None` (placeholder edges)  → carry forward unchanged; these are not
///      resolvable at this phase.
///
/// 3. Write the classified edges to `<stage_dir>/final-edges.parquet` using the same
///    Arrow schema and KV-magic writer properties as the input shards (ADR-3).
///
/// ## Spill
///
/// The `UsrMap` wrapper in [`crate::resolve::spill`] manages the HashMap → RocksDB
/// transition transparently.  Callers use `resolve_per_repo` (production default,
/// 8 GiB threshold) or `resolve_per_repo_with_threshold` (tests, injectable threshold).
///
/// ## Single-threaded contract
///
/// Phase 3 is intentionally single-threaded per repo (per design.md §Phase 3).  No rayon
/// or Tokio runtime is required; synchronous Parquet I/O is used throughout.
use std::{
    fs::{self, File},
    path::{Path, PathBuf},
    sync::Arc,
};

use parquet::{
    arrow::ArrowWriter,
    file::reader::{FileReader, SerializedFileReader},
};

use crate::{
    resolve::spill::{UsrMap, DEFAULT_SPILL_THRESHOLD_BYTES},
    schema::{
        arrow::{edge_schema, edges_to_record_batch, record_batch_to_edges, record_batch_to_nodes},
        EdgeRecord, NodeKind,
    },
    stage::schema::{has_magic, writer_properties},
    Error, Result,
};

// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

/// Minimal metadata retained for each node in the in-memory USR map.
///
/// Extended in later stories if Phase 4 / Phase 5 need more fields without
/// a second pass over node shards.
#[derive(Debug, Clone)]
pub struct NodeMeta {
    /// Node kind retained for diagnostics and potential Phase 4 hints.
    pub kind: NodeKind,
    /// Repository name this node came from.
    pub repo_name: String,
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// Run Phase 3 resolution for a single repository using the default 8 GiB
/// spill threshold (AC-M3-12).
///
/// # Parameters
/// - `stage_dir`: The run-level staging directory containing `worker-NNN/` subdirectories
///   produced by Phase 1 (and optionally Phase 2).
///
/// # Returns
/// The absolute path to the written `final-edges.parquet` file inside `stage_dir`.
///
/// # Errors
/// - `Error::Schema` when a shard is missing the ADR-3 KV magic header (version mismatch)
///   or when Arrow/Parquet serialisation fails.
/// - `Error::Cache` when RocksDB spill initialisation or I/O fails.
/// - `Error::Io` on any file-system error.
pub fn resolve_per_repo(stage_dir: &Path) -> Result<PathBuf> {
    resolve_per_repo_with_threshold(stage_dir, DEFAULT_SPILL_THRESHOLD_BYTES)
}

/// Run Phase 3 resolution with an explicit spill threshold.
///
/// Prefer [`resolve_per_repo`] in production.  Use this variant in tests to
/// inject a small threshold that triggers the RocksDB spill path without
/// allocating gigabytes.
pub fn resolve_per_repo_with_threshold(
    stage_dir: &Path,
    spill_threshold: usize,
) -> Result<PathBuf> {
    let node_shards = collect_shards(stage_dir, "nodes")?;
    let edge_shards = collect_shards(stage_dir, "edges")?;

    let usr_map = build_usr_map(&node_shards, stage_dir, spill_threshold)?;
    let output_path = stage_dir.join("final-edges.parquet");
    write_resolved_edges(&edge_shards, &usr_map, &output_path)?;

    Ok(output_path)
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// Collect all `<prefix>-*.parquet` shard paths from every `worker-*/` subdirectory
/// inside `stage_dir`, sorted lexicographically for deterministic ordering.
fn collect_shards(stage_dir: &Path, prefix: &str) -> Result<Vec<PathBuf>> {
    let mut shards = Vec::new();

    let dir_entries = fs::read_dir(stage_dir)?;
    for entry in dir_entries {
        let entry = entry?;
        let path = entry.path();

        // Skip anything that is not a `worker-NNN/` directory.
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

        // Enumerate shard files inside the worker directory.
        let worker_entries = fs::read_dir(&path)?;
        for w_entry in worker_entries {
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

/// Build a [`UsrMap`] from a list of node shard paths.
///
/// Each shard is verified to carry the ADR-3 KV magic before being read.
/// When the map's estimated footprint exceeds `spill_threshold` it spills
/// transparently to RocksDB (AC-M3-12).
///
/// # Errors
/// Returns `Error::Schema` when a shard is missing the magic key or when Parquet
/// deserialisation fails.  Returns `Error::Cache` on RocksDB spill failures.
fn build_usr_map(
    node_shards: &[PathBuf],
    stage_dir: &Path,
    spill_threshold: usize,
) -> Result<UsrMap> {
    let mut map = UsrMap::new(stage_dir, spill_threshold);

    for shard_path in node_shards {
        let file = File::open(shard_path)?;
        let reader = SerializedFileReader::new(file)
            .map_err(|e| Error::Schema(format!("open node shard {}: {e}", shard_path.display())))?;

        let meta = reader.metadata().file_metadata().clone();
        if !has_magic(&meta) {
            return Err(Error::Schema(format!(
                "node shard {} is missing the ADR-3 KV magic header (cxg_parquet_v1); \
                 refusing to read a potentially mismatched-version shard",
                shard_path.display()
            )));
        }

        let arrow_reader = parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder::try_new(
            File::open(shard_path)?,
        )
        .map_err(|e| Error::Schema(format!("build reader for {}: {e}", shard_path.display())))?
        .build()
        .map_err(|e| {
            Error::Schema(format!(
                "build batch reader for {}: {e}",
                shard_path.display()
            ))
        })?;

        for batch_result in arrow_reader {
            let batch = batch_result.map_err(|e| Error::Schema(format!("read node batch: {e}")))?;
            let records = record_batch_to_nodes(&batch);
            for node in records {
                map.insert(
                    node.usr,
                    NodeMeta {
                        kind: node.kind,
                        repo_name: node.repo_name,
                    },
                )?;
            }
        }
    }

    Ok(map)
}

/// Walk edge shards, classify each edge, and write to `output_path`.
///
/// Edge classification rules (§Phase 3 contract):
///
/// | `dst_usr`           | `resolved` | `cross_repo_candidate` |
/// |---------------------|------------|------------------------|
/// | `Some(u)` — in map  | `true`     | `false`                |
/// | `Some(u)` — not in  | `false`    | `true`                 |
/// | `None` (placeholder)| unchanged  | unchanged              |
fn write_resolved_edges(
    edge_shards: &[PathBuf],
    usr_map: &UsrMap,
    output_path: &Path,
) -> Result<()> {
    let schema = Arc::new(edge_schema());
    let props = writer_properties();
    let out_file = File::create(output_path)?;
    let mut writer = ArrowWriter::try_new(out_file, schema, Some(props))
        .map_err(|e| Error::Schema(format!("create final-edges ArrowWriter: {e}")))?;

    for shard_path in edge_shards {
        let file = File::open(shard_path)?;
        let reader = SerializedFileReader::new(file)
            .map_err(|e| Error::Schema(format!("open edge shard {}: {e}", shard_path.display())))?;

        let meta = reader.metadata().file_metadata().clone();
        if !has_magic(&meta) {
            return Err(Error::Schema(format!(
                "edge shard {} is missing the ADR-3 KV magic header (cxg_parquet_v1); \
                 refusing to read a potentially mismatched-version shard",
                shard_path.display()
            )));
        }

        let arrow_reader = parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder::try_new(
            File::open(shard_path)?,
        )
        .map_err(|e| Error::Schema(format!("build reader for {}: {e}", shard_path.display())))?
        .build()
        .map_err(|e| {
            Error::Schema(format!(
                "build batch reader for {}: {e}",
                shard_path.display()
            ))
        })?;

        for batch_result in arrow_reader {
            let batch = batch_result.map_err(|e| Error::Schema(format!("read edge batch: {e}")))?;
            let mut records = record_batch_to_edges(&batch);

            for edge in &mut records {
                classify_edge(edge, usr_map)?;
            }

            if records.is_empty() {
                continue;
            }
            let out_batch = edges_to_record_batch(&records)
                .map_err(|e| Error::Schema(format!("serialise resolved edge batch: {e}")))?;
            writer
                .write(&out_batch)
                .map_err(|e| Error::Schema(format!("write resolved edge batch: {e}")))?;
        }
    }

    writer
        .close()
        .map_err(|e| Error::Schema(format!("close final-edges.parquet: {e}")))?;
    Ok(())
}

/// Classify a single edge in place according to the Phase 3 contract.
#[inline]
fn classify_edge(edge: &mut EdgeRecord, usr_map: &UsrMap) -> Result<()> {
    // Placeholder edges (dst_usr = None): carry forward unchanged.
    if let Some(dst) = &edge.dst_usr {
        if usr_map.contains_key(dst.as_str())? {
            edge.resolved = true;
            edge.cross_repo_candidate = false;
        } else {
            edge.resolved = false;
            edge.cross_repo_candidate = true;
        }
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// Unit tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        schema::{EdgeKind, NodeKind},
        stage::writer::StageWriter,
    };

    fn make_node_meta(kind: NodeKind) -> NodeMeta {
        NodeMeta {
            kind,
            repo_name: "test-repo".to_owned(),
        }
    }

    fn sample_usr_map(stage_dir: &Path) -> UsrMap {
        let mut m = UsrMap::new(stage_dir, DEFAULT_SPILL_THRESHOLD_BYTES);
        m.insert("c:@F@fn_a".to_owned(), make_node_meta(NodeKind::Function))
            .unwrap();
        m.insert("c:@S@ClassB".to_owned(), make_node_meta(NodeKind::Class))
            .unwrap();
        m
    }

    fn make_edge(dst_usr: Option<&str>) -> EdgeRecord {
        EdgeRecord {
            src_usr: "c:@F@src".to_owned(),
            dst_usr: dst_usr.map(str::to_owned),
            dst_placeholder: None,
            kind: EdgeKind::Calls,
            resolved: false,
            cross_repo_candidate: false,
            repo_name: "test-repo".to_owned(),
            attrs_json: "{}".to_owned(),
            tu_hash: [0u8; 32],
            source_association_type: None,
            target_association_type: None,
        }
    }

    #[test]
    fn classify_resolved_when_dst_in_map() {
        let dir = tempfile::tempdir().unwrap();
        let map = sample_usr_map(dir.path());
        let mut edge = make_edge(Some("c:@F@fn_a"));
        classify_edge(&mut edge, &map).unwrap();
        assert!(edge.resolved);
        assert!(!edge.cross_repo_candidate);
    }

    #[test]
    fn classify_cross_repo_when_dst_not_in_map() {
        let dir = tempfile::tempdir().unwrap();
        let map = sample_usr_map(dir.path());
        let mut edge = make_edge(Some("c:@F@unknown"));
        classify_edge(&mut edge, &map).unwrap();
        assert!(!edge.resolved);
        assert!(edge.cross_repo_candidate);
    }

    #[test]
    fn classify_placeholder_unchanged() {
        let dir = tempfile::tempdir().unwrap();
        let map = sample_usr_map(dir.path());
        let mut edge = make_edge(None);
        edge.resolved = false;
        edge.cross_repo_candidate = false;
        classify_edge(&mut edge, &map).unwrap();
        // Placeholder edges must not change.
        assert!(!edge.resolved);
        assert!(!edge.cross_repo_candidate);
    }

    /// Write node + edge shards via `StageWriter`, run `resolve_per_repo`, read back
    /// `final-edges.parquet`, and assert classification counts.
    #[test]
    fn resolve_per_repo_roundtrip() {
        use crate::schema::{EdgeRecord, NodeRecord};

        let dir = tempfile::tempdir().unwrap();
        let stage_dir = dir.path().join("stage");

        // Build 3 nodes with distinct USRs.
        let node_usrs = ["c:@F@fn_0", "c:@F@fn_1", "c:@F@fn_2"];
        let nodes: Vec<NodeRecord> = node_usrs
            .iter()
            .enumerate()
            .map(|(i, &usr)| NodeRecord {
                usr: usr.to_owned(),
                kind: NodeKind::Function,
                name: format!("fn_{i}"),
                qualified_name: format!("ns::fn_{i}"),
                mangled_name: None,
                file_path: format!("/repo/src/file_{i}.cpp"),
                line: Some(u32::try_from(i + 1).unwrap()),
                col: Some(1),
                repo_name: "test-repo".to_owned(),
                attrs_json: "{}".to_owned(),
                partial: false,
                phase: 1,
                tu_hash: [u8::try_from(i).unwrap(); 32],
                return_type: None,
                params: None,
                signature: None,
                code: None,
                code_truncated: None,
                template_params: None,
                template_args: None,
                is_virtual: None,
                is_pure_virtual: None,
                is_static: None,
            })
            .collect();

        // 3 within-repo edges (dst USRs present in node list).
        // 2 unresolved edges (dst USRs absent from nodes).
        let within_repo_edges: Vec<EdgeRecord> = node_usrs
            .iter()
            .enumerate()
            .map(|(i, &dst)| EdgeRecord {
                src_usr: format!("c:@F@caller_{i}"),
                dst_usr: Some(dst.to_owned()),
                dst_placeholder: None,
                kind: EdgeKind::Calls,
                resolved: false,
                cross_repo_candidate: false,
                repo_name: "test-repo".to_owned(),
                attrs_json: "{}".to_owned(),
                tu_hash: [0u8; 32],
                source_association_type: None,
                target_association_type: None,
            })
            .collect();

        let unresolved_edges: Vec<EdgeRecord> = (0..2usize)
            .map(|i| EdgeRecord {
                src_usr: format!("c:@F@caller_ext_{i}"),
                dst_usr: Some(format!("c:@F@external_{i}")),
                dst_placeholder: None,
                kind: EdgeKind::Calls,
                resolved: false,
                cross_repo_candidate: false,
                repo_name: "test-repo".to_owned(),
                attrs_json: "{}".to_owned(),
                tu_hash: [0u8; 32],
                source_association_type: None,
                target_association_type: None,
            })
            .collect();

        let mut all_edges = within_repo_edges;
        all_edges.extend(unresolved_edges);

        // Write via StageWriter so shards have the correct magic + schema.
        let mut writer = StageWriter::new(&stage_dir, 0).unwrap();
        writer.write_nodes(&nodes).unwrap();
        writer.write_edges(&all_edges).unwrap();
        writer.finish().unwrap();

        // Run Phase 3.
        let output = resolve_per_repo(&stage_dir).unwrap();
        assert!(output.exists(), "final-edges.parquet must exist");

        // Read back and classify.
        let file = File::open(&output).unwrap();
        let arrow_reader =
            parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder::try_new(file)
                .unwrap()
                .build()
                .unwrap();

        let mut resolved_count = 0usize;
        let mut cross_repo_count = 0usize;
        let mut total = 0usize;
        for batch in arrow_reader {
            let batch = batch.unwrap();
            let records = record_batch_to_edges(&batch);
            total += records.len();
            for r in &records {
                if r.resolved {
                    resolved_count += 1;
                }
                if r.cross_repo_candidate {
                    cross_repo_count += 1;
                }
            }
        }

        assert_eq!(total, 5, "all 5 edges must be in final-edges.parquet");
        assert_eq!(
            resolved_count, 3,
            "3 within-repo edges must be resolved=true"
        );
        assert_eq!(
            cross_repo_count, 2,
            "2 external edges must be cross_repo_candidate=true"
        );
    }

    /// A missing magic header on a node shard must produce `Error::Schema`.
    #[test]
    fn missing_magic_returns_error() {
        use parquet::{arrow::ArrowWriter, file::properties::WriterProperties};
        use std::sync::Arc;

        let dir = tempfile::tempdir().unwrap();
        let stage_dir = dir.path().join("stage");
        let worker_dir = stage_dir.join("worker-000");
        fs::create_dir_all(&worker_dir).unwrap();

        // Write a node shard WITHOUT the magic KV metadata.
        let schema = Arc::new(crate::schema::arrow::node_schema());
        let props = WriterProperties::builder().build(); // no magic
        let path = worker_dir.join("nodes-0001.parquet");
        let file = File::create(&path).unwrap();
        let writer = ArrowWriter::try_new(file, schema, Some(props)).unwrap();
        writer.close().unwrap();

        let result = resolve_per_repo(&stage_dir);
        assert!(
            matches!(result, Err(Error::Schema(_))),
            "expected Error::Schema for missing magic, got: {result:?}"
        );
    }
}
