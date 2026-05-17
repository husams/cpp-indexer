//! Prometheus metrics exposed by `cxg-index` and `cxg-daemon`.
//!
//! Counters are created lazily the first time they are referenced.  They are
//! **not** registered into a global registry here; scrape-endpoint wiring is
//! deferred to the REST control-plane story (S20+, `cxg-daemon`).  The
//! counters can still be incremented and read in unit tests without a running
//! HTTP server.

use prometheus::{Gauge, IntCounter, Opts};
use std::sync::OnceLock;

// ---------------------------------------------------------------------------
// Counters
// ---------------------------------------------------------------------------

/// Total number of TUs that triggered a libclang hard failure or panic during
/// Phase 1 parallel ingestion (AC-M3-2, ADR-7).
///
/// Incremented once per TU on panic or `Err` return from the per-TU parse
/// closure.  Does not include TUs with soft diagnostic errors (`partial = true`).
pub fn cxg_libclang_errors_total() -> &'static IntCounter {
    static COUNTER: OnceLock<IntCounter> = OnceLock::new();
    COUNTER.get_or_init(|| {
        IntCounter::with_opts(Opts::new(
            "cxg_libclang_errors_total",
            "Total TUs that failed or panicked in Phase 1 libclang parse",
        ))
        .expect("cxg_libclang_errors_total: invalid metric opts")
    })
}

/// Total graph nodes written by completed pipeline runs.
pub fn cxg_nodes_total() -> &'static IntCounter {
    static COUNTER: OnceLock<IntCounter> = OnceLock::new();
    COUNTER.get_or_init(|| {
        IntCounter::with_opts(Opts::new(
            "cxg_nodes_total",
            "Total graph nodes written by completed pipeline runs",
        ))
        .expect("cxg_nodes_total: invalid metric opts")
    })
}

/// Total graph edges written by completed pipeline runs.
pub fn cxg_edges_total() -> &'static IntCounter {
    static COUNTER: OnceLock<IntCounter> = OnceLock::new();
    COUNTER.get_or_init(|| {
        IntCounter::with_opts(Opts::new(
            "cxg_edges_total",
            "Total graph edges written by completed pipeline runs",
        ))
        .expect("cxg_edges_total: invalid metric opts")
    })
}

/// Nodes per second for the most recent completed pipeline run.
pub fn cxg_nodes_per_second() -> &'static Gauge {
    static GAUGE: OnceLock<Gauge> = OnceLock::new();
    GAUGE.get_or_init(|| {
        Gauge::with_opts(Opts::new(
            "cxg_nodes_per_second",
            "Nodes written per second by the most recent completed pipeline run",
        ))
        .expect("cxg_nodes_per_second: invalid metric opts")
    })
}

/// Edges per second for the most recent completed pipeline run.
pub fn cxg_edges_per_second() -> &'static Gauge {
    static GAUGE: OnceLock<Gauge> = OnceLock::new();
    GAUGE.get_or_init(|| {
        Gauge::with_opts(Opts::new(
            "cxg_edges_per_second",
            "Edges written per second by the most recent completed pipeline run",
        ))
        .expect("cxg_edges_per_second: invalid metric opts")
    })
}

/// Cache-hit ratio for the most recent completed pipeline run.
pub fn cxg_cache_hit_ratio() -> &'static Gauge {
    static GAUGE: OnceLock<Gauge> = OnceLock::new();
    GAUGE.get_or_init(|| {
        Gauge::with_opts(Opts::new(
            "cxg_cache_hit_ratio",
            "Translation-unit cache hit ratio for the most recent completed pipeline run",
        ))
        .expect("cxg_cache_hit_ratio: invalid metric opts")
    })
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn counter_starts_at_zero_and_increments() {
        // Each test binary gets its own process, so the OnceLock is fresh.
        let c = cxg_libclang_errors_total();
        let before = c.get();
        c.inc();
        assert_eq!(c.get(), before + 1);
    }
}
