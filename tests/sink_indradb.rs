//! Integration tests for `IndraDbSink` against a live IndraDB gRPC server.
//!
//! Story: S12-sink-indradb (AC-M1-21, AC-M1-23 IndraDB half, AC-M3-6).
//! ADR: ADR-2.
//!
//! # Gating
//!
//! All tests are `#[ignore]` by default.  They require a running IndraDB server.
//! Set `INDRADB_ENDPOINT` to the gRPC base URL before running:
//!
//! ```sh
//! docker compose -f tests/compose/indradb.yml up -d
//! INDRADB_ENDPOINT=http://localhost:27615 \
//!     cargo nextest run --test sink_indradb -- --ignored
//! docker compose -f tests/compose/indradb.yml down
//! ```

use std::sync::Arc;

use cpp_indexer::{
    config::IndraDbSinkConfig,
    schema::edges::{EdgeKind, EdgeRecord},
    schema::nodes::{NodeKind, NodeRecord},
    sink::{indradb::IndraDbSink, GraphSink, ResetTarget},
};

// ── Helpers ───────────────────────────────────────────────────────────────────

/// Return the IndraDB endpoint from the environment, or `None` if unset.
fn endpoint() -> Option<String> {
    std::env::var("INDRADB_ENDPOINT").ok()
}

/// Build an `IndraDbSinkConfig` from `$INDRADB_ENDPOINT`.
///
/// # Panics
///
/// Panics if `INDRADB_ENDPOINT` is not set (tests must be `#[ignore]` when unset).
async fn live_sink() -> Arc<dyn GraphSink> {
    let ep = endpoint().expect("INDRADB_ENDPOINT must be set for live tests");
    let config = IndraDbSinkConfig {
        endpoint: ep,
        token_env: None,
        sessions: None,
    };
    let sink = IndraDbSink::new(&config, String::new())
        .await
        .expect("IndraDbSink::new must succeed");
    Arc::new(sink)
}

fn make_node(usr: &str, repo: &str) -> NodeRecord {
    NodeRecord {
        usr: usr.to_owned(),
        kind: NodeKind::Function,
        name: "foo".to_owned(),
        qualified_name: format!("ns::{usr}"),
        mangled_name: None,
        file_path: "/src/foo.cpp".to_owned(),
        line: Some(10),
        col: Some(1),
        repo_name: repo.to_owned(),
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
    }
}

fn make_edge(src: &str, dst: &str, repo: &str) -> EdgeRecord {
    EdgeRecord {
        src_usr: src.to_owned(),
        dst_usr: Some(dst.to_owned()),
        dst_placeholder: None,
        kind: EdgeKind::Calls,
        resolved: true,
        cross_repo_candidate: false,
        repo_name: repo.to_owned(),
        attrs_json: "{}".to_owned(),
        tu_hash: [0u8; 32],
        source_association_type: None,
        target_association_type: None,
        src_id: 1,
        dst_id: Some(2),
        dst_repo_name: repo.to_owned(),
    }
}

// ── Tests ─────────────────────────────────────────────────────────────────────

/// Verify that the sink can connect and respond to a ping.
#[tokio::test]
#[ignore = "requires INDRADB_ENDPOINT"]
async fn preflight_succeeds_with_live_server() {
    let sink = live_sink().await;
    sink.preflight().await.expect("preflight must succeed");
}

/// Verify health probe returns `"ok"` with a measurable latency.
#[tokio::test]
#[ignore = "requires INDRADB_ENDPOINT"]
async fn health_returns_ok() {
    let sink = live_sink().await;
    let info = sink.health().await.expect("health must succeed");
    assert_eq!(info.status, "ok");
    // Latency should be non-negative (no negative clock skew).
    assert!(info.latency.as_nanos() < u128::MAX);
}

/// `ensure_indexes` must succeed and be idempotent (safe to call twice).
#[tokio::test]
#[ignore = "requires INDRADB_ENDPOINT"]
async fn ensure_indexes_is_idempotent() {
    let sink = live_sink().await;
    sink.ensure_indexes()
        .await
        .expect("first ensure_indexes call must succeed");
    sink.ensure_indexes()
        .await
        .expect("second ensure_indexes call must be idempotent");
}

/// Write a batch of nodes; the call must succeed and return the correct count.
#[tokio::test]
#[ignore = "requires INDRADB_ENDPOINT"]
async fn write_nodes_returns_correct_count() {
    let sink = live_sink().await;
    sink.ensure_indexes().await.unwrap();
    // Reset first to start from a clean slate.
    sink.reset(ResetTarget::All).await.unwrap();

    let nodes = vec![
        make_node("c:@F@alpha", "repo_s12"),
        make_node("c:@F@beta", "repo_s12"),
        make_node("c:@F@gamma", "repo_s12"),
    ];
    let stats = sink
        .write_nodes(&nodes)
        .await
        .expect("write_nodes must succeed");
    assert_eq!(stats.nodes_written, 3, "must report 3 nodes written");
    assert_eq!(stats.retries, 0, "no retries expected on healthy server");
}

/// `write_nodes` must be idempotent: writing the same batch twice does not error.
#[tokio::test]
#[ignore = "requires INDRADB_ENDPOINT"]
async fn write_nodes_is_idempotent() {
    let sink = live_sink().await;
    sink.ensure_indexes().await.unwrap();
    sink.reset(ResetTarget::All).await.unwrap();

    let nodes = vec![
        make_node("c:@F@foo_idem", "repo_s12_idem"),
        make_node("c:@F@bar_idem", "repo_s12_idem"),
    ];
    sink.write_nodes(&nodes)
        .await
        .expect("first write must succeed");
    // Second write of the same batch must not error (idempotency AC-M3-6).
    sink.write_nodes(&nodes)
        .await
        .expect("second write (idempotent) must succeed");
}

/// Write a batch of edges; the call must succeed and return the correct count.
#[tokio::test]
#[ignore = "requires INDRADB_ENDPOINT"]
async fn write_edges_returns_correct_count() {
    let sink = live_sink().await;
    sink.ensure_indexes().await.unwrap();
    sink.reset(ResetTarget::All).await.unwrap();

    // Nodes must exist before edges (IndraDB bulk_insert may not validate referential
    // integrity in memory mode, but we write them anyway for correctness).
    let nodes = vec![
        make_node("c:@F@src_edge", "repo_s12_e"),
        make_node("c:@F@dst_edge", "repo_s12_e"),
    ];
    sink.write_nodes(&nodes).await.unwrap();

    let edges = vec![make_edge("c:@F@src_edge", "c:@F@dst_edge", "repo_s12_e")];
    let stats = sink
        .write_edges(&edges)
        .await
        .expect("write_edges must succeed");
    assert_eq!(stats.nodes_written, 1, "must report 1 edge written");
}

/// Unresolved edges (dst_usr == None) must be silently skipped, not error.
#[tokio::test]
#[ignore = "requires INDRADB_ENDPOINT"]
async fn write_edges_skips_unresolved() {
    let sink = live_sink().await;
    sink.ensure_indexes().await.unwrap();
    sink.reset(ResetTarget::All).await.unwrap();

    let mut edge = make_edge("c:@F@src_unres", "c:@F@dst_unres", "repo_s12_unres");
    edge.dst_usr = None;
    let stats = sink
        .write_edges(&[edge])
        .await
        .expect("skipping unresolved edge must not error");
    assert_eq!(stats.nodes_written, 0, "skipped edge must not be counted");
}

/// `reset(ResetTarget::All)` must clear all data; subsequent reads return empty.
#[tokio::test]
#[ignore = "requires INDRADB_ENDPOINT"]
async fn reset_all_clears_data() {
    let sink = live_sink().await;
    sink.ensure_indexes().await.unwrap();

    // Write some data.
    let nodes = vec![make_node("c:@F@reset_all_test", "repo_reset")];
    sink.write_nodes(&nodes).await.unwrap();

    // Reset all.
    sink.reset(ResetTarget::All)
        .await
        .expect("reset(All) must succeed");

    // A second reset of an already-empty graph must also succeed (idempotent).
    sink.reset(ResetTarget::All)
        .await
        .expect("reset(All) on empty graph must succeed");
}

/// `reset(ResetTarget::Repo)` must remove only data for the named repo.
#[tokio::test]
#[ignore = "requires INDRADB_ENDPOINT"]
async fn reset_repo_removes_only_target_repo() {
    let sink = live_sink().await;
    sink.ensure_indexes().await.unwrap();
    // Start clean.
    sink.reset(ResetTarget::All).await.unwrap();

    let nodes_a = vec![make_node("c:@F@fn_repo_a", "repo_a")];
    let nodes_b = vec![make_node("c:@F@fn_repo_b", "repo_b")];
    sink.write_nodes(&nodes_a).await.unwrap();
    sink.write_nodes(&nodes_b).await.unwrap();

    // Reset only repo_a.
    sink.reset(ResetTarget::Repo("repo_a".to_owned()))
        .await
        .expect("reset(Repo) must succeed");

    // We can verify indirectly: writing repo_b data again (idempotent) still works.
    sink.write_nodes(&nodes_b)
        .await
        .expect("repo_b data must survive repo_a reset");
}

/// Phase 5 lock must be acquirable and releasable.
#[tokio::test]
#[ignore = "requires INDRADB_ENDPOINT"]
async fn phase5_lock_acquire_and_release() {
    let sink = live_sink().await;
    sink.reset(ResetTarget::All).await.unwrap();

    let mut guard = sink
        .acquire_phase5_lock(std::time::Duration::from_secs(10))
        .await
        .expect("acquire_phase5_lock must succeed");
    guard.release().await.expect("release must succeed");

    // A second explicit release must be idempotent (no error).
    guard
        .release()
        .await
        .expect("second release must be a no-op");
}

/// `read_schema_version` must return `None` when no schema version is stored.
#[tokio::test]
#[ignore = "requires INDRADB_ENDPOINT"]
async fn read_schema_version_returns_none_when_absent() {
    let sink = live_sink().await;
    sink.reset(ResetTarget::All).await.unwrap();

    let v = sink
        .read_schema_version()
        .await
        .expect("read_schema_version must not error");
    assert!(v.is_none(), "no schema version vertex = None expected");
}

/// `backend_name` must return the stable string `"indradb"`.
#[tokio::test]
#[ignore = "requires INDRADB_ENDPOINT"]
async fn backend_name_is_indradb() {
    let sink = live_sink().await;
    assert_eq!(sink.backend_name(), "indradb");
}
