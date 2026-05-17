//! Boundary test: AC-M5-4 — EXPANDS_TO edge count is bounded.
//!
//! AC covered: AC-M5-4
//! Scenario: "EXPANDS_TO edge count is bounded to at most 10× source line
//!            count on LLVM .def fixtures"
//!
//! # What this tests
//!
//! The `xmacro_def` fixture contains a 20-entry X-macro `.def` file included
//! twice (building an enum and a name table) from a 38-line `main.cpp`.  The
//! visitor's `is_top_level_expansion` filter must suppress nested expansions
//! so that the EXPANDS_TO edge count stays ≤ 10 × L (where L = source lines
//! of the TU).
//!
//! Specifically:
//!   - L = 38  (wc -l main.cpp)
//!   - bound  = 380
//!   - expected actual count ≤ 40  (≤ 20 top-level expansions per include pass)
//!
//! If the `is_top_level_expansion` filter is accidentally removed or broken,
//! each nested HANDLE_EVENT call generates hundreds of edges and the assertion
//! fails, catching the regression.
//!
//! # Running
//!
//! ```sh
//! LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib \
//! DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib \
//!   cargo nextest run --test macro_expands_to_bound --features test-mock
//! ```

use std::path::PathBuf;
use std::sync::Arc;

use cpp_indexer::pipeline::{run, RunOptions};
use cpp_indexer::schema::EdgeKind;
use cpp_indexer::sink::mock::{MockCall, MockSink};
use cpp_indexer::sink::GraphSink;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

fn fixture_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/xmacro_def")
}

/// Count EXPANDS_TO edges across all MockSink WriteEdges calls.
fn count_expands_to(calls: &[MockCall]) -> usize {
    calls
        .iter()
        .filter_map(|c| {
            if let MockCall::WriteEdges(edges) = c {
                Some(
                    edges
                        .iter()
                        .filter(|e| e.kind == EdgeKind::ExpandsTo)
                        .count(),
                )
            } else {
                None
            }
        })
        .sum()
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

/// AC-M5-4: EXPANDS_TO edge count must not exceed 10 × source lines of the TU.
///
/// The xmacro_def fixture has L=38 source lines in main.cpp (the compiled TU).
/// The AC bound is 10 × 38 = 380.  Because the visitor's `is_top_level_expansion`
/// filter suppresses nested X-macro expansions, the actual count must be well
/// below the bound (≤ 40 top-level invocations across both include passes).
///
/// This test catches regressions where the nesting filter is removed or
/// incorrectly conditioned — a class of bug the existing ≥1-edge assertions
/// in m5_exit_gate cannot detect.
#[tokio::test]
async fn expands_to_count_bounded_by_10x_source_lines() {
    let fixture = fixture_dir();
    let stage_dir = tempfile::tempdir().expect("tempdir");
    let sink = Arc::new(MockSink::default());

    let opts = RunOptions {
        input_path: fixture.clone(),
        compile_commands: Some(fixture.join("compile_commands.json")),
        repo_name: "xmacro_def_bound_test".to_owned(),
        stage_dir: Some(stage_dir.path().to_path_buf()),
        skip_phase2: true,
        skip_system_headers: true,
        skip_cache: false,
        skip_repo_node: true,
    };

    let stats = run(Arc::clone(&sink) as Arc<dyn GraphSink>, opts)
        .await
        .expect("AC-M5-4: pipeline must complete on xmacro_def fixture");

    assert!(
        stats.tu_count > 0,
        "at least one TU must be processed; fixture contains main.cpp"
    );

    let calls = sink.calls();
    let expands_to_count = count_expands_to(&calls);

    // Source lines of the compiled TU (main.cpp), measured at fixture creation.
    // Update this constant if the fixture file changes.
    const TU_SOURCE_LINES: usize = 38;
    const BOUND: usize = 10 * TU_SOURCE_LINES; // AC-M5-4 formula

    eprintln!(
        "AC-M5-4 boundary: L={TU_SOURCE_LINES} bound={BOUND} actual_expands_to={expands_to_count}"
    );

    assert!(
        expands_to_count <= BOUND,
        "AC-M5-4: EXPANDS_TO edge count {expands_to_count} exceeds 10×L bound ({BOUND}). \
         Check that is_top_level_expansion filter is active in visit/macros.rs."
    );
}

/// AC-M5-4 negative companion: the pipeline must still emit ≥1 EXPANDS_TO
/// edge (guards against the degenerate case where the filter is too aggressive
/// and suppresses all edges).
#[tokio::test]
async fn expands_to_count_is_nonzero_for_xmacro_fixture() {
    let fixture = fixture_dir();
    let stage_dir = tempfile::tempdir().expect("tempdir");
    let sink = Arc::new(MockSink::default());

    let opts = RunOptions {
        input_path: fixture.clone(),
        compile_commands: Some(fixture.join("compile_commands.json")),
        repo_name: "xmacro_def_nonzero_test".to_owned(),
        stage_dir: Some(stage_dir.path().to_path_buf()),
        skip_phase2: true,
        skip_system_headers: true,
        skip_cache: false,
        skip_repo_node: true,
    };

    let stats = run(Arc::clone(&sink) as Arc<dyn GraphSink>, opts)
        .await
        .expect("pipeline must complete");

    assert!(stats.tu_count > 0);

    let calls = sink.calls();
    let expands_to_count = count_expands_to(&calls);

    assert!(
        expands_to_count >= 1,
        "AC-M5-4 companion: expected ≥1 EXPANDS_TO edge from X-macro fixture; \
         got 0 — is_top_level_expansion filter may be too restrictive."
    );
}
