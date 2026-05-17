//! M2 exit-gate integration test (AC-M2-16, AC-M2-17).
//!
//! Indexes the vendored Boost.Optional fixture and asserts:
//!   - Zero `cross_repo_candidate=true` edges (all USRs resolve within-repo).
//!   - At least one `NodeKind::Specialization` node whose `qualified_name`
//!     contains `boost::optional`.
//!   - At least one `EdgeKind::Specializes` edge, confirming the partial
//!     specialization points to the primary template.
//!
//! # Fixture
//! `tests/fixtures/boost_optional/` is a minimal vendored fixture — it is NOT
//! real Boost.  It contains just enough C++ to exercise the
//! `ClassTemplate` → `TEMPLATE_DECL` and
//! `ClassTemplatePartialSpecialization` → `SPECIALIZATION` + `SPECIALIZES` path.
//!
//! # Gating
//! The test is `#[ignore]` because the exit-criteria command uses `-- --ignored`:
//!
//! ```sh
//! cargo nextest run -p cpp_indexer --test m2_exit_gate -- --ignored
//! ```

use std::path::PathBuf;
use std::sync::Arc;

use cpp_indexer::pipeline::{run, RunOptions};
use cpp_indexer::schema::{EdgeKind, NodeKind};
use cpp_indexer::sink::mock::{MockCall, MockSink};
use cpp_indexer::sink::GraphSink;

// ---------------------------------------------------------------------------
// Fixture helpers
// ---------------------------------------------------------------------------

fn fixture_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/boost_optional")
}

fn make_run_opts(stage_dir: PathBuf) -> RunOptions {
    let fixture = fixture_dir();
    let cc_path = fixture.join("compile_commands.json");
    RunOptions {
        input_path: fixture.clone(),
        compile_commands: Some(cc_path),
        repo_name: "boost_optional_fixture".to_owned(),
        stage_dir: Some(stage_dir),
        skip_phase2: true,
        // Keep system-header filter ON so no system-header USRs leak in.
        skip_system_headers: true,
        skip_cache: false,
        skip_repo_node: true,
    }
}

// ---------------------------------------------------------------------------
// M2 exit-gate test (requires boost-optional-checkout vendored fixture)
// ---------------------------------------------------------------------------

/// M2 exit gate: Boost.Optional fixture fully resolves (AC-M2-16, AC-M2-17).
///
/// Asserts:
///   1. Zero `cross_repo_candidate=true` edges.
///   2. At least one `SPECIALIZATION` node for `boost::optional`.
///   3. At least one `SPECIALIZES` edge.
#[tokio::test]
#[ignore = "requires boost-optional-checkout"]
async fn m2_boost_optional_fully_resolves() {
    let stage_dir = tempfile::tempdir().expect("tempdir");
    let sink = Arc::new(MockSink::default());
    let opts = make_run_opts(stage_dir.path().to_path_buf());

    let stats = run(Arc::clone(&sink) as Arc<dyn GraphSink>, opts)
        .await
        .expect("pipeline must succeed on boost_optional fixture");

    assert!(stats.tu_count > 0, "at least one TU must be processed");

    let calls = sink.calls();

    // Collect all nodes and edges from the call log.
    let mut all_nodes = Vec::new();
    let mut all_edges = Vec::new();
    for call in &calls {
        match call {
            MockCall::WriteNodes(nodes) => all_nodes.extend(nodes.iter().cloned()),
            MockCall::WriteEdges(edges) => all_edges.extend(edges.iter().cloned()),
            _ => {}
        }
    }

    // AC-M2-16: zero cross_repo_candidate=true edges.
    let cross_repo_edges: Vec<_> = all_edges
        .iter()
        .filter(|e| e.cross_repo_candidate)
        .collect();
    assert!(
        cross_repo_edges.is_empty(),
        "AC-M2-16: expected zero cross_repo_candidate edges, got {}: {:?}",
        cross_repo_edges.len(),
        cross_repo_edges
            .iter()
            .map(|e| format!("{} -> {:?}", e.src_usr, e.dst_usr))
            .collect::<Vec<_>>()
    );

    // AC-M2-17a: at least one Specialization node containing "optional".
    let spec_nodes: Vec<_> = all_nodes
        .iter()
        .filter(|n| {
            n.kind == NodeKind::Specialization
                && (n.qualified_name.contains("optional") || n.name.contains("optional"))
        })
        .collect();
    assert!(
        !spec_nodes.is_empty(),
        "AC-M2-17: expected at least one Specialization node for boost::optional, \
         found none. All Specialization nodes: {:?}",
        all_nodes
            .iter()
            .filter(|n| n.kind == NodeKind::Specialization)
            .map(|n| &n.qualified_name)
            .collect::<Vec<_>>()
    );

    // AC-M2-17b: at least one SPECIALIZES edge whose src_usr corresponds to a
    // boost::optional Specialization node (tighter than a bare edge count check).
    let spec_usrs: std::collections::HashSet<&str> =
        spec_nodes.iter().map(|n| n.usr.as_str()).collect();
    let boost_specializes_edges: Vec<_> = all_edges
        .iter()
        .filter(|e| e.kind == EdgeKind::Specializes && spec_usrs.contains(e.src_usr.as_str()))
        .collect();
    assert!(
        !boost_specializes_edges.is_empty(),
        "AC-M2-17: expected at least one SPECIALIZES edge from a boost::optional \
         Specialization node, got none. All SPECIALIZES edges: {:?}",
        all_edges
            .iter()
            .filter(|e| e.kind == EdgeKind::Specializes)
            .map(|e| format!("{} -> {:?}", e.src_usr, e.dst_usr))
            .collect::<Vec<_>>()
    );
}
