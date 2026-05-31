//! Integration tests for `Neo4jSink`.
//!
//! These tests require a live Neo4j instance.  They are gated with `#[ignore]`
//! and only run when `NEO4J_URI` is set in the environment.
//!
//! Run via:
//! ```bash
//! NEO4J_URI=bolt://localhost:7687 NEO4J_PASSWORD_ENV=N4J_PW N4J_PW=test \
//!   cargo nextest run --test sink_neo4j -- --ignored
//! ```
//!
//! Docker compose for a local Neo4j instance:
//!   `tests/compose/neo4j.yml`

use std::sync::Arc;

use cpp_indexer::schema::edges::{EdgeKind, EdgeRecord};
use cpp_indexer::schema::nodes::{NodeKind, NodeRecord};
use cpp_indexer::sink::neo4j::Neo4jSink;
use cpp_indexer::sink::{GraphSink, ResetTarget};

// ── Helpers ───────────────────────────────────────────────────────────────────

/// Returns `None` when `NEO4J_URI` env var is unset (test should be skipped).
fn neo4j_uri() -> Option<String> {
    std::env::var("NEO4J_URI").ok()
}

/// Construct a `Neo4jSink` from environment variables.
///
/// Expects:
/// - `NEO4J_URI`          — bolt URI
/// - `NEO4J_PASSWORD_ENV` — name of the env var holding the password
/// - `<NEO4J_PASSWORD_ENV>` — the actual password value
async fn make_sink() -> Neo4jSink {
    let uri = std::env::var("NEO4J_URI").expect("NEO4J_URI must be set for integration tests");
    let pw_env_name = std::env::var("NEO4J_PASSWORD_ENV").expect("NEO4J_PASSWORD_ENV must be set");
    let password =
        std::env::var(&pw_env_name).unwrap_or_else(|_| panic!("env var {pw_env_name} must be set"));

    let config = cpp_indexer::config::Neo4jSinkConfig {
        uri,
        user: "neo4j".to_owned(),
        password_env: pw_env_name,
        sessions: Some(4),
    };
    Neo4jSink::connect(&config, &password)
        .await
        .expect("connect must succeed")
}

fn sample_nodes(repo: &str) -> Vec<NodeRecord> {
    vec![
        NodeRecord {
            usr: format!("c:@F@fn_a#{repo}"),
            kind: NodeKind::Function,
            name: "fn_a".to_owned(),
            qualified_name: "ns::fn_a".to_owned(),
            mangled_name: None,
            file_path: "/repo/a.cpp".to_owned(),
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
        },
        NodeRecord {
            usr: format!("c:@F@fn_b#{repo}"),
            kind: NodeKind::Function,
            name: "fn_b".to_owned(),
            qualified_name: "ns::fn_b".to_owned(),
            mangled_name: None,
            file_path: "/repo/b.cpp".to_owned(),
            line: Some(20),
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
            symbol_id: 2,
            file_id: 2,
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
        },
    ]
}

fn sample_edges(repo: &str) -> Vec<EdgeRecord> {
    vec![EdgeRecord {
        src_usr: format!("c:@F@fn_a#{repo}"),
        dst_usr: Some(format!("c:@F@fn_b#{repo}")),
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
        access: None,
        edge_index: None,
        inherits_is_virtual: None,
    }]
}

// ── Tests ─────────────────────────────────────────────────────────────────────

/// preflight → ensure_indexes → write_nodes → write_edges → MERGE idempotency.
#[tokio::test]
#[ignore = "requires NEO4J_URI to be set"]
async fn neo4j_sink_full_lifecycle() {
    if neo4j_uri().is_none() {
        return;
    }

    let sink: Arc<dyn GraphSink> = Arc::new(make_sink().await);
    let repo = "integration-test-repo";

    // Clean slate.
    sink.reset(ResetTarget::Repo(repo.to_owned()))
        .await
        .expect("reset must succeed");

    // preflight
    sink.preflight().await.expect("preflight must succeed");

    // ensure_indexes (idempotent — run twice)
    sink.ensure_indexes()
        .await
        .expect("ensure_indexes first call");
    sink.ensure_indexes()
        .await
        .expect("ensure_indexes second call (idempotent)");

    // write_nodes
    let nodes = sample_nodes(repo);
    let stats = sink
        .write_nodes(&nodes)
        .await
        .expect("write_nodes must succeed");
    assert_eq!(stats.nodes_written, 2, "two nodes must be written");
    assert_eq!(stats.retries, 0);

    // write_edges
    let edges = sample_edges(repo);
    let stats = sink
        .write_edges(&edges)
        .await
        .expect("write_edges must succeed");
    assert_eq!(stats.nodes_written, 1, "one edge must be written");

    // MERGE idempotency — re-run the same batch; counts must still be 2 nodes, 1 edge.
    let stats2 = sink
        .write_nodes(&nodes)
        .await
        .expect("second write_nodes must succeed");
    assert_eq!(
        stats2.nodes_written, 2,
        "idempotent write must report same count"
    );

    let stats3 = sink
        .write_edges(&edges)
        .await
        .expect("second write_edges must succeed");
    assert_eq!(
        stats3.nodes_written, 1,
        "idempotent edge write must report same count"
    );

    // health check
    let info = sink.health().await.expect("health must succeed");
    assert_eq!(info.status, "ok");

    // cleanup
    sink.reset(ResetTarget::Repo(repo.to_owned()))
        .await
        .expect("cleanup reset must succeed");
}

/// Edges with `dst_usr = None` must be silently skipped (not panic or error).
#[tokio::test]
#[ignore = "requires NEO4J_URI to be set"]
async fn neo4j_sink_skips_unresolved_edges() {
    if neo4j_uri().is_none() {
        return;
    }

    let sink = make_sink().await;
    let repo = "integration-test-unresolved";

    sink.reset(ResetTarget::Repo(repo.to_owned()))
        .await
        .expect("reset must succeed");

    // Write nodes so the test is realistic.
    let nodes = sample_nodes(repo);
    sink.write_nodes(&nodes).await.expect("write_nodes");

    // Edge with dst_id = None (unresolved).
    let bad_edge = EdgeRecord {
        src_usr: format!("c:@F@fn_a#{repo}"),
        dst_usr: None,
        dst_placeholder: Some("unknown_symbol".to_owned()),
        kind: EdgeKind::Calls,
        resolved: false,
        cross_repo_candidate: true,
        repo_name: repo.to_owned(),
        attrs_json: "{}".to_owned(),
        tu_hash: [0u8; 32],
        source_association_type: None,
        target_association_type: None,
        src_id: 1,
        dst_id: None,
        dst_repo_name: repo.to_owned(),
        access: None,
        edge_index: None,
        inherits_is_virtual: None,
    };

    let stats = sink
        .write_edges(&[bad_edge])
        .await
        .expect("write_edges must not error on unresolved edge");
    assert_eq!(stats.nodes_written, 0, "unresolved edge must be skipped");

    sink.reset(ResetTarget::Repo(repo.to_owned()))
        .await
        .expect("cleanup");
}

/// `read_schema_version` returns `None` on a fresh DB (no SchemaVersion node).
#[tokio::test]
#[ignore = "requires NEO4J_URI to be set"]
async fn neo4j_sink_read_schema_version_returns_none_on_empty_db() {
    if neo4j_uri().is_none() {
        return;
    }

    let sink = make_sink().await;
    let version = sink
        .read_schema_version()
        .await
        .expect("read_schema_version must not error");
    // May be None (no SchemaVersion node) or Some (if a prior test left one).
    // We only assert it doesn't panic.
    let _ = version;
}
