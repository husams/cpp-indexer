//! Gated integration test for EXTERNAL_REF access-classification mirroring (ADR-13).
//!
//! Opted in via `CPP_INDEXER_LIVE_NEO4J=1` (same pattern as S41/S42 neo4j_indexes gate).
//!
//! AC covered:
//!   ADR-13 §EXTERNAL_REF mirroring — Phase 5 copies source_association_type /
//!   target_association_type from the originating USES edge onto the synthesised
//!   EXTERNAL_REF edge.
//!
//! The test creates two in-memory stage directories wired to mock Parquet shards,
//! runs Phase 5, and asserts that EXTERNAL_REF edges carry the same classification
//! as the source USES edge.

use cpp_indexer::schema::arrow::edges_to_record_batch;
use cpp_indexer::schema::edges::{EdgeKind, EdgeRecord};

// ── Gating helper ─────────────────────────────────────────────────────────────

/// Returns `true` when `CPP_INDEXER_LIVE_NEO4J=1` is set in the environment.
fn live_neo4j_enabled() -> bool {
    std::env::var("CPP_INDEXER_LIVE_NEO4J")
        .map(|v| v == "1")
        .unwrap_or(false)
}

// ── Tests ──────────────────────────────────────────────────────────────────────

/// ADR-13: EXTERNAL_REF edge mirrors source_association_type / target_association_type
/// from the originating USES edge.
///
/// This test is `#[ignore]` by default; run with:
///   CPP_INDEXER_LIVE_NEO4J=1 cargo test --test cross_repo_access_mirror -- --ignored --nocapture
#[tokio::test]
#[ignore]
async fn external_ref_mirrors_uses_classification() {
    if !live_neo4j_enabled() {
        eprintln!("SKIP: CPP_INDEXER_LIVE_NEO4J=1 not set");
        return;
    }

    // Build two stage directories:
    //   repo-a: has a USES edge with source_association_type = "write"
    //   repo-b: owns the destination USR
    use cpp_indexer::schema::arrow::nodes_to_record_batch;
    use cpp_indexer::schema::nodes::{NodeKind, NodeRecord};
    use std::path::PathBuf;
    use tempfile::TempDir;

    // ── repo-a stage dir ──────────────────────────────────────────────────────
    let dir_a = TempDir::new().unwrap();
    let stage_a: PathBuf = dir_a.path().to_owned();
    let worker_a = stage_a.join("worker-000");
    std::fs::create_dir_all(&worker_a).unwrap();

    // repo-a source node
    let node_a = NodeRecord {
        usr: "c:@F@src_fn".to_owned(),
        kind: NodeKind::Function,
        name: "src_fn".to_owned(),
        qualified_name: "src_fn".to_owned(),
        file_path: "/repo-a/src.cpp".to_owned(),
        line: Some(1),
        col: Some(1),
        repo_name: "repo-a".to_owned(),
        ..Default::default()
    };
    let batch_na = nodes_to_record_batch(&[node_a]).unwrap();
    write_parquet_batch(&worker_a.join("nodes-0.parquet"), &batch_na);

    // repo-a USES edge pointing at repo-b USR, with source_association_type = "write"
    let edge_a = EdgeRecord {
        src_usr: "c:@F@src_fn".to_owned(),
        dst_usr: Some("c:@F@dst_fn".to_owned()),
        kind: EdgeKind::Uses,
        cross_repo_candidate: true,
        repo_name: "repo-a".to_owned(),
        source_association_type: Some("write".to_owned()),
        target_association_type: Some("write".to_owned()),
        dst_repo_name: "repo-a".to_owned(),
        ..Default::default()
    };
    let batch_ea = edges_to_record_batch(&[edge_a]).unwrap();
    write_parquet_batch(&worker_a.join("edges-0.parquet"), &batch_ea);

    // final-edges.parquet (Phase 3 output) — same edge, marked cross_repo_candidate
    let final_edge = EdgeRecord {
        src_usr: "c:@F@src_fn".to_owned(),
        dst_usr: Some("c:@F@dst_fn".to_owned()),
        kind: EdgeKind::Uses,
        cross_repo_candidate: true,
        repo_name: "repo-a".to_owned(),
        source_association_type: Some("write".to_owned()),
        target_association_type: Some("write".to_owned()),
        dst_repo_name: "repo-a".to_owned(),
        ..Default::default()
    };
    let batch_fe = edges_to_record_batch(&[final_edge]).unwrap();
    write_parquet_batch(&stage_a.join("final-edges.parquet"), &batch_fe);

    // ── repo-b stage dir ──────────────────────────────────────────────────────
    let dir_b = TempDir::new().unwrap();
    let stage_b: PathBuf = dir_b.path().to_owned();
    let worker_b = stage_b.join("worker-000");
    std::fs::create_dir_all(&worker_b).unwrap();

    let node_b = NodeRecord {
        usr: "c:@F@dst_fn".to_owned(),
        kind: NodeKind::Function,
        name: "dst_fn".to_owned(),
        qualified_name: "dst_fn".to_owned(),
        file_path: "/repo-b/dst.cpp".to_owned(),
        line: Some(1),
        col: Some(1),
        repo_name: "repo-b".to_owned(),
        ..Default::default()
    };
    let batch_nb = nodes_to_record_batch(&[node_b]).unwrap();
    write_parquet_batch(&worker_b.join("nodes-0.parquet"), &batch_nb);

    // repo-b has no cross-repo edges; final-edges.parquet is empty but must exist.
    let batch_empty = edges_to_record_batch(&[]).unwrap();
    write_parquet_batch(&stage_b.join("final-edges.parquet"), &batch_empty);

    // ── Run Phase 5 via the mock sink ────────────────────────────────────────
    use cpp_indexer::resolve::cross_repo::{run, Phase5Options};
    use cpp_indexer::sink::mock::MockSink;
    use std::sync::Arc;
    use std::time::Duration;

    let mock = Arc::new(MockSink::default());
    let opts = Phase5Options {
        stage_dirs: vec![stage_a.clone(), stage_b.clone()],
        lock_ttl: Duration::from_secs(10),
        batch_size: 100,
        canonical_overrides: vec![],
    };

    // Phase 5 requires the schema version to be set; mock returns None by default.
    let stats = run(mock.clone(), opts).await.expect("Phase 5 must succeed");

    assert_eq!(
        stats.external_refs_written, 1,
        "expected exactly 1 EXTERNAL_REF edge to be written"
    );

    // Verify the written edge carries the classification.
    use cpp_indexer::sink::mock::MockCall;
    let all_edges: Vec<EdgeRecord> = mock
        .calls()
        .into_iter()
        .filter_map(|c| {
            if let MockCall::WriteEdges(edges) = c {
                Some(edges)
            } else {
                None
            }
        })
        .flatten()
        .collect();
    let ext_ref = all_edges
        .iter()
        .find(|e| e.kind == EdgeKind::ExternalRef)
        .expect("EXTERNAL_REF edge must be written");

    assert_eq!(
        ext_ref.source_association_type.as_deref(),
        Some("write"),
        "EXTERNAL_REF must mirror source_association_type='write' from originating USES edge"
    );
    assert_eq!(
        ext_ref.target_association_type.as_deref(),
        Some("write"),
        "EXTERNAL_REF must mirror target_association_type='write' from originating USES edge"
    );
}

// ---------------------------------------------------------------------------
// Parquet-write helper (same pattern as phase3.rs)
// ---------------------------------------------------------------------------

fn write_parquet_batch(path: &std::path::Path, batch: &arrow::record_batch::RecordBatch) {
    use parquet::arrow::ArrowWriter;
    use parquet::file::properties::WriterProperties;

    let file = std::fs::File::create(path).expect("create parquet file");
    let props = WriterProperties::builder().build();
    let mut writer =
        ArrowWriter::try_new(file, batch.schema(), Some(props)).expect("create ArrowWriter");
    writer.write(batch).expect("write batch");
    writer.close().expect("close writer");
}
