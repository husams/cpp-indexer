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
    fs::File,
    path::{Path, PathBuf},
    sync::Arc,
};

use parquet::arrow::ArrowWriter;

use crate::{
    resolve::spill::{UsrMap, DEFAULT_SPILL_THRESHOLD_BYTES},
    schema::{
        arrow::{edge_schema, edges_to_record_batch, record_batch_to_edges, record_batch_to_nodes},
        EdgeRecord, NodeKind,
    },
    stage::schema::{collect_shards, open_shard_reader, writer_properties, MissingDirPolicy},
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
    let node_shards = collect_shards(stage_dir, "nodes", MissingDirPolicy::ErrorOnMissing)?;
    let edge_shards = collect_shards(stage_dir, "edges", MissingDirPolicy::ErrorOnMissing)?;

    let usr_map = build_usr_map(&node_shards, stage_dir, spill_threshold)?;
    let output_path = stage_dir.join("final-edges.parquet");
    write_resolved_edges(&edge_shards, &usr_map, &output_path)?;

    Ok(output_path)
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

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
        let arrow_reader = open_shard_reader(shard_path, true)?;

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

/// Walk edge shards, classify each edge, deduplicate duplicate sink keys, and
/// write to `output_path`.
///
/// Edge classification rules (§Phase 3 contract):
///
/// | `dst_usr`           | `resolved` | `cross_repo_candidate` |
/// |---------------------|------------|------------------------|
/// | `Some(u)` — in map  | `true`     | `false`                |
/// | `Some(u)` — not in  | `false`    | `true`                 |
/// | `None` (placeholder)| unchanged  | unchanged              |
///
/// # Sink-key dedup (Issue 0002 Bug 2a)
///
/// The same edge is emitted in every TU shard that observed it, so without dedup
/// the persisted file (and the downstream sink write) would carry millions of
/// duplicate rows.  We collapse rows with the same sink key to a single row here,
/// so Phase 4 needs zero in-memory edge dedup.  This replaces the old
/// `dedupe_edges_for_sink` pass that ran in Phase 4.
///
/// Phase 3 processes a **single repo** (see `resolve_per_repo` API contract and
/// `build_usr_map`); all edges share the same `repo_name`, so it is excluded
/// from the dedup key — including it added per-row heap allocation with zero
/// additional selectivity.
///
/// # Dedup key strategy
///
/// - **ID path** (`src_id != 0 && dst_id != None && dst_id != 0`): use
///   `(src_id, dst_id, kind)` — three integer copies, no String allocation.
/// - **Fallback path** (either ID is zero / unresolved): use
///   `(src_usr, dst_usr, kind)` as owned strings.  This path fires only for
///   edges where symbol-ID assignment has not yet been performed.
///
/// Unresolved rows are **kept**: rows with `dst_usr == None` (placeholders) are
/// passed through unchanged, and `cross_repo_candidate=true` rows (which still
/// carry a `dst_usr`) survive dedup like any other keyed row.  Phase 5
/// cross-repo resolution reads exactly those unresolved rows from
/// `final-edges.parquet`, so stripping them here would silently break it.  The
/// sink drops unresolved rows at write time, so keeping them in the file costs
/// nothing for Phase 4.
fn write_resolved_edges(
    edge_shards: &[PathBuf],
    usr_map: &UsrMap,
    output_path: &Path,
) -> Result<()> {
    use crate::schema::EdgeKind;
    use std::collections::HashSet;

    let schema = Arc::new(edge_schema());
    let props = writer_properties();
    let out_file = File::create(output_path)?;
    let mut writer = ArrowWriter::try_new(out_file, schema, Some(props))
        .map_err(|e| Error::Schema(format!("create final-edges ArrowWriter: {e}")))?;

    // Integer-keyed dedup: covers the common case where both IDs are assigned.
    // `EdgeKind` is `Copy + Hash + Eq` so it serves as a zero-cost discriminant.
    let mut seen_ids: HashSet<(i64, i64, EdgeKind)> = HashSet::new();
    // String-keyed fallback: only used when src_id == 0 or dst_id is None/0.
    let mut seen_str: HashSet<(String, String, EdgeKind)> = HashSet::new();

    for shard_path in edge_shards {
        let arrow_reader = open_shard_reader(shard_path, true)?;

        for batch_result in arrow_reader {
            let batch = batch_result.map_err(|e| Error::Schema(format!("read edge batch: {e}")))?;
            let mut records = record_batch_to_edges(&batch);

            for edge in &mut records {
                classify_edge(edge, usr_map)?;
            }

            // Collapse duplicate sink keys.  Keyed rows (dst_usr = Some) are
            // deduped using the integer-ID path when both IDs are present, or
            // the string-USR fallback otherwise.  Placeholder rows (dst_usr =
            // None) are passed through unchanged.
            records.retain(|edge| {
                let Some(_dst_usr) = edge.dst_usr.as_ref() else {
                    // Placeholder edge — always keep.
                    return true;
                };
                let dst_id_nonzero = edge.dst_id.map(|id| id != 0).unwrap_or(false);
                if edge.src_id != 0 && dst_id_nonzero {
                    // Fast path: dedup on integer IDs.
                    seen_ids.insert((edge.src_id, edge.dst_id.unwrap(), edge.kind))
                } else {
                    // Fallback: dedup on USR strings (no repo_name — single-repo phase).
                    seen_str.insert((edge.src_usr.clone(), _dst_usr.clone(), edge.kind))
                }
            });

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
        schema::{EdgeKind, EdgeRecord, NodeKind},
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
            src_id: 0,
            dst_id: None,
            dst_repo_name: "test-repo".to_owned(),
            access: None,
            edge_index: None,
            inherits_is_virtual: None,
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
                symbol_id: i as i64 + 1,
                file_id: i as i64 + 1,
                is_const: None,
                is_constexpr: None,
                storage_class: None,
                is_template: None,
                is_noexcept: None,
                is_override: None,
                is_deleted: None,
                is_defaulted: None,
                cv_qualifiers: None,
                ref_qualifier: None,
                is_final: None,
                is_abstract: None,
                record_kind: None,
                type_spelling: None,
                param_index: None,
                param_kind: None,
                enum_value: None,
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
                src_id: 0,
                dst_id: None,
                dst_repo_name: "test-repo".to_owned(),
                access: None,
                edge_index: None,
                inherits_is_virtual: None,
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
                src_id: 0,
                dst_id: None,
                dst_repo_name: "test-repo".to_owned(),
                access: None,
                edge_index: None,
                inherits_is_virtual: None,
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

    /// Phase 3 sink-key dedup (Issue 0002 Bug 2a): the same edge emitted in
    /// multiple TU shards collapses to one row in `final-edges.parquet`, while
    /// unresolved/cross-repo rows are retained for Phase 5.
    ///
    /// This replaces the old Phase-4 `dedupe_edges_for_sink` assertions.
    #[test]
    fn dedup_collapses_duplicate_sink_keys_and_keeps_unresolved() {
        use crate::schema::{EdgeKind, EdgeRecord, NodeRecord};

        let dir = tempfile::tempdir().unwrap();
        let stage_dir = dir.path().join("stage");

        // One in-repo target node.
        let node = NodeRecord {
            usr: "c:@F@target".to_owned(),
            kind: NodeKind::Function,
            name: "target".to_owned(),
            qualified_name: "target".to_owned(),
            mangled_name: None,
            file_path: "/repo/src/t.cpp".to_owned(),
            line: Some(1),
            col: Some(1),
            repo_name: "repo".to_owned(),
            attrs_json: "{}".to_owned(),
            partial: false,
            phase: 1,
            tu_hash: [0u8; 32],
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
            symbol_id: 1,
            file_id: 1,
            is_const: None,
            is_constexpr: None,
            storage_class: None,
            is_template: None,
            is_noexcept: None,
            is_override: None,
            is_deleted: None,
            is_defaulted: None,
            cv_qualifiers: None,
            ref_qualifier: None,
            is_final: None,
            is_abstract: None,
            record_kind: None,
            type_spelling: None,
            param_index: None,
            param_kind: None,
            enum_value: None,
        };

        let dup_edge = |kind: EdgeKind| EdgeRecord {
            src_usr: "c:@F@caller".to_owned(),
            dst_usr: Some("c:@F@target".to_owned()),
            dst_placeholder: None,
            kind,
            resolved: false,
            cross_repo_candidate: false,
            repo_name: "repo".to_owned(),
            attrs_json: "{}".to_owned(),
            tu_hash: [0u8; 32],
            source_association_type: None,
            target_association_type: None,
            src_id: 2,
            dst_id: None,
            dst_repo_name: "repo".to_owned(),
            access: None,
            edge_index: None,
            inherits_is_virtual: None,
        };
        // An edge to an external (not-in-map) symbol → cross_repo_candidate.
        let external_edge = EdgeRecord {
            src_usr: "c:@F@caller".to_owned(),
            dst_usr: Some("c:@F@external".to_owned()),
            dst_placeholder: None,
            kind: EdgeKind::Calls,
            resolved: false,
            cross_repo_candidate: false,
            repo_name: "repo".to_owned(),
            attrs_json: "{}".to_owned(),
            tu_hash: [0u8; 32],
            source_association_type: None,
            target_association_type: None,
            src_id: 2,
            dst_id: None,
            dst_repo_name: "repo".to_owned(),
            access: None,
            edge_index: None,
            inherits_is_virtual: None,
        };

        // Two worker shards, each emitting the SAME (caller→target, Calls) edge
        // plus one distinct (caller→target, Uses) edge; shard 0 also carries the
        // external edge.
        let mut w0 = StageWriter::new(&stage_dir, 0).unwrap();
        w0.write_nodes(std::slice::from_ref(&node)).unwrap();
        w0.write_edges(&[
            dup_edge(EdgeKind::Calls),
            dup_edge(EdgeKind::Uses),
            external_edge.clone(),
        ])
        .unwrap();
        w0.finish().unwrap();

        let mut w1 = StageWriter::new(&stage_dir, 1).unwrap();
        w1.write_nodes(&[node]).unwrap();
        w1.write_edges(&[dup_edge(EdgeKind::Calls)]).unwrap();
        w1.finish().unwrap();

        let output = resolve_per_repo(&stage_dir).unwrap();

        // Read back and tally.
        let file = File::open(&output).unwrap();
        let arrow_reader =
            parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder::try_new(file)
                .unwrap()
                .build()
                .unwrap();
        let mut seen: std::collections::HashSet<(String, String, &'static str)> =
            std::collections::HashSet::new();
        let mut total = 0usize;
        let mut cross_repo = 0usize;
        for batch in arrow_reader {
            let batch = batch.unwrap();
            for r in &record_batch_to_edges(&batch) {
                total += 1;
                if r.cross_repo_candidate {
                    cross_repo += 1;
                }
                if let Some(dst) = r.dst_usr.as_ref() {
                    let key = (r.src_usr.clone(), dst.clone(), r.kind.as_str());
                    assert!(
                        seen.insert(key.clone()),
                        "duplicate sink key survived Phase 3 dedup: {key:?}"
                    );
                }
            }
        }
        // Three distinct keyed rows: (caller→target, Calls), (caller→target,
        // Uses), (caller→external, Calls).  The duplicate Calls edge collapsed.
        assert_eq!(total, 3, "expected 3 deduped rows, got {total}");
        assert_eq!(
            cross_repo, 1,
            "the unresolved external edge must be retained for Phase 5"
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
        std::fs::create_dir_all(&worker_dir).unwrap();

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

    // -----------------------------------------------------------------------
    // New dedup-strategy tests (integer-ID path + string-fallback path)
    // -----------------------------------------------------------------------

    /// Helper: build a minimal NodeRecord for test shards.
    fn make_node_record(i: usize, usr: &str) -> crate::schema::NodeRecord {
        crate::schema::NodeRecord {
            usr: usr.to_owned(),
            kind: NodeKind::Function,
            name: format!("fn_{i}"),
            qualified_name: format!("fn_{i}"),
            mangled_name: None,
            file_path: "/repo/src/f.cpp".to_owned(),
            line: Some(1),
            col: Some(1),
            repo_name: "repo".to_owned(),
            attrs_json: "{}".to_owned(),
            partial: false,
            phase: 1,
            tu_hash: [0u8; 32],
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
            symbol_id: i as i64 + 1,
            file_id: 1,
            is_const: None,
            is_constexpr: None,
            storage_class: None,
            is_template: None,
            is_noexcept: None,
            is_override: None,
            is_deleted: None,
            is_defaulted: None,
            cv_qualifiers: None,
            ref_qualifier: None,
            is_final: None,
            is_abstract: None,
            record_kind: None,
            type_spelling: None,
            param_index: None,
            param_kind: None,
            enum_value: None,
        }
    }

    /// Helper: edge with both IDs set (integer-path).
    fn make_edge_with_ids(
        src_usr: &str,
        dst_usr: &str,
        kind: EdgeKind,
        src_id: i64,
        dst_id: i64,
    ) -> EdgeRecord {
        EdgeRecord {
            src_usr: src_usr.to_owned(),
            dst_usr: Some(dst_usr.to_owned()),
            dst_placeholder: None,
            kind,
            resolved: false,
            cross_repo_candidate: false,
            repo_name: "repo".to_owned(),
            attrs_json: "{}".to_owned(),
            tu_hash: [0u8; 32],
            source_association_type: None,
            target_association_type: None,
            src_id,
            dst_id: Some(dst_id),
            dst_repo_name: "repo".to_owned(),
            access: None,
            edge_index: None,
            inherits_is_virtual: None,
        }
    }

    /// Helper: edge with IDs zeroed (string-fallback path).
    fn make_edge_no_ids(src_usr: &str, dst_usr: &str, kind: EdgeKind) -> EdgeRecord {
        make_edge_with_ids(src_usr, dst_usr, kind, 0, 0)
    }

    /// Duplicate edges (integer-ID path) must collapse to one row in final-edges.
    #[test]
    fn dedup_integer_id_path_collapses_duplicates() {
        let dir = tempfile::tempdir().unwrap();
        let stage_dir = dir.path().join("stage");

        // One target node.
        let node = make_node_record(0, "c:@F@target");

        // Same (src, dst, kind) edge with IDs set — emit from two workers.
        let edge_a = make_edge_with_ids("c:@F@src", "c:@F@target", EdgeKind::Calls, 10, 1);
        let edge_b = make_edge_with_ids("c:@F@src", "c:@F@target", EdgeKind::Calls, 10, 1);
        // A different kind — must NOT be merged.
        let edge_c = make_edge_with_ids("c:@F@src", "c:@F@target", EdgeKind::Uses, 10, 1);

        let mut w0 = StageWriter::new(&stage_dir, 0).unwrap();
        w0.write_nodes(std::slice::from_ref(&node)).unwrap();
        w0.write_edges(&[edge_a, edge_c]).unwrap();
        w0.finish().unwrap();

        let mut w1 = StageWriter::new(&stage_dir, 1).unwrap();
        w1.write_nodes(std::slice::from_ref(&node)).unwrap();
        w1.write_edges(&[edge_b]).unwrap();
        w1.finish().unwrap();

        let output = resolve_per_repo(&stage_dir).unwrap();
        let file = File::open(&output).unwrap();
        let arrow_reader =
            parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder::try_new(file)
                .unwrap()
                .build()
                .unwrap();

        let rows: Vec<EdgeRecord> = arrow_reader
            .flat_map(|b| record_batch_to_edges(&b.unwrap()))
            .collect();

        // (src→target, Calls) deduped to 1; (src→target, Uses) is distinct → 2 total.
        assert_eq!(
            rows.len(),
            2,
            "expected 2 distinct rows, got {}",
            rows.len()
        );
        let kinds: std::collections::HashSet<&str> = rows.iter().map(|r| r.kind.as_str()).collect();
        assert!(kinds.contains("CALLS"), "CALLS edge must survive");
        assert!(kinds.contains("USES"), "USES edge must survive");
    }

    /// Duplicate edges (string-fallback path, both IDs == 0) must also collapse.
    #[test]
    fn dedup_string_fallback_path_collapses_duplicates() {
        let dir = tempfile::tempdir().unwrap();
        let stage_dir = dir.path().join("stage");

        let node = make_node_record(0, "c:@F@tgt");

        // IDs are 0 → forces string-fallback path.
        let dup_a = make_edge_no_ids("c:@F@src", "c:@F@tgt", EdgeKind::Calls);
        let dup_b = make_edge_no_ids("c:@F@src", "c:@F@tgt", EdgeKind::Calls);
        let distinct = make_edge_no_ids("c:@F@src", "c:@F@tgt", EdgeKind::Uses);

        let mut w = StageWriter::new(&stage_dir, 0).unwrap();
        w.write_nodes(std::slice::from_ref(&node)).unwrap();
        w.write_edges(&[dup_a, dup_b, distinct]).unwrap();
        w.finish().unwrap();

        let output = resolve_per_repo(&stage_dir).unwrap();
        let file = File::open(&output).unwrap();
        let rows: Vec<EdgeRecord> =
            parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder::try_new(file)
                .unwrap()
                .build()
                .unwrap()
                .flat_map(|b| record_batch_to_edges(&b.unwrap()))
                .collect();

        assert_eq!(rows.len(), 2, "expected 2 deduped rows, got {}", rows.len());
    }

    /// Two edges that differ only in kind must NOT be collapsed.
    #[test]
    fn dedup_different_kind_not_merged() {
        let dir = tempfile::tempdir().unwrap();
        let stage_dir = dir.path().join("stage");

        let node = make_node_record(0, "c:@F@tgt2");

        let calls_edge = make_edge_with_ids("c:@F@src2", "c:@F@tgt2", EdgeKind::Calls, 20, 1);
        let uses_edge = make_edge_with_ids("c:@F@src2", "c:@F@tgt2", EdgeKind::Uses, 20, 1);
        // Duplicate of Calls — must collapse.
        let calls_dup = make_edge_with_ids("c:@F@src2", "c:@F@tgt2", EdgeKind::Calls, 20, 1);

        let mut w = StageWriter::new(&stage_dir, 0).unwrap();
        w.write_nodes(std::slice::from_ref(&node)).unwrap();
        w.write_edges(&[calls_edge, uses_edge, calls_dup]).unwrap();
        w.finish().unwrap();

        let output = resolve_per_repo(&stage_dir).unwrap();
        let file = File::open(&output).unwrap();
        let rows: Vec<EdgeRecord> =
            parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder::try_new(file)
                .unwrap()
                .build()
                .unwrap()
                .flat_map(|b| record_batch_to_edges(&b.unwrap()))
                .collect();

        assert_eq!(
            rows.len(),
            2,
            "Calls+Uses must survive; duplicate Calls must collapse → 2 rows, got {}",
            rows.len()
        );
    }
}
