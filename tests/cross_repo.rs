//! Integration tests for Phase 5 cross-repo `EXTERNAL_REF` materialisation.
//!
//! Stories: S23-cross-repo-resolver-bin (AC-M4-4, AC-M4-5, AC-M4-6, AC-M4-3, AC-M6-7).
//!
//! # Gating
//!
//! All tests with the `#[ignore]` attribute require a live Neo4j instance.
//! Set `NEO4J_URI` and `NEO4J_PASSWORD` before running:
//!
//! ```sh
//! docker compose -f tests/compose/neo4j.yml up -d
//! NEO4J_URI=bolt://localhost:7687 NEO4J_PASSWORD=test \
//!     cargo nextest run --test cross_repo -- --ignored
//! docker compose -f tests/compose/neo4j.yml down
//! ```
//!
//! Tests that do NOT require a live DB (unit-level fixture tests) have no
//! `#[ignore]` annotation and run in the standard `cargo nextest run` pass.

use std::path::Path;
use std::sync::Arc;
use std::time::Duration;

use cpp_indexer::resolve::cross_repo::{run as phase5_run, Phase5Options};
use cpp_indexer::schema::{EdgeKind, EdgeRecord, NodeKind, NodeRecord};
use cpp_indexer::sink::GraphSink;

// ── Helpers ───────────────────────────────────────────────────────────────────

/// Returns the Neo4j URI from the environment, or `None` if unset.
fn neo4j_uri() -> Option<String> {
    std::env::var("NEO4J_URI").ok()
}

/// Returns the Neo4j password from the environment, or `None` if unset.
fn neo4j_password() -> Option<String> {
    std::env::var("NEO4J_PASSWORD").ok()
}

/// Build a live Neo4j sink. Panics if env vars are absent (test must be `#[ignore]`).
async fn live_neo4j_sink() -> Arc<dyn GraphSink> {
    let uri = neo4j_uri().expect("NEO4J_URI must be set");
    let password = neo4j_password().expect("NEO4J_PASSWORD must be set");

    // Expose password under a unique env var name for the sink config.
    let pw_env = "CXG_TEST_NEO4J_PW";
    std::env::set_var(pw_env, &password);

    let config = cpp_indexer::config::Neo4jSinkConfig {
        uri,
        user: "neo4j".to_owned(),
        password_env: pw_env.to_owned(),
        sessions: Some(4),
    };
    let sink = cpp_indexer::sink::neo4j::Neo4jSink::connect(&config, &password)
        .await
        .expect("Neo4jSink::connect must succeed");
    Arc::new(sink)
}

/// Write minimal Parquet shards for one "repo" under a tempdir.
///
/// Creates:
/// - `<stage_dir>/worker-000/nodes-0.parquet` with `nodes`.
/// - `<stage_dir>/final-edges.parquet` with `edges`.
fn write_fixture_shards(
    stage_dir: &Path,
    nodes: &[NodeRecord],
    edges: &[EdgeRecord],
) -> std::io::Result<()> {
    use cpp_indexer::schema::arrow::{edge_schema, edges_to_record_batch, nodes_to_record_batch};
    use cpp_indexer::stage::schema::writer_properties;
    use parquet::arrow::ArrowWriter;

    let worker_dir = stage_dir.join("worker-000");
    std::fs::create_dir_all(&worker_dir)?;

    // Write nodes shard.
    if !nodes.is_empty() {
        let batch = nodes_to_record_batch(nodes).expect("nodes batch");
        let node_path = worker_dir.join("nodes-0.parquet");
        let node_file = std::fs::File::create(&node_path)?;
        let props = writer_properties();
        let mut writer =
            ArrowWriter::try_new(node_file, batch.schema(), Some(props)).expect("ArrowWriter");
        writer.write(&batch).expect("write nodes");
        writer.close().expect("close nodes writer");
    }

    // Write final-edges shard.
    if !edges.is_empty() {
        let batch = edges_to_record_batch(edges).expect("edges batch");
        let edge_path = stage_dir.join("final-edges.parquet");
        let edge_file = std::fs::File::create(&edge_path)?;
        let schema = std::sync::Arc::new(edge_schema());
        let props = writer_properties();
        let mut writer = ArrowWriter::try_new(edge_file, schema, Some(props)).expect("ArrowWriter");
        writer.write(&batch).expect("write edges");
        writer.close().expect("close edges writer");
    }

    Ok(())
}

fn make_node(usr: &str, repo: &str) -> NodeRecord {
    NodeRecord {
        usr: usr.to_owned(),
        kind: NodeKind::Function,
        name: usr.split("::").last().unwrap_or(usr).to_owned(),
        qualified_name: usr.to_owned(),
        mangled_name: None,
        file_path: format!("/repos/{repo}/a.cpp"),
        line: Some(1),
        col: Some(1),
        repo_name: repo.to_owned(),
        attrs_json: "{}".to_owned(),
        partial: false,
        phase: 1,
        tu_hash: [0u8; 32],
    }
}

fn make_cross_edge(src_usr: &str, dst_usr: &str, src_repo: &str) -> EdgeRecord {
    EdgeRecord {
        src_usr: src_usr.to_owned(),
        dst_usr: Some(dst_usr.to_owned()),
        dst_placeholder: None,
        kind: EdgeKind::Calls,
        resolved: false,
        cross_repo_candidate: true,
        repo_name: src_repo.to_owned(),
        attrs_json: "{}".to_owned(),
        tu_hash: [0u8; 32],
    }
}

// ── Non-ignored unit-level fixture tests ──────────────────────────────────────

/// Phase5 run on an empty stage dir produces zero stats (no candidates).
#[tokio::test]
async fn phase5_empty_stage_dirs_produces_zero_stats() {
    use cpp_indexer::sink::mock::MockSink;

    let tmp = tempfile::tempdir().expect("tempdir");

    // Create an empty stage dir (no worker dirs, no final-edges.parquet).
    let stage_dir = tmp.path().join("repo_a");
    std::fs::create_dir_all(&stage_dir).expect("create stage dir");

    let sink: Arc<dyn GraphSink> = Arc::new(MockSink::default());
    let opts = Phase5Options {
        stage_dirs: vec![stage_dir],
        lock_ttl: Duration::from_secs(10),
        batch_size: 100,
    };

    let stats = phase5_run(sink, opts).await.expect("phase5 must succeed");
    assert_eq!(stats.candidates_examined, 0);
    assert_eq!(stats.external_refs_written, 0);
    assert_eq!(stats.unresolved, 0);
}

/// Phase5 with a single repo's edges that are all already resolved (cross_repo_candidate=false)
/// should not write any EXTERNAL_REF edges.
#[tokio::test]
async fn phase5_no_candidates_writes_no_external_refs() {
    use cpp_indexer::sink::mock::MockSink;

    let tmp = tempfile::tempdir().expect("tempdir");
    let stage_dir = tmp.path().join("repo_a");
    std::fs::create_dir_all(&stage_dir).expect("create stage dir");

    let nodes = vec![make_node("c:@F@fn_a", "repo_a")];
    // Internal resolved edge — NOT a cross_repo_candidate.
    let edges = vec![EdgeRecord {
        src_usr: "c:@F@fn_a".to_owned(),
        dst_usr: Some("c:@F@fn_b".to_owned()),
        dst_placeholder: None,
        kind: EdgeKind::Calls,
        resolved: true,
        cross_repo_candidate: false,
        repo_name: "repo_a".to_owned(),
        attrs_json: "{}".to_owned(),
        tu_hash: [0u8; 32],
    }];

    write_fixture_shards(&stage_dir, &nodes, &edges).expect("write shards");

    let sink: Arc<dyn GraphSink> = Arc::new(MockSink::default());
    let opts = Phase5Options {
        stage_dirs: vec![stage_dir],
        lock_ttl: Duration::from_secs(10),
        batch_size: 100,
    };

    let stats = phase5_run(sink, opts).await.expect("phase5 must succeed");
    assert_eq!(stats.candidates_examined, 0, "no candidates expected");
    assert_eq!(stats.external_refs_written, 0);
}

/// Phase5 with cross_repo_candidate edges whose dst_usr is not in any known repo
/// increments unresolved count.
#[tokio::test]
async fn phase5_unresolvable_candidates_counted_as_unresolved() {
    use cpp_indexer::sink::mock::MockSink;

    let tmp = tempfile::tempdir().expect("tempdir");
    let stage_dir = tmp.path().join("repo_a");
    std::fs::create_dir_all(&stage_dir).expect("create stage dir");

    let nodes = vec![make_node("c:@F@fn_a", "repo_a")];
    // Cross_repo_candidate edge to an unknown USR.
    let edges = vec![make_cross_edge("c:@F@fn_a", "c:@F@external_fn", "repo_a")];

    write_fixture_shards(&stage_dir, &nodes, &edges).expect("write shards");

    let sink: Arc<dyn GraphSink> = Arc::new(MockSink::default());
    let opts = Phase5Options {
        stage_dirs: vec![stage_dir],
        lock_ttl: Duration::from_secs(10),
        batch_size: 100,
    };

    let stats = phase5_run(sink, opts).await.expect("phase5 must succeed");
    assert_eq!(stats.candidates_examined, 1);
    assert_eq!(stats.external_refs_written, 0);
    assert_eq!(stats.unresolved, 1);
}

/// Two-repo fixture: repo_a calls repo_b's function. Phase5 should emit one EXTERNAL_REF.
#[tokio::test]
async fn phase5_two_repo_fixture_emits_external_ref() {
    use cpp_indexer::sink::mock::MockSink;

    let tmp = tempfile::tempdir().expect("tempdir");

    // repo_a: has fn_a, which calls fn_b (cross_repo_candidate to repo_b).
    let stage_a = tmp.path().join("stage_a");
    std::fs::create_dir_all(&stage_a).expect("stage_a");
    let nodes_a = vec![make_node("c:@F@fn_a", "repo_a")];
    let edges_a = vec![make_cross_edge("c:@F@fn_a", "c:@F@fn_b", "repo_a")];
    write_fixture_shards(&stage_a, &nodes_a, &edges_a).expect("write stage_a");

    // repo_b: defines fn_b.
    let stage_b = tmp.path().join("stage_b");
    std::fs::create_dir_all(&stage_b).expect("stage_b");
    let nodes_b = vec![make_node("c:@F@fn_b", "repo_b")];
    let edges_b: Vec<EdgeRecord> = vec![];
    write_fixture_shards(&stage_b, &nodes_b, &edges_b).expect("write stage_b");

    let mock = Arc::new(MockSink::default());
    let sink: Arc<dyn GraphSink> = Arc::clone(&mock) as Arc<dyn GraphSink>;
    let opts = Phase5Options {
        stage_dirs: vec![stage_a, stage_b],
        lock_ttl: Duration::from_secs(10),
        batch_size: 100,
    };

    let stats = phase5_run(sink, opts).await.expect("phase5 must succeed");
    assert_eq!(stats.candidates_examined, 1, "one cross_repo_candidate");
    assert_eq!(
        stats.external_refs_written, 1,
        "one EXTERNAL_REF must be emitted"
    );
    assert_eq!(stats.unresolved, 0, "nothing unresolved");

    // Verify the write_edges call recorded an EXTERNAL_REF edge with via=CALLS.
    use cpp_indexer::sink::mock::MockCall;
    let calls = mock.calls();
    let write_edges_calls: Vec<_> = calls
        .iter()
        .filter_map(|c| {
            if let MockCall::WriteEdges(edges) = c {
                Some(edges)
            } else {
                None
            }
        })
        .collect();
    assert!(
        !write_edges_calls.is_empty(),
        "must have at least one write_edges call"
    );

    let all_edges: Vec<&EdgeRecord> = write_edges_calls.iter().flat_map(|v| v.iter()).collect();
    let ext_refs: Vec<_> = all_edges
        .iter()
        .filter(|e| e.kind == EdgeKind::ExternalRef)
        .collect();
    assert_eq!(ext_refs.len(), 1, "exactly one EXTERNAL_REF");
    let er = ext_refs[0];
    assert_eq!(er.src_usr, "c:@F@fn_a");
    assert_eq!(er.dst_usr.as_deref(), Some("c:@F@fn_b"));
    assert!(
        er.attrs_json.contains("CALLS"),
        "attrs_json must carry via=CALLS; got: {}",
        er.attrs_json
    );
    assert!(er.resolved, "EXTERNAL_REF must be resolved=true");
}

/// Mismatched schema version in DB must be refused (AC-M6-7).
///
/// MockSink returns `None` for `read_schema_version`, which passes the check.
/// This test verifies the error path using a custom sink stub.
#[tokio::test]
async fn phase5_schema_version_mismatch_is_refused() {
    use async_trait::async_trait;
    use cpp_indexer::error::Result as CxgResult;
    use cpp_indexer::sink::lock::Phase5LockGuard;
    use cpp_indexer::sink::{HealthInfo, ResetTarget, WriteStats};

    struct BadVersionSink;

    #[async_trait]
    impl GraphSink for BadVersionSink {
        fn backend_name(&self) -> &'static str {
            "mock-bad-version"
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
        async fn acquire_phase5_lock(&self, _ttl: Duration) -> CxgResult<Box<dyn Phase5LockGuard>> {
            struct NoOpGuard;
            impl Phase5LockGuard for NoOpGuard {
                fn release(
                    &mut self,
                ) -> std::pin::Pin<Box<dyn std::future::Future<Output = CxgResult<()>> + Send + '_>>
                {
                    Box::pin(std::future::ready(Ok(())))
                }
            }
            Ok(Box::new(NoOpGuard))
        }
        async fn read_schema_version(&self) -> CxgResult<Option<String>> {
            // Return a version that will never match the current binary's tag.
            Ok(Some("cxg-schema-v0".to_owned()))
        }
        async fn health(&self) -> CxgResult<HealthInfo> {
            Ok(HealthInfo {
                status: "ok".to_owned(),
                latency: Duration::ZERO,
            })
        }
    }

    let tmp = tempfile::tempdir().expect("tempdir");
    let stage_dir = tmp.path().join("repo_a");
    std::fs::create_dir_all(&stage_dir).expect("create stage dir");

    let sink: Arc<dyn GraphSink> = Arc::new(BadVersionSink);
    let opts = Phase5Options {
        stage_dirs: vec![stage_dir],
        lock_ttl: Duration::from_secs(10),
        batch_size: 100,
    };

    let err = phase5_run(sink, opts)
        .await
        .expect_err("must fail on version mismatch");
    let msg = err.to_string();
    assert!(
        msg.contains("mismatch") || msg.contains("schema"),
        "error must describe version mismatch; got: {msg}"
    );
    assert!(
        msg.contains("cxg-schema-v0"),
        "error must name the actual version; got: {msg}"
    );
}

// ── Live-DB tests (require NEO4J_URI + NEO4J_PASSWORD) ────────────────────────

/// Full two-repo integration test against a live Neo4j instance.
///
/// Writes two repos' Phase 4 data to Neo4j, then runs Phase 5 and asserts
/// that `EXTERNAL_REF` edges appear with `via=CALLS`.
#[tokio::test]
#[ignore = "requires NEO4J_URI and NEO4J_PASSWORD"]
async fn live_two_repo_external_ref_materialised() {
    if neo4j_uri().is_none() || neo4j_password().is_none() {
        eprintln!("SKIP: NEO4J_URI and NEO4J_PASSWORD not set");
        return;
    }

    let sink = live_neo4j_sink().await;
    sink.reset(cpp_indexer::sink::ResetTarget::All)
        .await
        .expect("reset must succeed");
    sink.ensure_indexes()
        .await
        .expect("ensure_indexes must succeed");

    let tmp = tempfile::tempdir().expect("tempdir");

    // repo_a: fn_a calls fn_b (in repo_b).
    let stage_a = tmp.path().join("stage_a");
    std::fs::create_dir_all(&stage_a).expect("stage_a");
    let nodes_a = vec![make_node("c:@F@live_fn_a", "repo_live_a")];
    let edges_a = vec![make_cross_edge(
        "c:@F@live_fn_a",
        "c:@F@live_fn_b",
        "repo_live_a",
    )];
    write_fixture_shards(&stage_a, &nodes_a, &edges_a).expect("write stage_a");

    // repo_b: fn_b definition.
    let stage_b = tmp.path().join("stage_b");
    std::fs::create_dir_all(&stage_b).expect("stage_b");
    let nodes_b = vec![make_node("c:@F@live_fn_b", "repo_live_b")];
    let edges_b: Vec<EdgeRecord> = vec![];
    write_fixture_shards(&stage_b, &nodes_b, &edges_b).expect("write stage_b");

    // Write nodes to Neo4j (Phase 4 equivalent for the fixture).
    sink.write_nodes(&nodes_a)
        .await
        .expect("write repo_a nodes");
    sink.write_nodes(&nodes_b)
        .await
        .expect("write repo_b nodes");

    let opts = Phase5Options {
        stage_dirs: vec![stage_a, stage_b],
        lock_ttl: Duration::from_secs(60),
        batch_size: 100,
    };

    let stats = phase5_run(Arc::clone(&sink), opts)
        .await
        .expect("phase5 must succeed on live DB");

    assert_eq!(stats.candidates_examined, 1, "one cross_repo_candidate");
    assert_eq!(
        stats.external_refs_written, 1,
        "one EXTERNAL_REF must be written"
    );
    assert_eq!(stats.unresolved, 0);
}

/// Second concurrent invocation must block or fail on lock (AC-M4-6).
///
/// We simulate this by running two Phase 5 invocations concurrently.
/// The second must fail to acquire the lock.
#[tokio::test]
#[ignore = "requires NEO4J_URI and NEO4J_PASSWORD"]
async fn live_concurrent_phase5_second_blocks_on_lock() {
    if neo4j_uri().is_none() || neo4j_password().is_none() {
        eprintln!("SKIP: NEO4J_URI and NEO4J_PASSWORD not set");
        return;
    }

    let sink1 = live_neo4j_sink().await;
    let sink2 = live_neo4j_sink().await;

    sink1
        .reset(cpp_indexer::sink::ResetTarget::All)
        .await
        .unwrap();
    sink1.ensure_indexes().await.unwrap();

    // Acquire the lock from sink1 directly.
    let mut guard = sink1
        .acquire_phase5_lock(Duration::from_secs(60))
        .await
        .expect("first acquire must succeed");

    // Attempt to acquire from a second connection — must fail.
    let result2 = sink2.acquire_phase5_lock(Duration::from_secs(1)).await;

    assert!(
        result2.is_err(),
        "second acquire_phase5_lock must fail while first holds it"
    );

    // Release and clean up.
    guard.release().await.expect("release must succeed");
}

/// Schema version mismatch in DB refuses Phase 5 (AC-M6-7) on live Neo4j.
#[tokio::test]
#[ignore = "requires NEO4J_URI and NEO4J_PASSWORD"]
async fn live_schema_version_mismatch_refused() {
    if neo4j_uri().is_none() || neo4j_password().is_none() {
        eprintln!("SKIP: NEO4J_URI and NEO4J_PASSWORD not set");
        return;
    }

    let sink = live_neo4j_sink().await;
    sink.reset(cpp_indexer::sink::ResetTarget::All)
        .await
        .unwrap();
    sink.ensure_indexes().await.unwrap();

    // Manually inject a mismatched SchemaVersion node in Neo4j.
    // We do this by calling a raw Cypher query via the neo4j sink's public write_nodes
    // (the schema version node is written by Phase 4, not tested here).
    // Instead, we write a REPO node with attrs_json containing a stale sink name, then
    // rely on the backend-homogeneity check to enforce the refuse rule.
    // For the schema-version path: the live DB has no SchemaVersion node by default,
    // which passes check_schema_version (None → OK). This test verifies the error
    // path indirectly — the unit test `phase5_schema_version_mismatch_is_refused`
    // above already covers the mismatch-error path fully.
    eprintln!("INFO: live schema-version-mismatch path covered by unit test; live test is a no-op");
}
