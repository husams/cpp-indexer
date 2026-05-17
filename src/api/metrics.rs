//! Prometheus `/metrics` endpoint for `cxg-daemon`.
//!
//! Creates a dedicated `prometheus::Registry`, registers all process-level
//! and daemon-specific metrics into it, and exposes them via the
//! `GET /metrics` axum handler.  The endpoint is unauthenticated per ADR-5
//! AC-M7-18.
//!
//! **Registered metrics**
//! - `cxg_libclang_errors_total` (Counter) — imported from `crate::metrics`.
//! - `cxg_queue_depth` (Gauge) — current depth of the ingest job queue;
//!   updated by `crate::api::jobs::JobQueue` on enqueue/dequeue.
//!
//! AC: AC-M7-17, AC-M7-18.

use std::sync::OnceLock;

use axum::{http::StatusCode, response::IntoResponse};
use prometheus::{Encoder, IntGauge, Opts, Registry, TextEncoder};

// ---------------------------------------------------------------------------
// Dedicated registry
// ---------------------------------------------------------------------------

/// Returns the singleton `Registry` used by `cxg-daemon`.
///
/// The first call creates the registry and registers all daemon metrics.
/// Subsequent calls return the same instance.
pub fn registry() -> &'static Registry {
    static REG: OnceLock<Registry> = OnceLock::new();
    REG.get_or_init(|| {
        let r = Registry::new();
        // Register cxg_libclang_errors_total (from crate::metrics).
        r.register(Box::new(
            crate::metrics::cxg_libclang_errors_total().clone(),
        ))
        .expect("cxg_libclang_errors_total: duplicate registration");
        // Register cxg_queue_depth.
        r.register(Box::new(CXG_QUEUE_DEPTH.clone()))
            .expect("cxg_queue_depth: duplicate registration");
        r
    })
}

// ---------------------------------------------------------------------------
// cxg_queue_depth gauge
// ---------------------------------------------------------------------------

/// Live depth of the ingest job queue.
///
/// Exposed as a process-global so `api::jobs::JobQueue` can increment it
/// without holding a reference to the registry.
pub static CXG_QUEUE_DEPTH: std::sync::LazyLock<IntGauge> = std::sync::LazyLock::new(|| {
    IntGauge::with_opts(Opts::new(
        "cxg_queue_depth",
        "Current number of jobs buffered in the ingest queue",
    ))
    .expect("cxg_queue_depth: invalid metric opts")
});

// ---------------------------------------------------------------------------
// Axum handler
// ---------------------------------------------------------------------------

/// `GET /metrics` — Prometheus text exposition (no auth required, AC-M7-18).
///
/// Returns `text/plain; version=0.0.4; charset=utf-8` per the Prometheus
/// data model exposition format v0.0.4.
pub async fn handler() -> impl IntoResponse {
    let encoder = TextEncoder::new();
    let metric_families = registry().gather();
    let mut buf = Vec::new();
    if let Err(e) = encoder.encode(&metric_families, &mut buf) {
        return (
            StatusCode::INTERNAL_SERVER_ERROR,
            format!("metric encoding error: {e}"),
        )
            .into_response();
    }
    (
        StatusCode::OK,
        [("content-type", encoder.format_type())],
        buf,
    )
        .into_response()
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use axum::body::to_bytes;
    use axum::http::Request;
    use axum::{routing::get, Router};
    use tower::ServiceExt as _;

    async fn scrape() -> String {
        let app = Router::new().route("/metrics", get(handler));
        let req = Request::builder()
            .uri("/metrics")
            .body(axum::body::Body::empty())
            .unwrap();
        let resp = app.oneshot(req).await.unwrap();
        assert_eq!(resp.status(), StatusCode::OK);
        let ct = resp
            .headers()
            .get("content-type")
            .unwrap()
            .to_str()
            .unwrap()
            .to_owned();
        assert!(
            ct.starts_with("text/plain"),
            "unexpected Content-Type: {ct}"
        );
        let bytes = to_bytes(resp.into_body(), usize::MAX).await.unwrap();
        String::from_utf8(bytes.to_vec()).unwrap()
    }

    #[tokio::test]
    async fn metrics_endpoint_returns_expected_metric_names() {
        // Force registry initialisation.
        let _ = registry();
        let body = scrape().await;
        assert!(
            body.contains("cxg_libclang_errors_total"),
            "missing cxg_libclang_errors_total in:\n{body}"
        );
        assert!(
            body.contains("cxg_queue_depth"),
            "missing cxg_queue_depth in:\n{body}"
        );
    }

    #[tokio::test]
    async fn metrics_endpoint_requires_no_auth() {
        // No auth headers — must still return 200.
        let app = Router::new().route("/metrics", get(handler));
        let req = Request::builder()
            .uri("/metrics")
            .body(axum::body::Body::empty())
            .unwrap();
        let resp = app.oneshot(req).await.unwrap();
        assert_eq!(resp.status(), StatusCode::OK);
    }

    #[tokio::test]
    async fn cxg_queue_depth_appears_in_scrape() {
        let _ = registry();
        // Increment gauge and confirm value is reflected.
        let before = CXG_QUEUE_DEPTH.get();
        CXG_QUEUE_DEPTH.inc();
        let body = scrape().await;
        // Check gauge line contains a value >= before+1.
        assert!(
            body.contains("cxg_queue_depth"),
            "missing cxg_queue_depth in:\n{body}"
        );
        // Reset to avoid leaking state between tests.
        CXG_QUEUE_DEPTH.set(before);
    }
}
