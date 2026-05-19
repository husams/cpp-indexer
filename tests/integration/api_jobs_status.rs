//! Integration tests for `JobQueue` status / outcome fields (S4 — AC-6, AC-7).
//!
//! These tests call into `JobQueue` and `JobRecord` directly (no HTTP server
//! required) to assert the wire JSON shape and the three `status` transitions.
//! The daemon REST route (`GET /v1/jobs/{id}`) serialises `JobRecord` verbatim,
//! so verifying the struct → JSON shape here covers AC-6 end-to-end.

use std::path::PathBuf;

use cpp_indexer::api::jobs::{
    IngestOptions, IngestSource, JobOutcome, JobQueue, JobRecord, JobState,
};

fn path_source(p: &str) -> IngestSource {
    IngestSource::Path {
        path: PathBuf::from(p),
    }
}

// ── Three status transitions ───────────────────────────────────────────────────

/// AC-6 transition 1: zero failures → `completed`.
#[test]
fn status_completed_zero_failures() {
    let (queue, _rx) = JobQueue::new(64);
    let id = queue
        .enqueue(path_source("/repo"), IngestOptions::default())
        .expect("enqueue");
    queue.mark_running(&id);
    queue.mark_done_with_counts(&id, 7, 0, 100, 50);

    let rec = queue.get(&id).expect("job exists");
    assert_eq!(rec.state, JobState::Done);
    assert_eq!(rec.status, Some(JobOutcome::Completed));
    assert_eq!(rec.failed_tu_count, 0);

    // Wire JSON shape
    let json = serde_json::to_value(&rec).expect("serialise");
    assert_eq!(json["status"], "completed");
    assert_eq!(json["failed_tu_count"], 0);
    assert_eq!(json["state"], "done");
}

/// AC-6 transition 2: partial failures → `completed_with_errors`.
#[test]
fn status_completed_with_errors_partial_failures() {
    let (queue, _rx) = JobQueue::new(64);
    let id = queue
        .enqueue(path_source("/repo"), IngestOptions::default())
        .expect("enqueue");
    queue.mark_running(&id);
    queue.mark_done_with_counts(&id, 7, 3, 60, 30);

    let rec = queue.get(&id).expect("job exists");
    assert_eq!(rec.status, Some(JobOutcome::CompletedWithErrors));
    assert_eq!(rec.failed_tu_count, 3);

    let json = serde_json::to_value(&rec).expect("serialise");
    assert_eq!(json["status"], "completed_with_errors");
    assert_eq!(json["failed_tu_count"], 3);
}

/// AC-6 transition 3: all failures → `failed` (done-state with `status=failed`,
/// distinct from pipeline crash `state=failed`).
#[test]
fn status_failed_all_failures() {
    let (queue, _rx) = JobQueue::new(64);
    let id = queue
        .enqueue(path_source("/repo"), IngestOptions::default())
        .expect("enqueue");
    queue.mark_running(&id);
    queue.mark_done_with_counts(&id, 7, 7, 0, 0);

    let rec = queue.get(&id).expect("job exists");
    assert_eq!(rec.state, JobState::Done);
    assert_eq!(rec.status, Some(JobOutcome::Failed));
    assert_eq!(rec.failed_tu_count, 7);

    let json = serde_json::to_value(&rec).expect("serialise");
    assert_eq!(json["status"], "failed");
    // `state` must still be "done" — not "failed" — for this path.
    assert_eq!(json["state"], "done");
}

// ── AC-7 back-compat ──────────────────────────────────────────────────────────

/// AC-7: legacy JSON without `failed_tu_count` / `status` keys must
/// deserialise with defaults (0 / None).
#[test]
fn legacy_record_without_new_fields_round_trips() {
    let legacy_json = r#"{
        "job_id": "legacy-abc-123",
        "state": "done",
        "phase": "done",
        "progress": 1.0,
        "tus_done": 7,
        "tus_total": 7,
        "nodes": 200,
        "edges": 80,
        "source": {"path": "/legacy/repo"},
        "options": {},
        "enqueued_at": 1716000000
    }"#;

    let rec: JobRecord =
        serde_json::from_str(legacy_json).expect("legacy JSON must deserialise (AC-7)");

    assert_eq!(rec.job_id, "legacy-abc-123");
    assert_eq!(rec.failed_tu_count, 0, "must default to 0");
    assert!(rec.status.is_none(), "must default to None");
}

// ── In-flight job has no `status` key in wire JSON ────────────────────────────

/// Queued and running records must NOT emit the `status` key in JSON.
#[test]
fn in_flight_records_omit_status_key() {
    let (queue, _rx) = JobQueue::new(64);
    let id = queue
        .enqueue(path_source("/repo"), IngestOptions::default())
        .expect("enqueue");

    // Queued
    let queued_json = serde_json::to_value(queue.get(&id).expect("job exists")).expect("serialise");
    assert!(
        queued_json.get("status").is_none(),
        "queued record must not have `status` key"
    );

    // Running
    queue.mark_running(&id);
    let running_json =
        serde_json::to_value(queue.get(&id).expect("job exists")).expect("serialise");
    assert!(
        running_json.get("status").is_none(),
        "running record must not have `status` key"
    );
}
