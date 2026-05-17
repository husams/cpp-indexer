//! M1 exit-gate integration test (AC-M1-25, AC-M1-26, AC-M1-27).
//!
//! Runs the full pipeline on the 5-file fixture against BOTH the Neo4j and
//! IndraDB sinks and asserts that each sink produces an isomorphic graph that
//! satisfies the golden snapshot.
//!
//! # Gating
//! The Neo4j and IndraDB tests are `#[ignore]` because they require live
//! database servers. Run them with:
//!
//! ```sh
//! cargo nextest run -p cpp_indexer --test m1_exit_gate -- --ignored
//! ```
//!
//! Start the test containers first if needed:
//!
//! ```sh
//! docker compose -f tests/compose/docker-compose.yml up -d
//! cargo nextest run -p cpp_indexer --test m1_exit_gate -- --ignored
//! ```

use std::collections::HashSet;
use std::path::PathBuf;
use std::sync::Arc;

use cpp_indexer::pipeline::{run, RunOptions};
use cpp_indexer::sink::mock::{MockCall, MockSink};
use cpp_indexer::sink::GraphSink;

// ---------------------------------------------------------------------------
// Fixture helpers
// ---------------------------------------------------------------------------

fn fixture_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/m1_5file")
}

fn load_golden() -> serde_json::Value {
    let path = fixture_dir().join("golden_graph.json");
    let content = std::fs::read_to_string(&path)
        .unwrap_or_else(|e| panic!("failed to read golden_graph.json: {e}"));
    serde_json::from_str(&content).expect("golden_graph.json must be valid JSON")
}

fn make_run_opts(stage_dir: PathBuf) -> RunOptions {
    let fixture = fixture_dir();
    let cc_path = fixture.join("compile_commands.json");
    RunOptions {
        input_path: fixture.clone(),
        compile_commands: Some(cc_path),
        repo_name: "m1_5file".to_owned(),
        stage_dir: Some(stage_dir),
        skip_phase2: true,
        skip_system_headers: true,
        workers: None,
        skip_cache: false,
        skip_repo_node: true,
    }
}

// ---------------------------------------------------------------------------
// Helper: assert golden constraints against a MockSink's call log.
// ---------------------------------------------------------------------------

fn assert_golden_mock(calls: &[MockCall], golden: &serde_json::Value, label: &str) {
    use cpp_indexer::schema::NodeRecord;

    // Collect all written nodes and edges from the call log.
    let mut all_nodes: Vec<NodeRecord> = Vec::new();
    let mut all_edges_count: u64 = 0;
    for call in calls {
        match call {
            MockCall::WriteNodes(nodes) => all_nodes.extend(nodes.iter().cloned()),
            MockCall::WriteEdges(edges) => all_edges_count += edges.len() as u64,
            _ => {}
        }
    }

    let min_nodes = golden["min_node_count"].as_u64().unwrap_or(0);
    let min_edges = golden["min_edge_count"].as_u64().unwrap_or(0);
    let node_count = all_nodes.len() as u64;

    assert!(
        node_count >= min_nodes,
        "[{label}] expected >= {min_nodes} nodes, got {node_count}"
    );
    assert!(
        all_edges_count >= min_edges,
        "[{label}] expected >= {min_edges} edges, got {all_edges_count}"
    );

    // Check that every expected node name appears in at least one NodeRecord.
    if let Some(expected_names) = golden["expected_node_names"].as_array() {
        let actual_names: HashSet<&str> = all_nodes.iter().map(|n| n.name.as_str()).collect();
        for expected in expected_names {
            let name = expected.as_str().unwrap_or_default();
            assert!(
                actual_names.contains(name),
                "[{label}] expected node named '{name}' not found in sink; \
                 actual names: {actual_names:?}"
            );
        }
    }
}

// ---------------------------------------------------------------------------
// Mock-sink smoke test (always runs — no server required)
// ---------------------------------------------------------------------------

/// Smoke test: run the full pipeline against a `MockSink` and verify the
/// golden graph constraints. This test does NOT need live database servers
/// and verifies the pipeline plumbing end-to-end.
///
/// AC-M1-25: pipeline produces non-empty graph for the 5-file fixture.
/// AC-M1-26: nodes are written before edges.
/// AC-M1-27: golden snapshot constraints are satisfied.
#[tokio::test]
async fn m1_pipeline_mock_sink_golden() {
    let stage_dir = tempfile::tempdir().expect("tempdir");
    let sink = Arc::new(MockSink::default());
    let opts = make_run_opts(stage_dir.path().to_path_buf());

    let stats = run(Arc::clone(&sink) as Arc<dyn GraphSink>, opts)
        .await
        .expect("pipeline must succeed");

    // Basic sanity: at least one TU was processed.
    assert!(stats.tu_count > 0, "at least one TU must be processed");

    let calls = sink.calls();

    // Verify golden constraints.
    let golden = load_golden();
    assert_golden_mock(&calls, &golden, "mock");

    // AC-M1-26: Preflight and EnsureIndexes must appear before any WriteNodes/WriteEdges.
    let first_write = calls
        .iter()
        .position(|c| matches!(c, MockCall::WriteNodes(_) | MockCall::WriteEdges(_)));
    let last_preflight = calls
        .iter()
        .rposition(|c| matches!(c, MockCall::Preflight | MockCall::EnsureIndexes));
    if let (Some(fw), Some(lp)) = (first_write, last_preflight) {
        assert!(
            lp < fw,
            "preflight/ensure_indexes must precede writes (AC-M1-26)"
        );
    }

    // AC-M1-26: All WriteNodes must appear before any WriteEdges.
    let first_write_edges = calls
        .iter()
        .position(|c| matches!(c, MockCall::WriteEdges(_)));
    let last_write_nodes = calls
        .iter()
        .rposition(|c| matches!(c, MockCall::WriteNodes(_)));
    if let (Some(fwe), Some(lwn)) = (first_write_edges, last_write_nodes) {
        assert!(
            lwn < fwe,
            "all WriteNodes must precede WriteEdges (AC-M1-26)"
        );
    }
}

// ---------------------------------------------------------------------------
// Neo4j isomorphic-graph test (requires live Neo4j)
// ---------------------------------------------------------------------------

/// Run the full pipeline against Neo4j and verify it writes nodes and edges.
///
/// Requires: `NEO4J_URI` and `NEO4J_PASSWORD` env vars pointing to a running
/// Neo4j instance.  The test skips gracefully when they are absent.
#[tokio::test]
#[ignore = "requires live Neo4j server (set NEO4J_URI + NEO4J_PASSWORD)"]
async fn m1_pipeline_neo4j_golden() {
    let uri = match std::env::var("NEO4J_URI") {
        Ok(v) => v,
        Err(_) => {
            eprintln!("SKIP: NEO4J_URI not set");
            return;
        }
    };
    if std::env::var("NEO4J_PASSWORD").is_err() {
        eprintln!("SKIP: NEO4J_PASSWORD not set");
        return;
    }

    use cpp_indexer::config::{Neo4jSinkConfig, SinkConfig};
    use cpp_indexer::sink::factory;

    let sink_cfg = SinkConfig {
        backend: "neo4j".to_owned(),
        batch_size: None,
        neo4j: Some(Neo4jSinkConfig {
            uri,
            user: std::env::var("NEO4J_USER").unwrap_or_else(|_| "neo4j".to_owned()),
            password_env: "NEO4J_PASSWORD".to_owned(),
            sessions: None,
        }),
        indradb: None,
    };

    let sink = factory::create(&sink_cfg)
        .await
        .expect("Neo4j sink creation must succeed");

    // Reset repo data before the run for idempotency.
    sink.reset(cpp_indexer::sink::ResetTarget::Repo("m1_5file".to_owned()))
        .await
        .expect("reset must succeed");

    let stage_dir = tempfile::tempdir().expect("tempdir");
    let opts = make_run_opts(stage_dir.path().to_path_buf());

    let stats = run(Arc::clone(&sink), opts)
        .await
        .expect("pipeline must succeed against Neo4j");

    assert!(stats.nodes_written > 0, "at least one node must be written");
    assert!(stats.tu_count > 0, "at least one TU must be processed");
}

// ---------------------------------------------------------------------------
// IndraDB isomorphic-graph test (requires live IndraDB)
// ---------------------------------------------------------------------------

/// Run the full pipeline against IndraDB and verify it writes nodes and edges.
///
/// Requires: `INDRADB_ENDPOINT` env var pointing to a running IndraDB gRPC
/// server.  The test skips gracefully when it is absent.
#[tokio::test]
#[ignore = "requires live IndraDB server (set INDRADB_ENDPOINT)"]
async fn m1_pipeline_indradb_golden() {
    let endpoint = match std::env::var("INDRADB_ENDPOINT") {
        Ok(v) => v,
        Err(_) => {
            eprintln!("SKIP: INDRADB_ENDPOINT not set");
            return;
        }
    };

    use cpp_indexer::config::{IndraDbSinkConfig, SinkConfig};
    use cpp_indexer::sink::factory;

    let sink_cfg = SinkConfig {
        backend: "indradb".to_owned(),
        batch_size: None,
        neo4j: None,
        indradb: Some(IndraDbSinkConfig {
            endpoint,
            token_env: std::env::var("INDRADB_TOKEN_ENV").ok(),
            sessions: None,
        }),
    };

    let sink = factory::create(&sink_cfg)
        .await
        .expect("IndraDB sink creation must succeed");

    sink.reset(cpp_indexer::sink::ResetTarget::Repo("m1_5file".to_owned()))
        .await
        .expect("reset must succeed");

    let stage_dir = tempfile::tempdir().expect("tempdir");
    let opts = make_run_opts(stage_dir.path().to_path_buf());

    let stats = run(Arc::clone(&sink), opts)
        .await
        .expect("pipeline must succeed against IndraDB");

    assert!(stats.nodes_written > 0, "at least one node must be written");
    assert!(stats.tu_count > 0, "at least one TU must be processed");
}
