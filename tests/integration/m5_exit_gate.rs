//! M5 exit-gate integration test (AC-M5-10, AC-M5-11).
//!
//! Two tests are provided:
//!
//! 1. **`m5_synthetic_macro_template_gate`** — runs the pipeline on a small
//!    but macro-heavy + template-heavy synthetic fixture
//!    (`tests/fixtures/m5_macro_template/`).  Asserts:
//!      - Pipeline returns `Ok` (proxy for "exit 0, no segfault") — AC-M5-10.
//!      - At least one `NodeKind::Macro` node in the MockSink output — AC-M5-11.
//!      - At least one `EdgeKind::ExpandsTo` edge in the MockSink output — AC-M5-11.
//!
//! 2. **`m5_chromium_subset_gate`** — runs the same assertions against a real
//!    Chromium `base/` + `net/` subtree.  Gated behind the
//!    `CXG_M5_CHROMIUM_PATH` environment variable; skipped when unset.
//!    See `tests/fixtures/chromium_subset.md` for acquisition instructions.
//!
//! # Running
//!
//! Both tests are `#[ignore]` (the exit-criteria command passes `-- --ignored`):
//!
//! ```sh
//! cargo nextest run -p cpp_indexer --test m5_exit_gate -- --ignored
//! ```
//!
//! For the Chromium gate:
//!
//! ```sh
//! CXG_M5_CHROMIUM_PATH=/data/chromium/src \
//!     cargo nextest run -p cpp_indexer --test m5_exit_gate -- --ignored
//! ```

use std::path::PathBuf;
use std::sync::Arc;

use cpp_indexer::pipeline::{run, RunOptions};
use cpp_indexer::schema::{EdgeKind, NodeKind};
use cpp_indexer::sink::mock::{MockCall, MockSink};
use cpp_indexer::sink::GraphSink;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

fn synthetic_fixture_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/m5_macro_template")
}

fn make_run_opts(fixture: PathBuf, cc_path: Option<PathBuf>, stage_dir: PathBuf) -> RunOptions {
    RunOptions {
        input_path: fixture.clone(),
        compile_commands: cc_path.or_else(|| Some(fixture.join("compile_commands.json"))),
        repo_name: "m5_macro_template".to_owned(),
        stage_dir: Some(stage_dir),
        skip_phase2: true,
        skip_system_headers: true,
        skip_cache: false,
        skip_repo_node: true,
    }
}

/// Collect all MACRO nodes and EXPANDS_TO edges from a MockSink call log.
fn extract_macro_counts(calls: &[MockCall]) -> (usize, usize) {
    let mut macro_nodes = 0usize;
    let mut expands_to_edges = 0usize;
    for call in calls {
        match call {
            MockCall::WriteNodes(nodes) => {
                macro_nodes += nodes.iter().filter(|n| n.kind == NodeKind::Macro).count();
            }
            MockCall::WriteEdges(edges) => {
                expands_to_edges += edges
                    .iter()
                    .filter(|e| e.kind == EdgeKind::ExpandsTo)
                    .count();
            }
            _ => {}
        }
    }
    (macro_nodes, expands_to_edges)
}

// ---------------------------------------------------------------------------
// Test 1: synthetic macro-heavy + template-heavy fixture
// ---------------------------------------------------------------------------

/// M5 exit gate (synthetic): macro-heavy + template-heavy fixture produces
/// ≥1 MACRO node and ≥1 EXPANDS_TO edge (AC-M5-10, AC-M5-11).
///
/// The fixture (`tests/fixtures/m5_macro_template/`) defines:
///   - Object-like macros: `VERSION_MAJOR`, `VERSION_MINOR`, `VERSION_STR`,
///     `ASSERT_TRUE`, `MAX_VALUE`, `MIN_VALUE`.
///   - Function-like macros: `MAKE_PAIR`, `CHECK_NULL`, `CLAMP`.
///   - X-macro pattern via `HANDLE_EVENT`.
///   - Class template `Container<T>` with partial specialisation `Container<T*>`.
///   - Function template `clamp<T>`.
///   - Multiple call sites at function scope (→ EXPANDS_TO edges).
#[tokio::test]
#[ignore = "chromium-gate family; run with -- --ignored"]
async fn m5_synthetic_macro_template_gate() {
    let stage_dir = tempfile::tempdir().expect("tempdir");
    let fixture = synthetic_fixture_dir();
    let sink = Arc::new(MockSink::default());

    let opts = make_run_opts(fixture, None, stage_dir.path().to_path_buf());

    // AC-M5-10: pipeline must complete without panic/segfault → Result::Ok.
    let stats = run(Arc::clone(&sink) as Arc<dyn GraphSink>, opts)
        .await
        .expect("AC-M5-10: pipeline must succeed on m5_macro_template fixture");

    assert!(
        stats.tu_count > 0,
        "at least one TU must be processed; fixture has main.cpp"
    );

    let calls = sink.calls();
    let (macro_nodes, expands_to_edges) = extract_macro_counts(&calls);

    // AC-M5-11: ≥1 MACRO node in Parquet output (via MockSink).
    assert!(
        macro_nodes >= 1,
        "AC-M5-11: expected ≥1 MACRO node, got {macro_nodes}; \
         check that detailed_preprocessing_record is on in shallow.rs"
    );

    // AC-M5-11: ≥1 EXPANDS_TO edge in Parquet output (via MockSink).
    assert!(
        expands_to_edges >= 1,
        "AC-M5-11: expected ≥1 EXPANDS_TO edge, got {expands_to_edges}; \
         check that macro expansion visitor fires for function-scope expansions"
    );

    eprintln!(
        "m5_synthetic_macro_template_gate: TUs={} macro_nodes={} expands_to_edges={}",
        stats.tu_count, macro_nodes, expands_to_edges
    );
}

// ---------------------------------------------------------------------------
// Test 2: real Chromium base/ + net/ subtree (requires checkout)
// ---------------------------------------------------------------------------

/// M5 exit gate (Chromium subset): index Chromium `base/` + `net/` subtree,
/// assert exit 0 and ≥1 MACRO + ≥1 EXPANDS_TO (AC-M5-10, AC-M5-11).
///
/// Set `CXG_M5_CHROMIUM_PATH` to the Chromium `src/` directory before running.
/// Optionally set `CXG_M5_CHROMIUM_CC` to an explicit `compile_commands.json`
/// path (auto-detected otherwise).
///
/// See `tests/fixtures/chromium_subset.md` for acquisition instructions.
#[tokio::test]
#[ignore = "requires-chromium: set CXG_M5_CHROMIUM_PATH=/data/chromium/src"]
async fn m5_chromium_subset_gate() {
    let chromium_path = match std::env::var("CXG_M5_CHROMIUM_PATH") {
        Ok(p) => PathBuf::from(p),
        Err(_) => {
            eprintln!(
                "SKIP m5_chromium_subset_gate: CXG_M5_CHROMIUM_PATH not set. \
                 See tests/fixtures/chromium_subset.md for setup instructions."
            );
            return;
        }
    };

    if !chromium_path.exists() {
        eprintln!(
            "SKIP m5_chromium_subset_gate: CXG_M5_CHROMIUM_PATH={} does not exist.",
            chromium_path.display()
        );
        return;
    }

    let cc_override = std::env::var("CXG_M5_CHROMIUM_CC").ok().map(PathBuf::from);

    let stage_dir = tempfile::tempdir().expect("tempdir");
    let sink = Arc::new(MockSink::default());

    let opts = RunOptions {
        input_path: chromium_path.clone(),
        compile_commands: cc_override,
        repo_name: "chromium_subset".to_owned(),
        stage_dir: Some(stage_dir.path().to_path_buf()),
        skip_phase2: true,
        skip_system_headers: true,
        skip_cache: false,
        skip_repo_node: true,
    };

    // AC-M5-10: pipeline must complete without panic/segfault.
    let stats = run(Arc::clone(&sink) as Arc<dyn GraphSink>, opts)
        .await
        .expect("AC-M5-10: pipeline must succeed on Chromium base/+net/ fixture");

    assert!(
        stats.tu_count > 0,
        "at least one TU must be processed from Chromium base/+net/"
    );

    let calls = sink.calls();
    let (macro_nodes, expands_to_edges) = extract_macro_counts(&calls);

    eprintln!(
        "m5_chromium_subset_gate: TUs={} macro_nodes={} expands_to_edges={}",
        stats.tu_count, macro_nodes, expands_to_edges
    );

    // AC-M5-11: ≥1 MACRO node.
    assert!(
        macro_nodes >= 1,
        "AC-M5-11: expected ≥1 MACRO node from Chromium base/+net/, got {macro_nodes}"
    );

    // AC-M5-11: ≥1 EXPANDS_TO edge.
    assert!(
        expands_to_edges >= 1,
        "AC-M5-11: expected ≥1 EXPANDS_TO edge from Chromium base/+net/, got {expands_to_edges}"
    );
}
