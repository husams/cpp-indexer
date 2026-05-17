//! Phase 5 advisory lock types.
//!
//! ## Drop semantics
//!
//! Implementations MUST attempt a best-effort lock release in [`Drop`].  The
//! [`Phase5LockGuard::release`] method is the preferred path because it is
//! `async` and can propagate errors; `Drop` is a synchronous fallback that
//! MUST NOT block the executor.
//!
//! Recommended pattern for `Drop` in concrete impls:
//!
//! ```rust,ignore
//! impl Drop for MyLockGuard {
//!     fn drop(&mut self) {
//!         // Best-effort: fire-and-forget via tokio::spawn or a channel signal.
//!         // Log errors; do NOT panic.
//!         if let Some(rt) = tokio::runtime::Handle::try_current().ok() {
//!             let fut = self.do_release();
//!             rt.spawn(fut);
//!         }
//!     }
//! }
//! ```
//!
//! ## Backend notes
//! - **Neo4j**: use APOC `apoc.lock.nodes` on a sentinel node, or a
//!   leader-election node with a timestamped TTL property.
//! - **IndraDB**: a vertex with property `cxg_phase5_lock=<host>:<pid>:<ts>`
//!   plus an optimistic check on acquire.

use crate::error::Result;

/// A held Phase 5 advisory lock.
///
/// Callers `await` the drop via [`release`][Phase5LockGuard::release]; the
/// lock is also released on `Drop` as a safety net (see module doc).
pub trait Phase5LockGuard: Send + Sync {
    /// Explicitly release the lock, propagating any backend error.
    ///
    /// Idempotent: calling this after the guard is already released is a no-op
    /// that returns `Ok(())`.
    fn release(
        &mut self,
    ) -> std::pin::Pin<Box<dyn std::future::Future<Output = Result<()>> + Send + '_>>;
}
