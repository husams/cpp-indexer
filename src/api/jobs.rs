//! In-process job queue and state machine for `cxg-daemon`.
//!
//! Job lifecycle: `Queued → Running → Done | Failed`.
//!
//! Jobs are stored in-memory only (v1; see ADR-5 §Persistence).  The channel
//! is bounded by `[api].job_queue_max` (default 64); posting when full returns
//! 429 (AC-M7-19).
//!
//! AC-M7-1, AC-M7-2, AC-M7-19.

use std::{
    cmp::Reverse,
    collections::HashMap,
    path::PathBuf,
    sync::{Arc, Mutex},
    time::SystemTime,
};

use serde::{Deserialize, Serialize};
use tokio::sync::mpsc;
use uuid::Uuid;

// ── IngestSource ───────────────────────────────────────────────────────────────

/// Source specification for an ingest job.
///
/// Wire format (ADR-5 §Ingest):
/// - local path: `{"path": "/abs/path"}`
/// - remote git: `{"git_url": "https://…", "ref": "main"}`
#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(untagged)]
pub enum IngestSource {
    /// Index a local path.
    Path { path: PathBuf },
    /// Clone + index a remote git repository.
    GitUrl {
        git_url: String,
        #[serde(rename = "ref", skip_serializing_if = "Option::is_none")]
        git_ref: Option<String>,
    },
}

// ── IngestOptions ──────────────────────────────────────────────────────────────

/// Per-job indexing options.
#[derive(Debug, Clone, Default, Deserialize, Serialize)]
pub struct IngestOptions {
    /// Override the configured sink backend (`"neo4j"` | `"indradb"`).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub sink: Option<String>,

    /// Skip Phase 2 decoration for this job.
    #[serde(default)]
    pub skip_phase2: bool,
}

// ── IngestRequest ──────────────────────────────────────────────────────────────

/// Body of `POST /v1/ingest`.
#[derive(Debug, Clone, Deserialize)]
pub struct IngestRequest {
    pub source: IngestSource,
    #[serde(default)]
    pub options: IngestOptions,
}

// ── JobPhase ───────────────────────────────────────────────────────────────────

/// Current pipeline phase for a running job.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum JobPhase {
    Bootstrap,
    Phase1,
    Phase2,
    Phase3,
    Phase4,
    Done,
}

// ── JobState ───────────────────────────────────────────────────────────────────

/// Top-level lifecycle state for a job.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum JobState {
    Queued,
    Running,
    Done,
    Failed,
}

// ── JobOutcome ─────────────────────────────────────────────────────────────────

/// Result classification of a completed job (set once at completion, absent
/// while queued or running).  Complements `JobState` which tracks lifecycle.
///
/// AC-6.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum JobOutcome {
    /// All TUs parsed successfully (`failed_tu_count == 0`).
    Completed,
    /// Some but not all TUs failed (`0 < failed_tu_count < tus_total`).
    CompletedWithErrors,
    /// All TUs failed, or only TUs were present and all failed
    /// (`failed_tu_count >= tus_total && tus_total > 0`).
    Failed,
}

// ── JobRecord ─────────────────────────────────────────────────────────────────

/// Complete record for a job, returned by `GET /v1/jobs/{id}`.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct JobRecord {
    pub job_id: String,
    pub state: JobState,
    pub phase: JobPhase,
    /// Progress fraction in `[0.0, 1.0]`.
    pub progress: f32,
    /// Translation units completed by the indexing pipeline.
    pub tus_done: u64,
    /// Total translation units selected for this job.
    pub tus_total: u64,
    /// Nodes written by this job.
    pub nodes: u64,
    /// Edges written by this job.
    pub edges: u64,
    pub source: IngestSource,
    pub options: IngestOptions,
    /// UNIX seconds at enqueue time.
    pub enqueued_at: u64,
    /// UNIX seconds when the job transitioned to `Running`, if applicable.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub started_at: Option<u64>,
    /// UNIX seconds when the job completed or failed, if applicable.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub finished_at: Option<u64>,
    /// Error message if `state == failed`.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<String>,
    /// Number of TUs that failed to parse.  Defaults to 0 for back-compat
    /// with legacy records that pre-date AC-6 (AC-7).
    #[serde(default)]
    pub failed_tu_count: u64,
    /// Result classification of a completed job.  Absent (`None`) while the
    /// job is queued or running; `skip_serializing_if` keeps the key out of
    /// in-flight responses.  AC-6.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub status: Option<JobOutcome>,
}

/// Concrete progress counters for a job.
#[derive(Debug, Clone, Copy, Default)]
pub struct JobProgressCounters {
    pub tus_done: u64,
    pub tus_total: u64,
    pub nodes: u64,
    pub edges: u64,
}

impl JobRecord {
    fn new(job_id: String, source: IngestSource, options: IngestOptions) -> Self {
        let now = SystemTime::now()
            .duration_since(SystemTime::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs();
        Self {
            job_id,
            state: JobState::Queued,
            phase: JobPhase::Bootstrap,
            progress: 0.0,
            tus_done: 0,
            tus_total: 0,
            nodes: 0,
            edges: 0,
            source,
            options,
            enqueued_at: now,
            started_at: None,
            finished_at: None,
            error: None,
            failed_tu_count: 0,
            status: None,
        }
    }
}

// ── JobMessage ─────────────────────────────────────────────────────────────────

/// Internal message sent over the bounded channel.
#[derive(Debug)]
pub struct JobMessage {
    pub job_id: String,
    pub source: IngestSource,
    pub options: IngestOptions,
}

/// Type alias for the receiver end of the job channel.
///
/// Tests hold onto this to prevent the channel from being closed prematurely.
pub type JobReceiver = mpsc::Receiver<JobMessage>;

// ── JobQueue ───────────────────────────────────────────────────────────────────

/// Maximum number of terminal (`Done` or `Failed`) job records retained in the
/// in-memory map.  When a job is marked terminal and this cap is exceeded, the
/// oldest terminal records (by `finished_at`, ascending) are evicted first.
/// Queued and running jobs are never evicted.
const MAX_TERMINAL_JOBS: usize = 256;

/// Shared job registry + bounded channel.
///
/// Clone-able; all clones share the same internal state.
#[derive(Clone, Debug)]
pub struct JobQueue {
    jobs: Arc<Mutex<HashMap<String, JobRecord>>>,
    sender: mpsc::Sender<JobMessage>,
    max_depth: usize,
}

impl JobQueue {
    /// Create a new queue with the given channel capacity.
    ///
    /// Returns the queue handle and the receiver end.  The caller is responsible
    /// for spawning a worker that drains the receiver.
    pub fn new(capacity: usize) -> (Self, mpsc::Receiver<JobMessage>) {
        let (sender, receiver) = mpsc::channel(capacity);
        let queue = Self {
            jobs: Arc::new(Mutex::new(HashMap::new())),
            sender,
            max_depth: capacity,
        };
        queue.update_queue_depth_metric();
        (queue, receiver)
    }

    /// Number of jobs currently in the channel (approximate).
    pub fn depth(&self) -> usize {
        self.max_depth - self.sender.capacity()
    }

    fn update_queue_depth_metric(&self) {
        crate::api::metrics::CXG_QUEUE_DEPTH.set(self.depth().try_into().unwrap_or(i64::MAX));
    }

    /// Enqueue a new ingest request.
    ///
    /// Returns the new `job_id` on success, or `None` if the queue is at
    /// capacity (caller should return 429).
    pub fn enqueue(&self, source: IngestSource, options: IngestOptions) -> Option<String> {
        // Check capacity without blocking.
        if self.sender.capacity() == 0 {
            return None;
        }

        let job_id = Uuid::now_v7().to_string();
        let record = JobRecord::new(job_id.clone(), source.clone(), options.clone());

        {
            let mut jobs = self.jobs.lock().expect("jobs lock poisoned");
            jobs.insert(job_id.clone(), record);
        }

        let msg = JobMessage {
            job_id: job_id.clone(),
            source,
            options,
        };

        // Non-blocking try_send since we already checked capacity above.
        match self.sender.try_send(msg) {
            Ok(()) => {
                self.update_queue_depth_metric();
                Some(job_id)
            }
            Err(_) => {
                // Race: another enqueue stole the slot.
                let mut jobs = self.jobs.lock().expect("jobs lock poisoned");
                jobs.remove(&job_id);
                drop(jobs);
                self.update_queue_depth_metric();
                None
            }
        }
    }

    /// Look up a job by ID.
    pub fn get(&self, job_id: &str) -> Option<JobRecord> {
        self.jobs
            .lock()
            .expect("jobs lock poisoned")
            .get(job_id)
            .cloned()
    }

    /// Iterate over all jobs, optionally filtered by state.
    pub fn list(&self, state_filter: Option<JobState>) -> Vec<JobRecord> {
        let jobs = self.jobs.lock().expect("jobs lock poisoned");
        let mut records: Vec<JobRecord> = match state_filter {
            Some(s) => jobs.values().filter(|r| r.state == s).cloned().collect(),
            None => jobs.values().cloned().collect(),
        };
        // Most recent first, matching the REST contract for `limit`.
        records.sort_by_key(|r| Reverse(r.enqueued_at));
        records
    }

    /// Mark a job as running.  No-op if the job is not found.
    pub fn mark_running(&self, job_id: &str) {
        let now = SystemTime::now()
            .duration_since(SystemTime::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs();
        let mut jobs = self.jobs.lock().expect("jobs lock poisoned");
        if let Some(rec) = jobs.get_mut(job_id) {
            rec.state = JobState::Running;
            rec.started_at = Some(now);
        }
        drop(jobs);
        self.update_queue_depth_metric();
    }

    /// Update the current phase and progress fraction.  No-op if not found.
    pub fn update_phase(&self, job_id: &str, phase: JobPhase, progress: f32) {
        let mut jobs = self.jobs.lock().expect("jobs lock poisoned");
        if let Some(rec) = jobs.get_mut(job_id) {
            rec.phase = phase;
            rec.progress = progress;
        }
    }

    /// Update phase, fraction, and concrete progress counters. No-op if not found.
    pub fn update_progress(
        &self,
        job_id: &str,
        phase: JobPhase,
        progress: f32,
        counters: JobProgressCounters,
    ) {
        let mut jobs = self.jobs.lock().expect("jobs lock poisoned");
        if let Some(rec) = jobs.get_mut(job_id) {
            rec.phase = phase;
            rec.progress = progress;
            rec.tus_done = counters.tus_done;
            rec.tus_total = counters.tus_total;
            rec.nodes = counters.nodes;
            rec.edges = counters.edges;
        }
    }

    /// Evict the oldest terminal records beyond `MAX_TERMINAL_JOBS`.
    ///
    /// Must be called **while holding `jobs`** (i.e. pass the locked guard).
    /// Eviction order: ascending `finished_at` (oldest first).  Jobs without
    /// `finished_at` sort before those with one (treated as `0`).
    /// Queued / Running jobs are never touched.
    fn evict_terminal_overflow(jobs: &mut HashMap<String, JobRecord>) {
        // Collect IDs of terminal records.
        let mut terminal: Vec<(u64, String)> = jobs
            .values()
            .filter(|r| matches!(r.state, JobState::Done | JobState::Failed))
            .map(|r| (r.finished_at.unwrap_or(0), r.job_id.clone()))
            .collect();

        let excess = terminal.len().saturating_sub(MAX_TERMINAL_JOBS);
        if excess == 0 {
            return;
        }

        // Sort ascending by finished_at so oldest entries come first.
        terminal.sort_unstable_by_key(|(ts, _)| *ts);
        for (_, id) in terminal.into_iter().take(excess) {
            jobs.remove(&id);
        }
    }

    /// Mark a job as done.  No-op if not found.
    pub fn mark_done(&self, job_id: &str) {
        let now = SystemTime::now()
            .duration_since(SystemTime::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs();
        let mut jobs = self.jobs.lock().expect("jobs lock poisoned");
        if let Some(rec) = jobs.get_mut(job_id) {
            rec.state = JobState::Done;
            rec.phase = JobPhase::Done;
            rec.progress = 1.0;
            rec.tus_done = rec.tus_total;
            rec.finished_at = Some(now);
        }
        Self::evict_terminal_overflow(&mut jobs);
    }

    /// Mark a job as done and set final progress counters.
    ///
    /// `failed_tu_count` is the number of TUs that failed to parse; it is used
    /// to derive `status` (AC-6).  `#[serde(default)]` on the field ensures
    /// legacy records without it round-trip correctly (AC-7).
    pub fn mark_done_with_counts(
        &self,
        job_id: &str,
        tus_total: u64,
        failed_tu_count: u64,
        nodes: u64,
        edges: u64,
    ) {
        let now = SystemTime::now()
            .duration_since(SystemTime::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs();
        let outcome = if failed_tu_count == 0 {
            JobOutcome::Completed
        } else if failed_tu_count >= tus_total {
            JobOutcome::Failed
        } else {
            JobOutcome::CompletedWithErrors
        };
        let mut jobs = self.jobs.lock().expect("jobs lock poisoned");
        if let Some(rec) = jobs.get_mut(job_id) {
            rec.state = JobState::Done;
            rec.phase = JobPhase::Done;
            rec.progress = 1.0;
            rec.tus_total = tus_total;
            rec.tus_done = tus_total;
            rec.failed_tu_count = failed_tu_count;
            rec.status = Some(outcome);
            rec.nodes = nodes;
            rec.edges = edges;
            rec.finished_at = Some(now);
        }
        Self::evict_terminal_overflow(&mut jobs);
    }

    /// Mark a job as failed with an error message.  No-op if not found.
    pub fn mark_failed(&self, job_id: &str, error: String) {
        let now = SystemTime::now()
            .duration_since(SystemTime::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs();
        let mut jobs = self.jobs.lock().expect("jobs lock poisoned");
        if let Some(rec) = jobs.get_mut(job_id) {
            rec.state = JobState::Failed;
            rec.finished_at = Some(now);
            rec.error = Some(error);
        }
        Self::evict_terminal_overflow(&mut jobs);
    }
}

// ── Repo registry ──────────────────────────────────────────────────────────────

/// A lightweight record of a repository that has been (or is being) indexed.
///
/// Populated from completed job metadata (no GraphSink query in S33).
/// The full query-from-backend integration is deferred to a later story.
#[derive(Debug, Clone, Serialize)]
pub struct RepoInfo {
    pub name: String,
    pub path: String,
    pub last_job_id: String,
}

/// Shared, append-only registry of repos seen by completed jobs.
#[derive(Clone, Debug, Default)]
pub struct RepoRegistry {
    repos: Arc<Mutex<Vec<RepoInfo>>>,
}

impl RepoRegistry {
    pub fn new() -> Self {
        Self::default()
    }

    /// Record a repo from a completed job.  Upserts by `name`.
    pub fn upsert(&self, info: RepoInfo) {
        let mut repos = self.repos.lock().expect("repos lock poisoned");
        if let Some(existing) = repos.iter_mut().find(|r| r.name == info.name) {
            *existing = info;
        } else {
            repos.push(info);
        }
    }

    /// Return a snapshot of all known repos.
    pub fn list(&self) -> Vec<RepoInfo> {
        self.repos.lock().expect("repos lock poisoned").clone()
    }
}

// ── Tests ──────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    fn path_source(p: &str) -> IngestSource {
        IngestSource::Path {
            path: PathBuf::from(p),
        }
    }

    #[test]
    fn enqueue_returns_job_id_and_get_returns_queued() {
        let (queue, _rx) = JobQueue::new(64);
        let id = queue
            .enqueue(path_source("/repo"), IngestOptions::default())
            .expect("enqueue must succeed");
        let rec = queue.get(&id).expect("job must exist");
        assert_eq!(rec.state, JobState::Queued);
        assert_eq!(rec.phase, JobPhase::Bootstrap);
        assert_eq!(rec.progress, 0.0);
    }

    #[test]
    fn mark_running_transitions_state() {
        let (queue, _rx) = JobQueue::new(64);
        let id = queue
            .enqueue(path_source("/repo"), IngestOptions::default())
            .unwrap();
        queue.mark_running(&id);
        let rec = queue.get(&id).unwrap();
        assert_eq!(rec.state, JobState::Running);
        assert!(rec.started_at.is_some());
    }

    #[test]
    fn mark_done_sets_progress_to_1() {
        let (queue, _rx) = JobQueue::new(64);
        let id = queue
            .enqueue(path_source("/repo"), IngestOptions::default())
            .unwrap();
        queue.mark_running(&id);
        queue.mark_done_with_counts(&id, 3, 0, 10, 20);
        let rec = queue.get(&id).unwrap();
        assert_eq!(rec.state, JobState::Done);
        assert_eq!(rec.progress, 1.0);
        assert_eq!(rec.tus_done, 3);
        assert_eq!(rec.tus_total, 3);
        assert_eq!(rec.nodes, 10);
        assert_eq!(rec.edges, 20);
        assert!(rec.finished_at.is_some());
    }

    #[test]
    fn mark_failed_records_error() {
        let (queue, _rx) = JobQueue::new(64);
        let id = queue
            .enqueue(path_source("/repo"), IngestOptions::default())
            .unwrap();
        queue.mark_failed(&id, "compile_commands not found".to_owned());
        let rec = queue.get(&id).unwrap();
        assert_eq!(rec.state, JobState::Failed);
        assert_eq!(rec.error.as_deref(), Some("compile_commands not found"));
    }

    #[test]
    fn list_filters_by_state() {
        let (queue, _rx) = JobQueue::new(64);
        let id1 = queue
            .enqueue(path_source("/a"), IngestOptions::default())
            .unwrap();
        let _id2 = queue
            .enqueue(path_source("/b"), IngestOptions::default())
            .unwrap();
        queue.mark_running(&id1);

        let running = queue.list(Some(JobState::Running));
        assert_eq!(running.len(), 1);
        assert_eq!(running[0].job_id, id1);

        let queued = queue.list(Some(JobState::Queued));
        assert_eq!(queued.len(), 1);

        let all = queue.list(None);
        assert_eq!(all.len(), 2);
    }

    #[test]
    fn list_orders_newest_first() {
        let (queue, _rx) = JobQueue::new(64);
        let older = queue
            .enqueue(path_source("/older"), IngestOptions::default())
            .unwrap();
        let newer = queue
            .enqueue(path_source("/newer"), IngestOptions::default())
            .unwrap();

        {
            let mut jobs = queue.jobs.lock().unwrap();
            jobs.get_mut(&older).unwrap().enqueued_at = 1;
            jobs.get_mut(&newer).unwrap().enqueued_at = 2;
        }

        let all = queue.list(None);
        assert_eq!(all[0].job_id, newer);
        assert_eq!(all[1].job_id, older);
    }

    #[test]
    fn job_record_serializes_progress_counters() {
        let (queue, _rx) = JobQueue::new(64);
        let id = queue
            .enqueue(path_source("/repo"), IngestOptions::default())
            .unwrap();
        queue.update_progress(
            &id,
            JobPhase::Phase1,
            0.5,
            JobProgressCounters {
                tus_done: 2,
                tus_total: 4,
                nodes: 10,
                edges: 20,
            },
        );
        let rec = queue.get(&id).unwrap();
        let json = serde_json::to_value(rec).unwrap();
        assert_eq!(json["tus_done"], 2);
        assert_eq!(json["tus_total"], 4);
        assert_eq!(json["nodes"], 10);
        assert_eq!(json["edges"], 20);
    }

    #[test]
    fn enqueue_returns_none_when_full() {
        // Capacity 1: fill it, then check that the second enqueue returns None.
        let (queue, _rx) = JobQueue::new(1);
        let first = queue.enqueue(path_source("/a"), IngestOptions::default());
        assert!(first.is_some(), "first enqueue must succeed");
        let second = queue.enqueue(path_source("/b"), IngestOptions::default());
        assert!(second.is_none(), "second enqueue must fail when full");
    }

    #[test]
    fn repo_registry_upserts_by_name() {
        let reg = RepoRegistry::new();
        reg.upsert(RepoInfo {
            name: "my_repo".to_owned(),
            path: "/old".to_owned(),
            last_job_id: "id-1".to_owned(),
        });
        reg.upsert(RepoInfo {
            name: "my_repo".to_owned(),
            path: "/new".to_owned(),
            last_job_id: "id-2".to_owned(),
        });
        let list = reg.list();
        assert_eq!(list.len(), 1);
        assert_eq!(list[0].path, "/new");
    }

    // ── S4 / AC-6 tests ────────────────────────────────────────────────────────

    /// All TUs succeeded → `status = completed`.
    #[test]
    fn outcome_completed_when_no_failures() {
        let (queue, _rx) = JobQueue::new(64);
        let id = queue
            .enqueue(path_source("/repo"), IngestOptions::default())
            .unwrap();
        queue.mark_running(&id);
        queue.mark_done_with_counts(&id, 7, 0, 100, 50);
        let rec = queue.get(&id).unwrap();
        assert_eq!(rec.failed_tu_count, 0);
        assert_eq!(rec.status, Some(JobOutcome::Completed));
    }

    /// Some TUs failed → `status = completed_with_errors`.
    #[test]
    fn outcome_completed_with_errors_when_partial_failures() {
        let (queue, _rx) = JobQueue::new(64);
        let id = queue
            .enqueue(path_source("/repo"), IngestOptions::default())
            .unwrap();
        queue.mark_running(&id);
        queue.mark_done_with_counts(&id, 7, 2, 80, 40);
        let rec = queue.get(&id).unwrap();
        assert_eq!(rec.failed_tu_count, 2);
        assert_eq!(rec.status, Some(JobOutcome::CompletedWithErrors));
    }

    /// All TUs failed → `status = failed`.
    #[test]
    fn outcome_failed_when_all_failures() {
        let (queue, _rx) = JobQueue::new(64);
        let id = queue
            .enqueue(path_source("/repo"), IngestOptions::default())
            .unwrap();
        queue.mark_running(&id);
        queue.mark_done_with_counts(&id, 7, 7, 0, 0);
        let rec = queue.get(&id).unwrap();
        assert_eq!(rec.failed_tu_count, 7);
        assert_eq!(rec.status, Some(JobOutcome::Failed));
    }

    /// Zero TUs, zero failures → treated as `completed` (consistent with ADR-3
    /// / ADR-4: "no work done is not a failure").
    #[test]
    fn outcome_completed_when_zero_tus() {
        let (queue, _rx) = JobQueue::new(64);
        let id = queue
            .enqueue(path_source("/repo"), IngestOptions::default())
            .unwrap();
        queue.mark_running(&id);
        queue.mark_done_with_counts(&id, 0, 0, 0, 0);
        let rec = queue.get(&id).unwrap();
        assert_eq!(rec.failed_tu_count, 0);
        assert_eq!(rec.status, Some(JobOutcome::Completed));
    }

    /// AC-7: a legacy JSON record without `failed_tu_count` and `status` keys
    /// must deserialise successfully, defaulting both to zero / None.
    #[test]
    fn legacy_json_without_new_fields_deserialises() {
        let json = r#"{
            "job_id": "legacy-id-001",
            "state": "done",
            "phase": "done",
            "progress": 1.0,
            "tus_done": 5,
            "tus_total": 5,
            "nodes": 42,
            "edges": 10,
            "source": {"path": "/repo"},
            "options": {},
            "enqueued_at": 1716000000
        }"#;
        let rec: JobRecord = serde_json::from_str(json)
            .expect("legacy JSON must deserialise without failed_tu_count/status keys (AC-7)");
        assert_eq!(rec.failed_tu_count, 0, "should default to 0");
        assert!(rec.status.is_none(), "should default to None");
        assert_eq!(rec.job_id, "legacy-id-001");
    }

    /// Queued/running records serialise without `status` key present
    /// (`skip_serializing_if = "Option::is_none"`).
    #[test]
    fn queued_record_serialises_without_status_key() {
        let (queue, _rx) = JobQueue::new(64);
        let id = queue
            .enqueue(path_source("/repo"), IngestOptions::default())
            .unwrap();
        let rec = queue.get(&id).unwrap();
        let json = serde_json::to_value(&rec).unwrap();
        assert!(
            json.get("status").is_none(),
            "queued record must not contain `status` key"
        );
    }

    /// Running record also serialises without `status` key.
    #[test]
    fn running_record_serialises_without_status_key() {
        let (queue, _rx) = JobQueue::new(64);
        let id = queue
            .enqueue(path_source("/repo"), IngestOptions::default())
            .unwrap();
        queue.mark_running(&id);
        let rec = queue.get(&id).unwrap();
        let json = serde_json::to_value(&rec).unwrap();
        assert!(
            json.get("status").is_none(),
            "running record must not contain `status` key"
        );
    }

    /// Completed record serialises with `status` key set to snake_case value.
    #[test]
    fn completed_record_serialises_status_as_snake_case() {
        let (queue, _rx) = JobQueue::new(64);
        let id = queue
            .enqueue(path_source("/repo"), IngestOptions::default())
            .unwrap();
        queue.mark_running(&id);
        queue.mark_done_with_counts(&id, 7, 2, 100, 50);
        let rec = queue.get(&id).unwrap();
        let json = serde_json::to_value(&rec).unwrap();
        assert_eq!(
            json["status"],
            serde_json::Value::String("completed_with_errors".to_owned())
        );
        assert_eq!(json["failed_tu_count"], 2);
    }

    // ── Terminal-job eviction tests ────────────────────────────────────────────

    /// Inserting more than MAX_TERMINAL_JOBS terminal records causes the oldest
    /// to be evicted; active (queued/running) jobs are never evicted.
    #[test]
    fn terminal_eviction_removes_oldest_beyond_cap() {
        // Use a large capacity so the channel never blocks the enqueue.
        let (queue, _rx) = JobQueue::new(MAX_TERMINAL_JOBS + 64);

        // Enqueue and mark done MAX_TERMINAL_JOBS + 1 jobs.
        let mut ids: Vec<String> = Vec::new();
        for i in 0..=(MAX_TERMINAL_JOBS as u64) {
            let id = queue
                .enqueue(path_source("/repo"), IngestOptions::default())
                .unwrap_or_else(|| panic!("enqueue failed at i={i}"));
            queue.mark_done_with_counts(&id, 1, 0, 0, 0);
            ids.push(id);
        }

        // mark_done stamps finished_at with wall-clock seconds, which ties for
        // jobs completed within the same second, so end-to-end we can only
        // assert the cap — eviction order among ties is unspecified.
        let total = queue.list(None).len();
        assert!(
            total <= MAX_TERMINAL_JOBS,
            "expected at most {MAX_TERMINAL_JOBS} records after eviction, got {total}"
        );

        // Deterministic ordering check against the eviction function itself:
        // distinct finished_at values, oldest must go, newest must stay.
        {
            let mut jobs = queue.jobs.lock().unwrap();
            jobs.clear();
            for i in 0..=(MAX_TERMINAL_JOBS as u64) {
                let id = format!("job-{i:05}");
                let mut rec =
                    JobRecord::new(id.clone(), path_source("/repo"), IngestOptions::default());
                rec.state = JobState::Done;
                rec.finished_at = Some(i + 1);
                jobs.insert(id, rec);
            }
            JobQueue::evict_terminal_overflow(&mut jobs);
            assert_eq!(jobs.len(), MAX_TERMINAL_JOBS);
            assert!(
                !jobs.contains_key("job-00000"),
                "oldest terminal job should have been evicted"
            );
            let newest = format!("job-{:05}", MAX_TERMINAL_JOBS);
            assert!(
                jobs.contains_key(&newest),
                "newest terminal job must not be evicted"
            );
        }
    }

    /// Active (queued/running) jobs are never evicted even when the terminal
    /// job count exceeds the cap.
    #[test]
    fn terminal_eviction_never_removes_active_jobs() {
        let cap = MAX_TERMINAL_JOBS + 32;
        let (queue, _rx) = JobQueue::new(cap + 10);

        // Enqueue one job and keep it running (active).
        let active_id = queue
            .enqueue(path_source("/active"), IngestOptions::default())
            .expect("enqueue active job");
        queue.mark_running(&active_id);

        // Fill terminal jobs beyond the cap.
        for i in 0..=(MAX_TERMINAL_JOBS as u64) {
            let id = queue
                .enqueue(path_source("/repo"), IngestOptions::default())
                .unwrap_or_else(|| panic!("enqueue failed at i={i}"));
            queue.mark_failed(&id, format!("err-{i}"));
        }

        // The running job must survive eviction.
        assert!(
            queue.get(&active_id).is_some(),
            "running job must not be evicted by terminal overflow"
        );
        let rec = queue.get(&active_id).unwrap();
        assert_eq!(rec.state, JobState::Running);
    }
}
