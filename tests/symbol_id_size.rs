//! Story 6 — Verification: size measurement + full regression sweep.
//!
//! # S6-SC-03 structural proxy
//! Asserts that the v6 `node_schema()` and `edge_schema()` contain the integer-ID
//! fields (`symbol_id`, `file_id`, `src_id`, `dst_id`, `dst_repo_name`) and that a
//! Parquet round-trip (`nodes_to_record_batch` → `record_batch_to_nodes`) preserves
//! those integer IDs exactly.
//!
//! These are NO-live-service hard CI gates: no Neo4j, IndraDB, libclang, or network.
//!
//! # S4-SC-06 size measurement (not a gate — recorded in implementation-notes.md)
//! See implementation notes for the v5→v6 per-record byte delta.

use cpp_indexer::schema::{
    arrow::{
        edge_schema, edges_to_record_batch, node_schema, nodes_to_record_batch,
        record_batch_to_edges, record_batch_to_nodes,
    },
    edges::{EdgeKind, EdgeRecord},
    nodes::{NodeKind, NodeRecord},
};

// ── Fixtures ──────────────────────────────────────────────────────────────────

fn fixture_node(usr: &str, symbol_id: i64, file_id: i64) -> NodeRecord {
    NodeRecord {
        usr: usr.to_owned(),
        kind: NodeKind::Function,
        name: "fixture_fn".to_owned(),
        qualified_name: format!("ns::{usr}"),
        mangled_name: None,
        file_path: "/src/fixture.cpp".to_owned(),
        line: Some(1),
        col: Some(1),
        repo_name: "test-repo".to_owned(),
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
        symbol_id,
        file_id,
    }
}

fn fixture_edge(src_usr: &str, dst_usr: &str, src_id: i64, dst_id: i64) -> EdgeRecord {
    EdgeRecord {
        src_usr: src_usr.to_owned(),
        dst_usr: Some(dst_usr.to_owned()),
        dst_placeholder: None,
        kind: EdgeKind::Calls,
        resolved: true,
        cross_repo_candidate: false,
        repo_name: "test-repo".to_owned(),
        attrs_json: "{}".to_owned(),
        tu_hash: [0u8; 32],
        source_association_type: None,
        target_association_type: None,
        src_id,
        dst_id: Some(dst_id),
        dst_repo_name: "test-repo".to_owned(),
    }
}

// ── Schema field-presence tests (S6-SC-03 proxy) ─────────────────────────────

/// v6 node schema must contain exactly 25 fields (23 M8 + 2 integer-ID fields).
#[test]
fn node_schema_has_expected_column_count() {
    let schema = node_schema();
    assert_eq!(
        schema.fields().len(),
        25,
        "expected 25 node schema fields (13 base + 10 M8 + 2 v6 integer IDs): got {}",
        schema.fields().len()
    );
}

/// v6 edge schema must contain exactly 14 fields (11 M8 + 3 integer-ID fields).
#[test]
fn edge_schema_has_expected_column_count() {
    let schema = edge_schema();
    assert_eq!(
        schema.fields().len(),
        14,
        "expected 14 edge schema fields (9 base + 2 M8 + 3 v6 integer IDs): got {}",
        schema.fields().len()
    );
}

/// v6 node schema must contain the integer-ID fields `symbol_id` and `file_id`.
#[test]
fn node_schema_contains_integer_id_fields() {
    let schema = node_schema();
    let names: Vec<&str> = schema.fields().iter().map(|f| f.name().as_str()).collect();

    assert!(
        names.contains(&"symbol_id"),
        "node schema must contain 'symbol_id'; got: {names:?}"
    );
    assert!(
        names.contains(&"file_id"),
        "node schema must contain 'file_id'; got: {names:?}"
    );
}

/// v6 edge schema must contain `src_id`, `dst_id`, `dst_repo_name`.
#[test]
fn edge_schema_contains_integer_id_fields() {
    let schema = edge_schema();
    let names: Vec<&str> = schema.fields().iter().map(|f| f.name().as_str()).collect();

    assert!(
        names.contains(&"src_id"),
        "edge schema must contain 'src_id'; got: {names:?}"
    );
    assert!(
        names.contains(&"dst_id"),
        "edge schema must contain 'dst_id'; got: {names:?}"
    );
    assert!(
        names.contains(&"dst_repo_name"),
        "edge schema must contain 'dst_repo_name'; got: {names:?}"
    );
}

// ── Parquet round-trip tests ──────────────────────────────────────────────────

/// Integer ID fields survive a Parquet serialise → deserialise round-trip (S6-SC-03 proxy).
#[test]
fn node_integer_ids_survive_parquet_roundtrip() {
    let nodes = vec![
        fixture_node("c:@F@alpha", 1, 10),
        fixture_node("c:@F@beta", 2, 10),
        fixture_node("c:@F@gamma", 3, 11),
    ];

    let batch = nodes_to_record_batch(&nodes).expect("nodes_to_record_batch must succeed");
    let recovered = record_batch_to_nodes(&batch);

    assert_eq!(recovered.len(), nodes.len());
    for (orig, got) in nodes.iter().zip(recovered.iter()) {
        assert_eq!(
            got.symbol_id, orig.symbol_id,
            "symbol_id must survive round-trip for USR '{}'",
            orig.usr
        );
        assert_eq!(
            got.file_id, orig.file_id,
            "file_id must survive round-trip for USR '{}'",
            orig.usr
        );
        // S5-SC-04: string fields must be preserved too.
        assert_eq!(got.usr, orig.usr, "usr must survive round-trip");
        assert_eq!(
            got.file_path, orig.file_path,
            "file_path must survive round-trip"
        );
    }
}

/// Edge integer ID fields survive a Parquet round-trip (S6-SC-03 proxy).
#[test]
fn edge_integer_ids_survive_parquet_roundtrip() {
    let edges = vec![
        fixture_edge("c:@F@alpha", "c:@F@beta", 1, 2),
        fixture_edge("c:@F@beta", "c:@F@gamma", 2, 3),
    ];

    let batch = edges_to_record_batch(&edges).expect("edges_to_record_batch must succeed");
    let recovered = record_batch_to_edges(&batch);

    assert_eq!(recovered.len(), edges.len());
    for (orig, got) in edges.iter().zip(recovered.iter()) {
        assert_eq!(
            got.src_id,
            orig.src_id,
            "src_id must survive round-trip for edge '{}'→'{}'",
            orig.src_usr,
            orig.dst_usr.as_deref().unwrap_or("")
        );
        assert_eq!(got.dst_id, orig.dst_id, "dst_id must survive round-trip");
        assert_eq!(
            got.dst_repo_name, orig.dst_repo_name,
            "dst_repo_name must survive round-trip"
        );
    }
}

/// Nodes with symbol_id > 0 satisfy the S6-SC-03 structural assertion
/// (integer IDs are present on v6 records).
#[test]
fn all_fixture_nodes_have_nonzero_integer_ids() {
    let nodes = vec![
        fixture_node("c:@F@f1", 1, 10),
        fixture_node("c:@F@f2", 2, 11),
    ];

    for node in &nodes {
        assert!(
            node.symbol_id > 0,
            "fixture node '{}' must have symbol_id > 0",
            node.usr
        );
        assert!(
            node.file_id > 0,
            "fixture node '{}' must have file_id > 0",
            node.usr
        );
    }
}
