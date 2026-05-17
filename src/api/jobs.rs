//! Bounded in-process job queue for `cxg-daemon`.
//!
//! `JobQueue` wraps a `tokio::sync::mpsc` channel bounded by `job_queue_max`
//! (from `[api]` config).  Attempting to enqueue when depth ≥ max returns
//! `QueueFull`, which the ingest handler maps to HTTP 429.
//!
//! `cxg_queue_depth` is updated on every enqueue / dequeue so that the
//! Prometheus `/metrics` scrape always shows the live value.
//!
//! AC: AC-M7-19.

use crate::api::metrics::CXG_QUEUE_DEPTH;
use tokio::sync::mpsc;

/// Opaque job payload.  Concrete fields are defined in S33 (ingest handler).
/// Kept as `serde_json::Value` here to avoid a circular dependency between
/// stories; S33 will replace this with a typed struct.
pub type JobPayload = serde_json::Value;

/// Errors that can arise from queue operations.
#[derive(Debug, thiserror::Error)]
pub enum QueueError {
    /// The queue has reached `job_queue_max`; return HTTP 429.
    #[error("job queue is full (capacity {capacity})")]
    QueueFull { capacity: usize },
}

/// Sending half of the job queue.  Clone this to share across handlers.
#[derive(Clone)]
pub struct JobQueue {
    tx: mpsc::Sender<JobPayload>,
    capacity: usize,
}

/// Receiving half of the job queue.  Owned by the worker task.
pub struct JobReceiver {
    pub rx: mpsc::Receiver<JobPayload>,
}

impl JobQueue {
    /// Create a new bounded queue with the given capacity.
    pub fn new(capacity: usize) -> (Self, JobReceiver) {
        let (tx, rx) = mpsc::channel(capacity);
        let queue = JobQueue { tx, capacity };
        let recv = JobReceiver { rx };
        (queue, recv)
    }

    /// Attempt a non-blocking enqueue.
    ///
    /// Returns `Err(QueueError::QueueFull)` when the channel has no free
    /// capacity, causing the ingest handler to return `429 Too Many Requests`.
    /// On success, increments `cxg_queue_depth`.
    pub fn try_enqueue(&self, job: JobPayload) -> Result<(), QueueError> {
        match self.tx.try_send(job) {
            Ok(()) => {
                CXG_QUEUE_DEPTH.inc();
                Ok(())
            }
            Err(mpsc::error::TrySendError::Full(_)) => Err(QueueError::QueueFull {
                capacity: self.capacity,
            }),
            Err(mpsc::error::TrySendError::Closed(_)) => {
                // Worker crashed — treat as full; daemon will surface this
                // via health / status endpoints (S33).
                Err(QueueError::QueueFull {
                    capacity: self.capacity,
                })
            }
        }
    }

    /// Current depth (number of items buffered in the channel).
    pub fn depth(&self) -> usize {
        self.capacity.saturating_sub(self.tx.capacity())
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn queue_full_returns_error_and_gauge_tracks_depth() {
        let capacity = 3_usize;
        let (queue, _recv) = JobQueue::new(capacity);

        // Read the gauge baseline before filling (other tests may have run).
        let baseline = CXG_QUEUE_DEPTH.get();

        // Fill the queue.
        for _ in 0..capacity {
            queue
                .try_enqueue(serde_json::json!({"job": "test"}))
                .unwrap();
        }
        assert_eq!(CXG_QUEUE_DEPTH.get(), baseline + capacity as i64);

        // Next enqueue must return QueueFull.
        let err = queue
            .try_enqueue(serde_json::json!({"job": "overflow"}))
            .unwrap_err();
        assert!(
            matches!(err, QueueError::QueueFull { .. }),
            "expected QueueFull, got {err}"
        );
        // Gauge must NOT have been incremented for the failed enqueue.
        assert_eq!(CXG_QUEUE_DEPTH.get(), baseline + capacity as i64);
    }

    #[tokio::test]
    async fn depth_reflects_channel_occupancy() {
        let (queue, _recv) = JobQueue::new(5);
        assert_eq!(queue.depth(), 0);
        queue.try_enqueue(serde_json::json!(1)).unwrap();
        assert_eq!(queue.depth(), 1);
    }
}
