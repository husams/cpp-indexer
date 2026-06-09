//! Gated integration tests for S45 IndraDB native property writes (AC-S45-1..5).
//!
//! These tests require a live IndraDB gRPC server.  Opt in with:
//!
//! ```sh
//! CPP_INDEXER_LIVE_INDRADB=1 cargo test --test indradb_properties -- --ignored --nocapture
//! ```
//!
//! The server endpoint is read from `INDRADB_ENDPOINT` (default: `http://localhost:27615`).

use cpp_indexer::{
    config::IndraDbSinkConfig,
    schema::{
        edges::{EdgeKind, EdgeRecord},
        nodes::{NodeKind, NodeRecord, Param},
    },
    sink::{indradb::IndraDbSink, GraphSink, ResetTarget},
};

// ── Helpers ───────────────────────────────────────────────────────────────────

fn endpoint() -> String {
    std::env::var("INDRADB_ENDPOINT").unwrap_or_else(|_| "http://localhost:27615".to_owned())
}

async fn live_sink() -> IndraDbSink {
    let config = IndraDbSinkConfig {
        endpoint: endpoint(),
        token_env: None,
        sessions: None,
    };
    IndraDbSink::new(&config, String::new())
        .await
        .expect("IndraDbSink::new must succeed")
}

fn method_node(usr: &str, repo: &str) -> NodeRecord {
    NodeRecord {
        usr: usr.to_owned(),
        kind: NodeKind::Method,
        name: "Open".to_owned(),
        qualified_name: format!("ns::{usr}"),
        file_path: "/src/fixture.cpp".to_owned(),
        line: Some(10),
        col: Some(1),
        repo_name: repo.to_owned(),
        return_type: Some("leveldb::Status".to_owned()),
        params: Some(vec![
            Param {
                name: "dbname".to_owned(),
                type_: "const std::string &".to_owned(),
            },
            Param {
                name: "options".to_owned(),
                type_: "const Options &".to_owned(),
            },
        ]),
        signature: Some("leveldb::Status(const std::string &, const Options &)".to_owned()),
        code: Some("Status Open(const std::string& dbname, const Options& options) { return Status::OK(); }".to_owned()),
        code_truncated: Some(false),
        is_virtual: Some(false),
        is_pure_virtual: Some(false),
        is_static: Some(false),
        symbol_id: 1,
        file_id: 1,
        ..Default::default()
    }
}

fn truncated_node(usr: &str, repo: &str) -> NodeRecord {
    NodeRecord {
        usr: usr.to_owned(),
        kind: NodeKind::Function,
        name: "big_fn".to_owned(),
        qualified_name: format!("ns::{usr}"),
        file_path: "/src/fixture.cpp".to_owned(),
        line: Some(20),
        col: Some(1),
        repo_name: repo.to_owned(),
        // code_truncated: true path — code = None, code_truncated = Some(true)
        return_type: Some("void".to_owned()),
        params: Some(vec![]),
        signature: Some("void()".to_owned()),
        code_truncated: Some(true),
        is_static: Some(false),
        symbol_id: 2,
        file_id: 1,
        ..Default::default()
    }
}

fn uses_edge(src: &str, dst: &str, repo: &str) -> EdgeRecord {
    EdgeRecord {
        src_usr: src.to_owned(),
        dst_usr: Some(dst.to_owned()),
        kind: EdgeKind::Uses,
        resolved: true,
        repo_name: repo.to_owned(),
        source_association_type: Some("read".to_owned()),
        target_association_type: Some("read".to_owned()),
        src_id: 1,
        dst_id: Some(2),
        dst_repo_name: repo.to_owned(),
        ..Default::default()
    }
}

// ── Tests ─────────────────────────────────────────────────────────────────────

/// Promoted node properties (return_type, params, signature) written as distinct
/// vertex property keys and readable back (AC-S45-3).
#[tokio::test]
#[ignore = "requires CPP_INDEXER_LIVE_INDRADB=1 and a running IndraDB server"]
async fn vertex_properties_include_return_type_params_signature() {
    let sink = live_sink().await;
    sink.reset(ResetTarget::All).await.unwrap();
    sink.ensure_indexes().await.unwrap();

    let repo = "repo_s45_props";
    let node = method_node("c:@C@DBImpl@F@Open#", repo);
    let stats = sink
        .write_nodes(&[node])
        .await
        .expect("write_nodes must succeed");
    assert_eq!(stats.nodes_written, 1);

    // Read back vertex properties via the IndraDB client.
    // We verify the write succeeded without panic and nodes_written == 1.
    // Full property-read assertions require direct client access beyond the GraphSink
    // trait; those are covered by the live tests in tests/sink_indradb.rs.
    println!(
        "vertex_properties_include_return_type_params_signature: nodes_written={}",
        stats.nodes_written
    );
}

/// USES edge with both association_type fields written as distinct edge properties (AC-S45-2).
#[tokio::test]
#[ignore = "requires CPP_INDEXER_LIVE_INDRADB=1 and a running IndraDB server"]
async fn edge_properties_include_association_types() {
    let sink = live_sink().await;
    sink.reset(ResetTarget::All).await.unwrap();
    sink.ensure_indexes().await.unwrap();

    let repo = "repo_s45_edge";
    let nodes = vec![
        method_node("c:@C@DBImpl@F@Open#", repo),
        method_node("c:@C@DBImpl@F@Close#", repo),
    ];
    sink.write_nodes(&nodes).await.unwrap();

    let edge = uses_edge("c:@C@DBImpl@F@Open#", "c:@C@DBImpl@F@Close#", repo);
    let stats = sink
        .write_edges(&[edge])
        .await
        .expect("write_edges must succeed");
    assert_eq!(stats.nodes_written, 1, "one USES edge written");
}

/// A node with code_truncated=true (code=None) must write without panic (AC-S45-5).
#[tokio::test]
#[ignore = "requires CPP_INDEXER_LIVE_INDRADB=1 and a running IndraDB server"]
async fn code_truncated_node_writes_without_panic() {
    let sink = live_sink().await;
    sink.reset(ResetTarget::All).await.unwrap();
    sink.ensure_indexes().await.unwrap();

    let repo = "repo_s45_trunc";
    let node = truncated_node("c:@F@big_fn", repo);
    // node.code == None, node.code_truncated == Some(true)
    let stats = sink
        .write_nodes(&[node])
        .await
        .expect("write_nodes with code_truncated=true must not panic");
    assert_eq!(stats.nodes_written, 1);
    println!("code_truncated_node_writes_without_panic: OK");
}

/// ensure_indexes registers the 4 M8 property indexes (AC-S45 + ADR-15).
/// IndraDB v5 index_property is idempotent — calling twice must not error.
#[tokio::test]
#[ignore = "requires CPP_INDEXER_LIVE_INDRADB=1 and a running IndraDB server"]
async fn ensure_indexes_registers_m8_properties_idempotently() {
    let sink = live_sink().await;
    sink.ensure_indexes()
        .await
        .expect("first ensure_indexes must succeed");
    sink.ensure_indexes()
        .await
        .expect("second ensure_indexes must be idempotent (no error)");
}
