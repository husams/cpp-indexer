//! Gate test for SCHEMA_VERSION bumps (ADR-9, ADR-11, AC-S40-1, AC-S40-5).
//!
//! This test asserts that:
//! 1. `SCHEMA_VERSION` is the expected v5 constant (AC-S40-1).
//! 2. `SCHEMA_VERSION_TAG` is consistent with the constant.
//! 3. A schema-handshake mismatch returns a typed `Error::Schema` (not a panic or
//!    generic IO error) — AC-S40-5.
//! 4. The Blake3 hash of `src/schema/nodes.rs` and `src/schema/edges.rs` matches the
//!    recorded baseline in `tests/schema-baseline.txt`, ensuring that any future
//!    structural change to the schema files forces a deliberate version bump.
//!
//! The baseline hash is computed as Blake3( nodes_source_bytes || edges_source_bytes )
//! where `||` is concatenation. If this test fails it means either:
//! - The schema files changed without bumping `SCHEMA_VERSION` → update the baseline
//!   AND bump the constant in the same PR, or
//! - `SCHEMA_VERSION` was bumped without updating the baseline → update the baseline.

use cpp_indexer::schema::version::{SCHEMA_VERSION, SCHEMA_VERSION_TAG};

/// AC-S40-1: The binary embeds schema v5 after the M8 bump.
#[test]
fn schema_version_is_v5() {
    assert_eq!(
        SCHEMA_VERSION, 5,
        "SCHEMA_VERSION must be 5 after M8 S40 bump"
    );
}

/// AC-S40-5 / tag consistency: SCHEMA_VERSION_TAG must match the integer.
#[test]
fn schema_version_tag_matches_integer() {
    let expected = format!("cxg-schema-v{SCHEMA_VERSION}");
    assert_eq!(
        SCHEMA_VERSION_TAG, expected,
        "SCHEMA_VERSION_TAG must be 'cxg-schema-v{SCHEMA_VERSION}'"
    );
}

/// AC-S40-5: Schema mismatch returns a typed `SchemaVersionMismatch`-class error
/// (manifested as `Error::Schema`) rather than a panic or generic IO error.
///
/// This test exercises the `check_schema_version` path via MockSink with a stale tag.
#[tokio::test]
async fn schema_mismatch_returns_typed_error_not_panic() {
    use cpp_indexer::resolve::cross_repo::check_schema_version;
    use cpp_indexer::sink::mock::MockSink;
    use cpp_indexer::sink::GraphSink;
    use std::sync::Arc;

    let sink = MockSink::default();
    let stale_attrs =
        cpp_indexer::schema::version::schema_version_attrs("old-commit", "libclang 17");
    // Write a stale tag that does not match current SCHEMA_VERSION_TAG
    sink.write_schema_version("cxg-schema-v4", &stale_attrs)
        .await
        .unwrap();

    let sink: Arc<dyn GraphSink> = Arc::new(sink);
    let result = check_schema_version(&sink).await;

    // Must be an error — not Ok, not a panic.
    assert!(
        result.is_err(),
        "check_schema_version must return Err on stale schema tag"
    );

    let err_msg = result.unwrap_err().to_string();
    // Error message must mention both tags so operators can diagnose.
    assert!(
        err_msg.contains("cxg-schema-v4"),
        "error must mention actual DB tag; got: {err_msg}"
    );
    assert!(
        err_msg.contains(SCHEMA_VERSION_TAG),
        "error must mention expected binary tag; got: {err_msg}"
    );
}

/// Baseline hash gate (ADR-9 §CI gate).
///
/// Computes Blake3( nodes.rs || edges.rs ) and compares against the recorded
/// value in `tests/schema-baseline.txt`. A mismatch means the schema files
/// changed without updating the baseline (and likely without bumping
/// SCHEMA_VERSION). Update `tests/schema-baseline.txt` **and** bump
/// `SCHEMA_VERSION` in the same PR whenever the schema files change.
#[test]
fn schema_source_matches_baseline() {
    use std::path::Path;

    let manifest_dir = env!("CARGO_MANIFEST_DIR");
    let nodes_path = Path::new(manifest_dir).join("src/schema/nodes.rs");
    let edges_path = Path::new(manifest_dir).join("src/schema/edges.rs");
    let baseline_path = Path::new(manifest_dir).join("tests/schema-baseline.txt");

    let nodes_src = std::fs::read(&nodes_path)
        .unwrap_or_else(|e| panic!("cannot read {}: {e}", nodes_path.display()));
    let edges_src = std::fs::read(&edges_path)
        .unwrap_or_else(|e| panic!("cannot read {}: {e}", edges_path.display()));

    // Compute hash over the concatenation.
    let mut hasher = blake3::Hasher::new();
    hasher.update(&nodes_src);
    hasher.update(&edges_src);
    let actual_hash = hasher.finalize().to_hex().to_string();

    if !baseline_path.exists() {
        panic!(
            "tests/schema-baseline.txt does not exist.\n\
             Create it with the current hash by running:\n\
             echo '{actual_hash}' > {}\n\
             This file must be committed alongside SCHEMA_VERSION = {SCHEMA_VERSION}.",
            baseline_path.display()
        );
    }

    let expected_hash = std::fs::read_to_string(&baseline_path)
        .unwrap_or_else(|e| panic!("cannot read baseline: {e}"))
        .trim()
        .to_owned();

    assert_eq!(
        actual_hash, expected_hash,
        "Schema source hash mismatch!\n\
         The contents of src/schema/nodes.rs or src/schema/edges.rs changed.\n\
         If this is intentional:\n\
         1. Bump SCHEMA_VERSION (currently {SCHEMA_VERSION}) in src/schema/version.rs.\n\
         2. Update tests/schema-baseline.txt to: {actual_hash}\n\
         Both changes MUST land in the same PR per ADR-9."
    );
}
