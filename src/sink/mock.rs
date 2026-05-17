//! In-memory `MockSink` for unit tests.
//!
//! Gated on `#[cfg(any(test, feature = "test-mock"))]` so it compiles in
//! `cargo test` and in dependent crates that enable the `test-mock` feature,
//! but is excluded from production binaries.
//!
//! ## Usage
//!
//! ```rust,ignore
//! let sink = Arc::new(MockSink::default());
//! sink.write_nodes(&nodes).await.unwrap();
//! let calls = sink.calls();
//! assert_eq!(calls[0], MockCall::WriteNodes(nodes));
//! ```

use std::sync::{Arc, Mutex};
use std::time::Duration;

use async_trait::async_trait;

use crate::error::Result;
use crate::schema::{EdgeRecord, NodeRecord};
use crate::sink::lock::Phase5LockGuard;
use crate::sink::{GraphSink, HealthInfo, ResetTarget, WriteStats};

// ── Call record ────────────────────────────────────────────────────────────────

/// A record of a single call made to [`MockSink`], in order.
#[derive(Debug, Clone, PartialEq)]
pub enum MockCall {
    Preflight,
    EnsureIndexes,
    WriteNodes(Vec<NodeRecord>),
    WriteEdges(Vec<EdgeRecord>),
    Reset(ResetTarget),
    AcquirePhase5Lock { ttl: Duration },
    ReadSchemaVersion,
    Health,
}

// ── MockPhase5LockGuard ────────────────────────────────────────────────────────

/// A no-op lock guard returned by [`MockSink::acquire_phase5_lock`].
#[derive(Debug)]
pub struct MockPhase5LockGuard {
    released: bool,
}

impl MockPhase5LockGuard {
    fn new() -> Self {
        Self { released: false }
    }
}

impl Phase5LockGuard for MockPhase5LockGuard {
    fn release(
        &mut self,
    ) -> std::pin::Pin<Box<dyn std::future::Future<Output = Result<()>> + Send + '_>> {
        self.released = true;
        Box::pin(std::future::ready(Ok(())))
    }
}

impl Drop for MockPhase5LockGuard {
    fn drop(&mut self) {
        // Best-effort: already released via explicit `release()` or the guard
        // is dropped without calling it (e.g. panic path).  Nothing to do for
        // the in-memory mock.
    }
}

// ── MockSink ──────────────────────────────────────────────────────────────────

/// An in-memory sink that records every call for assertion in unit tests.
///
/// Thread-safe via an internal `Mutex<Vec<MockCall>>`.
#[derive(Debug, Default)]
pub struct MockSink {
    log: Arc<Mutex<Vec<MockCall>>>,
}

impl MockSink {
    /// Return a snapshot of the call log in call order.
    pub fn calls(&self) -> Vec<MockCall> {
        self.log.lock().expect("mock lock poisoned").clone()
    }

    /// Return the number of recorded calls.
    pub fn call_count(&self) -> usize {
        self.log.lock().expect("mock lock poisoned").len()
    }

    /// Clear the call log (useful for multi-phase test assertions).
    pub fn reset_log(&self) {
        self.log.lock().expect("mock lock poisoned").clear();
    }

    fn push(&self, call: MockCall) {
        self.log.lock().expect("mock lock poisoned").push(call);
    }
}

#[async_trait]
impl GraphSink for MockSink {
    fn backend_name(&self) -> &'static str {
        "mock"
    }

    async fn preflight(&self) -> Result<()> {
        self.push(MockCall::Preflight);
        Ok(())
    }

    async fn ensure_indexes(&self) -> Result<()> {
        self.push(MockCall::EnsureIndexes);
        Ok(())
    }

    async fn write_nodes(&self, batch: &[NodeRecord]) -> Result<WriteStats> {
        self.push(MockCall::WriteNodes(batch.to_vec()));
        Ok(WriteStats {
            nodes_written: batch.len() as u64,
            retries: 0,
            elapsed: Duration::ZERO,
        })
    }

    async fn write_edges(&self, batch: &[EdgeRecord]) -> Result<WriteStats> {
        self.push(MockCall::WriteEdges(batch.to_vec()));
        Ok(WriteStats {
            nodes_written: batch.len() as u64,
            retries: 0,
            elapsed: Duration::ZERO,
        })
    }

    async fn reset(&self, target: ResetTarget) -> Result<()> {
        self.push(MockCall::Reset(target));
        Ok(())
    }

    async fn acquire_phase5_lock(&self, ttl: Duration) -> Result<Box<dyn Phase5LockGuard>> {
        self.push(MockCall::AcquirePhase5Lock { ttl });
        Ok(Box::new(MockPhase5LockGuard::new()))
    }

    async fn read_schema_version(&self) -> Result<Option<String>> {
        self.push(MockCall::ReadSchemaVersion);
        Ok(None)
    }

    async fn health(&self) -> Result<HealthInfo> {
        self.push(MockCall::Health);
        Ok(HealthInfo {
            status: "ok".to_owned(),
            latency: Duration::ZERO,
        })
    }
}

// ── Tests ──────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use super::*;
    use crate::schema::edges::{EdgeKind, EdgeRecord};
    use crate::schema::nodes::{NodeKind, NodeRecord};
    use crate::sink::GraphSink;

    fn make_node(usr: &str) -> NodeRecord {
        NodeRecord {
            usr: usr.to_owned(),
            kind: NodeKind::Function,
            name: "foo".to_owned(),
            qualified_name: "ns::foo".to_owned(),
            mangled_name: None,
            file_path: "/src/foo.cpp".to_owned(),
            line: Some(10),
            col: Some(1),
            repo_name: "my_repo".to_owned(),
            attrs_json: "{}".to_owned(),
            partial: false,
            phase: 1,
            tu_hash: [0u8; 32],
        }
    }

    fn make_edge(src: &str, dst: &str) -> EdgeRecord {
        EdgeRecord {
            src_usr: src.to_owned(),
            dst_usr: Some(dst.to_owned()),
            dst_placeholder: None,
            kind: EdgeKind::Calls,
            resolved: true,
            cross_repo_candidate: false,
            repo_name: "my_repo".to_owned(),
            attrs_json: "{}".to_owned(),
            tu_hash: [0u8; 32],
        }
    }

    #[tokio::test]
    async fn mock_records_call_sequence() {
        let sink = MockSink::default();

        sink.preflight().await.unwrap();
        sink.ensure_indexes().await.unwrap();
        let nodes = vec![make_node("usr::foo"), make_node("usr::bar")];
        sink.write_nodes(&nodes).await.unwrap();
        let edges = vec![make_edge("usr::foo", "usr::bar")];
        sink.write_edges(&edges).await.unwrap();
        sink.reset(ResetTarget::All).await.unwrap();

        let calls = sink.calls();
        assert_eq!(calls.len(), 5);
        assert_eq!(calls[0], MockCall::Preflight);
        assert_eq!(calls[1], MockCall::EnsureIndexes);
        assert_eq!(calls[2], MockCall::WriteNodes(nodes));
        assert_eq!(calls[3], MockCall::WriteEdges(edges));
        assert_eq!(calls[4], MockCall::Reset(ResetTarget::All));
    }

    #[tokio::test]
    async fn write_nodes_returns_correct_count() {
        let sink = MockSink::default();
        let nodes = vec![make_node("a"), make_node("b"), make_node("c")];
        let stats = sink.write_nodes(&nodes).await.unwrap();
        assert_eq!(stats.nodes_written, 3);
        assert_eq!(stats.retries, 0);
    }

    #[tokio::test]
    async fn write_edges_returns_correct_count() {
        let sink = MockSink::default();
        let edges = vec![make_edge("a", "b"), make_edge("b", "c")];
        let stats = sink.write_edges(&edges).await.unwrap();
        assert_eq!(stats.nodes_written, 2);
        assert_eq!(stats.retries, 0);
    }

    #[tokio::test]
    async fn phase5_lock_guard_is_releasable() {
        let sink = MockSink::default();
        let mut guard = sink
            .acquire_phase5_lock(Duration::from_secs(30))
            .await
            .unwrap();
        guard.release().await.unwrap();

        let calls = sink.calls();
        assert!(calls.contains(&MockCall::AcquirePhase5Lock {
            ttl: Duration::from_secs(30)
        }));
    }

    #[tokio::test]
    async fn trait_object_compiles_behind_arc() {
        // Verify the trait-object requirement: Arc<dyn GraphSink> must compile.
        let sink: Arc<dyn GraphSink> = Arc::new(MockSink::default());
        let info = sink.health().await.unwrap();
        assert_eq!(info.status, "ok");
        assert_eq!(sink.backend_name(), "mock");
    }

    #[tokio::test]
    async fn reset_log_clears_call_sequence() {
        let sink = MockSink::default();
        sink.preflight().await.unwrap();
        assert_eq!(sink.call_count(), 1);
        sink.reset_log();
        assert_eq!(sink.call_count(), 0);
    }

    #[tokio::test]
    async fn read_schema_version_returns_none() {
        let sink = MockSink::default();
        let v = sink.read_schema_version().await.unwrap();
        assert!(v.is_none());
    }

    #[tokio::test]
    async fn health_returns_ok() {
        let sink = MockSink::default();
        let info = sink.health().await.unwrap();
        assert_eq!(info.status, "ok");
    }
}
