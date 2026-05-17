//! Axum router for `cxg-daemon`.
//!
//! S35 adds the `/metrics` route (unauthenticated, AC-M7-18).
//! S33 will extend this router with `/v1/ingest`, `/v1/jobs`, `/v1/status`,
//! `/v1/repos` and the bearer-auth middleware layer.
//! S34 will extend it with `/v1/reset`.
//!
//! **Merge note**: S33 owns the final `Router` composition and auth layers.
//! This file exposes `metrics_router()` so S33 can merge with `.merge()`.

use axum::{routing::get, Router};

use crate::api::metrics::handler as metrics_handler;

/// Returns a `Router` containing only the `/metrics` scrape endpoint.
///
/// This route requires no authentication (ADR-5, AC-M7-18).
pub fn metrics_router() -> Router {
    Router::new().route("/metrics", get(metrics_handler))
}
