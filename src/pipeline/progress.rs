//! Progress reporter stub — emits periodic stderr progress (AC-M3-13).
//!
//! This module is a no-op stub for M1. The full throttled reporter (≥ once/5 s)
//! is implemented in a later story (S??-progress-reporter).

/// A no-op progress reporter.
///
/// Replace the body of [`Reporter::tick`] with rate-limited stderr output in
/// the story that implements AC-M3-13.
#[derive(Debug, Default)]
pub struct Reporter;

impl Reporter {
    /// Create a new reporter.
    pub fn new() -> Self {
        Self
    }

    /// Record one unit of work. No-op in the M1 stub.
    pub fn tick(&self) {}
}
