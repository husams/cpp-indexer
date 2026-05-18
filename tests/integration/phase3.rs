/// Integration test for S10-resolve-per-repo (AC-M1-18, AC-M1-19, AC-M2-13).
///
/// Synthesises Parquet shards with:
///   - N within-repo edges whose `dst_usr` exists in the node shard.
///   - K unresolved edges whose `dst_usr` is absent from the node shard.
///
/// Asserts:
///   - `final-edges.parquet` is produced.
///   - All N within-repo edges have `resolved=true` and `cross_repo_candidate=false`.
///   - All K unresolved edges have `resolved=false` and `cross_repo_candidate=true`.
///   - No overlap (no edge is simultaneously `resolved` and `cross_repo_candidate`).
use std::{fs::File, path::Path};

use cpp_indexer::{
    resolve::per_repo::resolve_per_repo,
    schema::{
        arrow::{edges_to_record_batch, record_batch_to_edges},
        EdgeKind, EdgeRecord, NodeKind, NodeRecord,
    },
    stage::writer::StageWriter,
};

// -------------------------------------------------------------------------
// Fixture builders
// -------------------------------------------------------------------------

const N_WITHIN: usize = 5;
const K_UNRESOLVED: usize = 3;

fn make_node(i: usize) -> NodeRecord {
    NodeRecord {
        usr: format!("c:@F@fn_{i}"),
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
        tu_hash: [u8::try_from(i % 256).unwrap(); 32],
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
    }
}

fn make_within_repo_edge(i: usize) -> EdgeRecord {
    EdgeRecord {
        src_usr: format!("c:@F@caller_{i}"),
        dst_usr: Some(format!("c:@F@fn_{i}")), // exists in node shard
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

fn make_unresolved_edge(i: usize) -> EdgeRecord {
    EdgeRecord {
        src_usr: format!("c:@F@ext_caller_{i}"),
        dst_usr: Some(format!("c:@F@external_{i}")), // absent from node shard
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

// -------------------------------------------------------------------------
// Helper: read all EdgeRecords from a parquet file
// -------------------------------------------------------------------------

fn read_edges(path: &Path) -> Vec<EdgeRecord> {
    let file = File::open(path).expect("final-edges.parquet must exist");
    let reader = parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder::try_new(file)
        .expect("build reader")
        .build()
        .expect("build batch reader");

    let mut edges = Vec::new();
    for batch in reader {
        let batch = batch.expect("read batch");
        edges.extend(record_batch_to_edges(&batch));
    }
    edges
}

// -------------------------------------------------------------------------
// Tests
// -------------------------------------------------------------------------

/// Happy path: N within-repo + K unresolved edges; assert classification counts.
#[test]
fn phase3_classifies_within_and_cross_repo_edges() {
    let dir = tempfile::tempdir().unwrap();
    let stage_dir = dir.path().join("stage");

    let nodes: Vec<NodeRecord> = (0..N_WITHIN).map(make_node).collect();
    let mut edges: Vec<EdgeRecord> = (0..N_WITHIN).map(make_within_repo_edge).collect();
    edges.extend((0..K_UNRESOLVED).map(make_unresolved_edge));

    let mut writer = StageWriter::new(&stage_dir, 0).unwrap();
    writer.write_nodes(&nodes).unwrap();
    writer.write_edges(&edges).unwrap();
    writer.finish().unwrap();

    let output = resolve_per_repo(&stage_dir).expect("phase3 must succeed");
    assert!(
        output.exists(),
        "final-edges.parquet must exist at {output:?}"
    );

    let result = read_edges(&output);

    assert_eq!(
        result.len(),
        N_WITHIN + K_UNRESOLVED,
        "all edges must appear in final-edges.parquet"
    );

    let resolved: Vec<_> = result.iter().filter(|e| e.resolved).collect();
    let cross_repo: Vec<_> = result.iter().filter(|e| e.cross_repo_candidate).collect();

    assert_eq!(
        resolved.len(),
        N_WITHIN,
        "exactly N within-repo edges must have resolved=true"
    );
    assert_eq!(
        cross_repo.len(),
        K_UNRESOLVED,
        "exactly K unresolved edges must have cross_repo_candidate=true"
    );

    // No edge may be simultaneously resolved and cross_repo_candidate.
    let both: Vec<_> = result
        .iter()
        .filter(|e| e.resolved && e.cross_repo_candidate)
        .collect();
    assert!(
        both.is_empty(),
        "no edge may be both resolved and cross_repo_candidate simultaneously"
    );

    // Within-repo edges must not be cross_repo_candidate.
    for e in &resolved {
        assert!(
            !e.cross_repo_candidate,
            "resolved edge {e:?} must not have cross_repo_candidate=true"
        );
    }

    // Unresolved edges must not be marked resolved.
    for e in &cross_repo {
        assert!(
            !e.resolved,
            "cross_repo edge {e:?} must not have resolved=true"
        );
    }
}

/// Placeholder edges (dst_usr = None) are carried forward with their original flags.
#[test]
fn phase3_preserves_placeholder_edges_unchanged() {
    let dir = tempfile::tempdir().unwrap();
    let stage_dir = dir.path().join("stage");

    let nodes: Vec<NodeRecord> = (0..2).map(make_node).collect();

    // One normal within-repo edge + one placeholder.
    let mut writer = StageWriter::new(&stage_dir, 0).unwrap();
    writer.write_nodes(&nodes).unwrap();

    // Write the placeholder edge directly; StageWriter accepts it.
    let placeholder = EdgeRecord {
        src_usr: "c:@F@src".to_owned(),
        dst_usr: None,
        dst_placeholder: Some("some::Symbol".to_owned()),
        kind: EdgeKind::Uses,
        resolved: false,
        cross_repo_candidate: false,
        repo_name: "test-repo".to_owned(),
        attrs_json: "{}".to_owned(),
        tu_hash: [0u8; 32],
        source_association_type: None,
        target_association_type: None,
    };

    let batch = edges_to_record_batch(&[placeholder]).expect("batch");
    // We need to use the underlying parquet writer; use a second StageWriter worker
    // and append the edge via its edge writer.
    let mut w2 = StageWriter::new(&stage_dir, 1).unwrap();
    // Re-create the placeholder for w2.
    let placeholder2 = EdgeRecord {
        src_usr: "c:@F@src".to_owned(),
        dst_usr: None,
        dst_placeholder: Some("some::Symbol".to_owned()),
        kind: EdgeKind::Uses,
        resolved: false,
        cross_repo_candidate: false,
        repo_name: "test-repo".to_owned(),
        attrs_json: "{}".to_owned(),
        tu_hash: [0u8; 32],
        source_association_type: None,
        target_association_type: None,
    };
    drop(batch); // batch was only created to verify serialisability
    w2.write_edges(&[placeholder2]).unwrap();
    w2.finish().unwrap();
    writer.finish().unwrap();

    let output = resolve_per_repo(&stage_dir).expect("phase3 must succeed");
    let result = read_edges(&output);

    let placeholder_results: Vec<_> = result.iter().filter(|e| e.dst_usr.is_none()).collect();
    assert_eq!(placeholder_results.len(), 1);
    let p = placeholder_results[0];
    assert!(!p.resolved, "placeholder edge must remain resolved=false");
    assert!(
        !p.cross_repo_candidate,
        "placeholder edge must remain cross_repo_candidate=false"
    );
}

/// Empty stage directory (no shards) produces an empty `final-edges.parquet`.
#[test]
fn phase3_empty_stage_produces_empty_output() {
    let dir = tempfile::tempdir().unwrap();
    let stage_dir = dir.path().join("stage");
    std::fs::create_dir_all(&stage_dir).unwrap();

    let output = resolve_per_repo(&stage_dir).expect("phase3 must succeed on empty stage");
    let result = read_edges(&output);
    assert!(result.is_empty(), "no edges expected for empty stage dir");
}

/// Multiple workers' shards are all picked up and processed.
#[test]
fn phase3_collects_shards_from_multiple_workers() {
    let dir = tempfile::tempdir().unwrap();
    let stage_dir = dir.path().join("stage");

    // Worker 0: 2 nodes + 2 within-repo edges.
    let nodes_w0: Vec<NodeRecord> = (0..2).map(make_node).collect();
    let edges_w0: Vec<EdgeRecord> = (0..2).map(make_within_repo_edge).collect();
    let mut w0 = StageWriter::new(&stage_dir, 0).unwrap();
    w0.write_nodes(&nodes_w0).unwrap();
    w0.write_edges(&edges_w0).unwrap();
    w0.finish().unwrap();

    // Worker 1: 2 more nodes + 1 unresolved edge.
    let nodes_w1: Vec<NodeRecord> = (2..4).map(make_node).collect();
    let edges_w1: Vec<EdgeRecord> = vec![make_unresolved_edge(99)];
    let mut w1 = StageWriter::new(&stage_dir, 1).unwrap();
    w1.write_nodes(&nodes_w1).unwrap();
    w1.write_edges(&edges_w1).unwrap();
    w1.finish().unwrap();

    let output = resolve_per_repo(&stage_dir).expect("phase3 must succeed");
    let result = read_edges(&output);

    assert_eq!(
        result.len(),
        3,
        "2 within-repo + 1 unresolved = 3 edges total"
    );

    let resolved: usize = result.iter().filter(|e| e.resolved).count();
    let cross_repo: usize = result.iter().filter(|e| e.cross_repo_candidate).count();
    assert_eq!(resolved, 2);
    assert_eq!(cross_repo, 1);
}
