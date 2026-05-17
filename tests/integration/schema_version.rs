//! Integration tests for the SchemaVersion handshake (S31, ADR-9).
//!
//! AC covered: AC-M6-6 (SchemaVersion node written by Phase 4),
//!             AC-M6-7 (Phase 5 refuses on version mismatch).
//!
//! These tests use `MockSink` to avoid a live DB dependency.

use std::sync::Arc;

use cpp_indexer::resolve::cross_repo::check_schema_version;
use cpp_indexer::schema::version::{schema_version_attrs, SCHEMA_VERSION, SCHEMA_VERSION_TAG};
use cpp_indexer::sink::mock::{MockCall, MockSink};
use cpp_indexer::sink::GraphSink;

// ── AC-M6-6: schema_version_attrs helper ─────────────────────────────────────

/// `schema_version_attrs` produces a JSON object containing the current version
/// constant and tag string.
#[test]
fn schema_version_attrs_contains_current_version() {
    let attrs = schema_version_attrs("cafebabe", "libclang 18.0.0");
    let v: serde_json::Value =
        serde_json::from_str(&attrs).expect("schema_version_attrs must be valid JSON");
    assert_eq!(
        v["version"].as_u64().unwrap() as u32,
        SCHEMA_VERSION,
        "attrs.version must match SCHEMA_VERSION constant"
    );
    assert_eq!(
        v["tag"].as_str().unwrap(),
        SCHEMA_VERSION_TAG,
        "attrs.tag must match SCHEMA_VERSION_TAG constant"
    );
    assert_eq!(v["indexer_commit"].as_str().unwrap(), "cafebabe");
}

// ── AC-M6-6: MockSink write + read round-trip ─────────────────────────────────

/// After `write_schema_version`, `read_schema_version` returns the written tag.
#[tokio::test]
async fn schema_version_write_then_read_roundtrip() {
    let sink = MockSink::default();
    let attrs = schema_version_attrs("abc123", "libclang 18");

    sink.write_schema_version(SCHEMA_VERSION_TAG, &attrs)
        .await
        .expect("write_schema_version must succeed");

    let read_back = sink
        .read_schema_version()
        .await
        .expect("read_schema_version must succeed");

    assert_eq!(
        read_back,
        Some(SCHEMA_VERSION_TAG.to_owned()),
        "read_schema_version must return the tag that was written"
    );
}

/// `write_schema_version` is idempotent: a second write overwrites the first.
#[tokio::test]
async fn schema_version_write_is_idempotent() {
    let sink = MockSink::default();
    let attrs = schema_version_attrs("v1-commit", "libclang 18");

    sink.write_schema_version("cxg-schema-v3", &attrs)
        .await
        .unwrap();
    sink.write_schema_version(SCHEMA_VERSION_TAG, &attrs)
        .await
        .unwrap();

    let v = sink.read_schema_version().await.unwrap();
    assert_eq!(
        v,
        Some(SCHEMA_VERSION_TAG.to_owned()),
        "second write must overwrite first"
    );
}

/// `MockSink` records `WriteSchemaVersion` call in the call log.
#[tokio::test]
async fn schema_version_write_is_recorded_in_mock_call_log() {
    let sink = MockSink::default();
    let attrs = schema_version_attrs("deadbeef", "libclang 18");

    sink.write_schema_version(SCHEMA_VERSION_TAG, &attrs)
        .await
        .unwrap();

    let calls = sink.calls();
    assert!(
        calls.contains(&MockCall::WriteSchemaVersion {
            tag: SCHEMA_VERSION_TAG.to_owned()
        }),
        "call log must contain WriteSchemaVersion; got: {calls:?}"
    );
}

// ── AC-M6-7: Phase 5 refuses on version mismatch ─────────────────────────────

/// Phase 5 `check_schema_version` returns `Ok` when no SchemaVersion node is
/// present (fresh DB).
#[tokio::test]
async fn schema_version_phase5_ok_when_no_node() {
    // MockSink starts with schema_version = None.
    let sink: Arc<dyn cpp_indexer::sink::GraphSink> = Arc::new(MockSink::default());
    let result = check_schema_version(&sink).await;
    assert!(
        result.is_ok(),
        "Phase 5 must allow a fresh DB with no SchemaVersion node"
    );
}

/// Phase 5 `check_schema_version` returns `Ok` when the DB version matches.
#[tokio::test]
async fn schema_version_phase5_ok_on_match() {
    let sink = MockSink::default();
    let attrs = schema_version_attrs("abc", "libclang 18");
    sink.write_schema_version(SCHEMA_VERSION_TAG, &attrs)
        .await
        .unwrap();

    let sink: Arc<dyn cpp_indexer::sink::GraphSink> = Arc::new(sink);
    let result = check_schema_version(&sink).await;
    assert!(
        result.is_ok(),
        "Phase 5 must accept a DB whose version matches the binary"
    );
}

/// Phase 5 `check_schema_version` returns `Err` when the DB version is older.
///
/// This is the canonical AC-M6-7 test: fixture repo indexed with schema v3
/// while the binary expects v4.
#[tokio::test]
async fn schema_version_phase5_refuses_on_mismatch() {
    let sink = MockSink::default();
    // Simulate a DB that was indexed with an older binary (schema v3).
    let stale_attrs = schema_version_attrs("old-commit", "libclang 17");
    sink.write_schema_version("cxg-schema-v3", &stale_attrs)
        .await
        .unwrap();

    let sink: Arc<dyn cpp_indexer::sink::GraphSink> = Arc::new(sink);
    let result = check_schema_version(&sink).await;

    assert!(
        result.is_err(),
        "Phase 5 must refuse when DB schema version != binary schema version"
    );
    let err_msg = result.unwrap_err().to_string();
    assert!(
        err_msg.contains("cxg-schema-v3"),
        "error must name the actual DB version; got: {err_msg}"
    );
    assert!(
        err_msg.contains(SCHEMA_VERSION_TAG),
        "error must name the expected binary version; got: {err_msg}"
    );
}

/// Error message from a mismatch includes both expected and actual tags so
/// operators can diagnose the discrepancy without inspecting code.
#[tokio::test]
async fn schema_version_mismatch_error_is_diagnostic() {
    let sink = MockSink::default();
    let stale_attrs = schema_version_attrs("commit-old", "libclang 17");
    sink.write_schema_version("cxg-schema-v1", &stale_attrs)
        .await
        .unwrap();

    let sink: Arc<dyn cpp_indexer::sink::GraphSink> = Arc::new(sink);
    let err = check_schema_version(&sink).await.unwrap_err().to_string();

    // Both the binary expectation and the DB actuality must appear.
    assert!(
        err.contains(SCHEMA_VERSION_TAG),
        "error must contain binary expected tag; got: {err}"
    );
    assert!(
        err.contains("cxg-schema-v1"),
        "error must contain actual DB tag; got: {err}"
    );
}
