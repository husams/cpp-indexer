# ADR-4: Daemon `JobRecord.failed_tu_count` and status-enum extension

Status: accepted
Date: 2026-05-19
Stage: architect (tu-parse-fail v3)
Covers AC: AC-6, AC-7 (S4); addresses scenarios Open questions 1 and 5

## Context
S4 / AC-6 requires `GET /v1/jobs/{id}` (already implemented; route lives at
src/api/routes.rs and the response struct is `JobRecord` at
src/api/jobs.rs:97) to include:

1. `failed_tu_count: integer` in the response body.
2. `status` field with three values: `completed`, `completed_with_errors`,
   `failed` — derived from `failed_tu_count` vs `tus_total`.

AC-7 requires back-compat: legacy persisted JSON records without
`failed_tu_count` must deserialise (serde default = 0).

However: `JobRecord` today has a `state` field (not `status`) with a
**different** semantic (lifecycle: `queued | running | done | failed`).
Operators expect `state` to continue to mean "is the job running?". The new
field `status` is a *result classification* of completed jobs.

Forces:
- Cannot remove or rename `state` (AC-7 forbids removing existing fields).
- Daemon currently has no persistence (src/api/jobs.rs:5 "Jobs are stored
  in-memory only"). AC-7's "legacy JSON record" clause is therefore
  forward-looking — when persistence ships, the schema must already tolerate
  missing fields. Implementing serde defaults now satisfies AC-7 without a
  persistence implementation.
- `JobQueue::mark_done_with_counts` (src/api/jobs.rs:334) is the
  single chokepoint that the daemon worker hits on pipeline success
  (src/bin/daemon.rs:135). We extend that signature rather than introduce a
  parallel API.
- `total == 0 && failed == 0` (scenarios Open question 1, repeated for S4):
  consistent with ADR-3, classify as `completed` (no work done is not a
  failure).

## Decision

### 1. Add field to `JobRecord`

```rust
pub struct JobRecord {
    // ... existing fields ...
    pub tus_done: u64,
    pub tus_total: u64,
    #[serde(default)]            // AC-7 back-compat
    pub failed_tu_count: u64,
    // ... rest unchanged ...
}
```

`#[serde(default)]` on the field gives the AC-7 guarantee: any deserialiser
encountering a legacy JSON record without the field gets `0`.

### 2. Add `JobOutcome` enum + `outcome()` derivation

New enum:
```rust
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum JobOutcome {
    Completed,            // failed == 0
    CompletedWithErrors,  // 0 < failed < total
    Failed,               // failed == total (and total > 0)
}
```

Derived on the fly from existing fields (NOT stored separately, no extra
state to keep in sync). Serialise via an `#[serde(skip)]` field plus a
custom getter — simplest: introduce a method and include it in the response
serialisation via a wrapper. Two viable shapes:

**Shape A (chosen): add a serialised computed field via `serialize_with`** —
add a transparent `#[serde(rename = "status")]` getter:

```rust
impl JobRecord {
    fn computed_outcome(&self) -> Option<JobOutcome> {
        if self.state != JobState::Done { return None; }
        if self.failed_tu_count == 0 { return Some(JobOutcome::Completed); }
        if self.failed_tu_count >= self.tus_total {
            return Some(JobOutcome::Failed);
        }
        Some(JobOutcome::CompletedWithErrors)
    }
}
```

Use a serde-flatten companion struct for the on-the-wire `status` field, or
simply add a real field `status: Option<JobOutcome>` that the daemon writes
inside `mark_done_with_counts`. The simpler path is the latter — **store
`status: Option<JobOutcome>` and set it once at completion**:

```rust
pub struct JobRecord {
    // ...
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub status: Option<JobOutcome>,
    #[serde(default)]
    pub failed_tu_count: u64,
}
```

`status` is `None` while the job is queued/running (the existing `state`
field tells operators "running"); set to `Some(_)` exactly when
`mark_done_with_counts` or `mark_failed` is called. This avoids on-the-fly
derivation and keeps the response shape simple. `skip_serializing_if` keeps
the field out of in-flight job responses (no semantic regression).

### 3. Extend `mark_done_with_counts` signature

```rust
pub fn mark_done_with_counts(
    &self,
    job_id: &str,
    tus_total: u64,
    failed_tu_count: u64,        // NEW
    nodes: u64,
    edges: u64,
) {
    // ... existing body, plus:
    rec.failed_tu_count = failed_tu_count;
    rec.status = Some(if failed_tu_count == 0 {
        JobOutcome::Completed
    } else if failed_tu_count >= tus_total {
        JobOutcome::Failed
    } else {
        JobOutcome::CompletedWithErrors
    });
}
```

`total == 0 && failed == 0` → first arm (`Completed`). Consistent with ADR-3.

### 4. Update daemon worker call site

src/bin/daemon.rs:135 — pass the new field:
```rust
queue_worker.mark_done_with_counts(
    &job_id,
    stats.tu_count.try_into().unwrap_or(u64::MAX),
    stats.failed_tu_count.try_into().unwrap_or(u64::MAX),  // NEW
    stats.nodes_written,
    stats.edges_written,
);
```

### 5. `mark_failed` behaviour
`mark_failed` is the pipeline-error path (anyhow bubble). Keep `status =
None` there — `state = Failed` already conveys the lifecycle. Operators
distinguish "pipeline crashed" (`state=failed`, no `status`) from "all TUs
failed to parse" (`state=done`, `status=failed`). This matches AC-6's three
status values exactly (no fourth value for crash).

### 6. Wire-format contract
Response JSON for a completed-with-errors job:
```json
{
  "job_id": "...",
  "state": "done",
  "status": "completed_with_errors",
  "failed_tu_count": 2,
  "tus_total": 7,
  // ... existing fields unchanged ...
}
```

For a queued/running job, neither `status` nor (effectively)
`failed_tu_count: 0` carries meaning — `skip_serializing_if` keeps `status`
out, `failed_tu_count` is still emitted as `0` (it's a real field, not
`Option`).

## Alternatives considered

| Option | Trade-off | Verdict |
| ------ | --------- | ------- |
| **A. Reuse existing `state` field, add new variants** (`done_with_errors`) | Breaks AC-7 (rename/expand existing enum); clients pattern-matching `state` would silently fall through. | rejected — back-compat |
| **B. Add separate `status: Option<JobOutcome>` field** (chosen) | Additive; no client churn; `skip_serializing_if` keeps queued/running response unchanged. | **accepted** |
| **C. Compute `status` on every read via `serialize_with`** | Saves 1 byte of state but adds complexity to every getter; no benefit. | rejected — complexity |
| **D. Map `total == 0 && failed == 0` to a new `vacant` status** | Adds a 4th value not in AC-6; operator surface area without operator demand. | rejected — scope |

## Consequences

Positive:
- Two additive fields, no breaking changes; AC-7 satisfied by
  `#[serde(default)]`.
- Three-way classification computed once at completion (no read-path cost).
- `state` semantics untouched — existing clients pattern-matching on
  `queued | running | done | failed` keep working.

Negative / follow-ups:
- Two failure surfaces now: `state == failed` (pipeline crash) and
  `state == done && status == failed` (all TUs failed). Documented in API
  reference (doc-writer hand-off downstream).
- Daemon does not honour `--fail-on-tu-error` semantics (no equivalent flag
  on the daemon); ratio threshold is a CLI-only feature. If operators want
  daemon-side gating, file a follow-up. Out of scope for Issue 0001.

## References
- requirements.md §S4, AC-6, AC-7
- scenarios.md Feature S4 (7 scenarios), Open questions 1 and 5
- /Users/husam/workspace/cpp-indexer/src/api/jobs.rs:97 (JobRecord struct)
- /Users/husam/workspace/cpp-indexer/src/api/jobs.rs:334 (mark_done_with_counts)
- /Users/husam/workspace/cpp-indexer/src/bin/daemon.rs:135 (worker call site)
- ADR-2 (provides `stats.failed_tu_count`)
- ADR-3 (consistent `0/0 → success` semantics)
- Wiki: [[pages/code/cpp-indexer]] — confirms `GET /v1/jobs/{id}` route
- Cognee tag: task:tu-parse-fail, role:architect
