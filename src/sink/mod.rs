//! `GraphSink` async trait + supporting types.
//!
//! Stories: S08-graphsink-trait-mock (AC-M1-22).
//! ADR: ADR-2.
//!
//! # Layout
//! - [`GraphSink`] — the shared async trait all sink backends implement.
//! - [`WriteStats`] — batch write statistics fed to Prometheus.
//! - [`ResetTarget`] — selector for `POST /v1/reset`.
//! - [`HealthInfo`] — connectivity probe result for `GET /v1/status`.
//! - [`Phase5LockGuard`] — cross-process advisory lock handle (see `lock.rs`).
//! - [`factory`] — runtime dispatch from `[sink].backend` to a concrete impl.
//! - [`mock`] — `MockSink` for unit tests (gated on `#[cfg(any(test, feature = "test-mock"))]`).

pub mod factory;
pub mod indradb;
pub mod lock;
#[cfg(any(test, feature = "test-mock"))]
pub mod mock;
pub mod neo4j;

use std::time::Duration;

use async_trait::async_trait;

use crate::error::Result;
use crate::schema::{EdgeRecord, NodeRecord};

// ── WriteStats ─────────────────────────────────────────────────────────────────

/// Statistics produced by a single batched sink write.
///
/// Fed to Prometheus counters/histograms; see `observability.rs`.
#[derive(Debug, Clone, PartialEq)]
pub struct WriteStats {
    /// Number of graph nodes or edges actually persisted.
    pub nodes_written: u64,
    /// Number of backend retries consumed during this call.
    pub retries: u32,
    /// Wall-clock time of the write operation.
    pub elapsed: Duration,
}

// ── ResetTarget ────────────────────────────────────────────────────────────────

/// Selector for [`GraphSink::reset`].
#[derive(Debug, Clone, PartialEq)]
pub enum ResetTarget {
    /// Reset data for a single named repository.
    Repo(String),
    /// Reset all repository data (full wipe).
    All,
}

// ── HealthInfo ─────────────────────────────────────────────────────────────────

/// Result of a connectivity probe; returned by [`GraphSink::health`].
#[derive(Debug, Clone, PartialEq)]
pub struct HealthInfo {
    /// Human-readable status string (e.g. `"ok"`, `"degraded"`).
    pub status: String,
    /// Round-trip latency to the database.
    pub latency: Duration,
}

// ── GraphSink trait ────────────────────────────────────────────────────────────

/// Shared async trait implemented by every graph-database sink.
///
/// Callers hold a `Arc<dyn GraphSink>` and are sink-agnostic.  The concrete
/// implementation is chosen at startup by [`factory::create`].
///
/// ## Idempotency contract
/// - [`write_nodes`][GraphSink::write_nodes]: idempotent on `(usr, repo_name)`.
/// - [`write_edges`][GraphSink::write_edges]: idempotent on `(src_usr, dst_usr, kind)`.
/// - [`ensure_indexes`][GraphSink::ensure_indexes]: always idempotent.
///
/// ## Concurrency
/// Multiple `write_nodes` / `write_edges` calls may be in flight concurrently up
/// to `[sink].sessions` (default 16).  Implementations use connection pools
/// internally.
#[async_trait]
pub trait GraphSink: Send + Sync {
    /// Stable name used in logs, metrics labels, and `REPO.sink` attributes.
    fn backend_name(&self) -> &'static str;

    /// Validate connectivity and credentials.  Called once at startup before
    /// any indexing begins.  MUST return `Err` if credentials are missing or
    /// the backend is unreachable.
    async fn preflight(&self) -> Result<()>;

    /// Create schema indexes / constraints.  Idempotent; called once before
    /// the first `write_nodes`.
    async fn ensure_indexes(&self) -> Result<()>;

    /// Write a batch of nodes.  MUST be idempotent on `(usr, repo_name)`.
    async fn write_nodes(&self, batch: &[NodeRecord]) -> Result<WriteStats>;

    /// Write a batch of edges.  MUST be idempotent on `(src_usr, dst_usr, kind)`.
    async fn write_edges(&self, batch: &[EdgeRecord]) -> Result<WriteStats>;

    /// Reset a single repository's data + cache, or all repositories.
    /// Used by `POST /v1/reset`.
    async fn reset(&self, target: ResetTarget) -> Result<()>;

    /// Acquire the Phase 5 cross-process advisory lock.
    ///
    /// The lock is held until the returned [`Phase5LockGuard`] is dropped.
    /// See `sink/lock.rs` for Drop semantics.
    async fn acquire_phase5_lock(&self, ttl: Duration) -> Result<Box<dyn lock::Phase5LockGuard>>;

    /// Read the `SchemaVersion` node value, if present.  Used by the Phase 5
    /// cross-repo compatibility check.
    async fn read_schema_version(&self) -> Result<Option<String>>;

    /// Lightweight connectivity probe for `GET /v1/status`.
    /// MUST NOT consume a write connection slot.
    async fn health(&self) -> Result<HealthInfo>;
}
