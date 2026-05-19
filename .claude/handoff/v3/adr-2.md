# ADR-2: `PipelineStats.failed_tu_count` field, threading, and summary format

Status: accepted
Date: 2026-05-19
Stage: architect (tu-parse-fail v3)
Covers AC: AC-4 (S2), supports AC-5 (S3) and AC-6 (S4)

## Context
`PipelineStats` (src/pipeline/mod.rs:653) currently exposes `tu_count`,
`cache_hits`, `partial_tu_count`, `nodes_written`, `edges_written`. The
parallel-phase counter `ParallelStats.tu_error` is captured at
src/pipeline/mod.rs:204 in a local `parallel_stats` value but **never written
back to `stats`**, so callers (the CLI binary at src/bin/index.rs:178, the
daemon worker at src/bin/daemon.rs:134) cannot see how many TUs failed.

S2 (AC-4) requires the closing summary line:

```text
cxg-index: done — <T> TUs | <P> partial | <F> failed | <N> nodes | <E> edges
```

Token order MUST remain stable; `failed` is inserted between `partial` and
`nodes`. Today's line at src/bin/index.rs:178 lacks `failed` entirely.

Forces:
- S3 and S4 both depend on a single counter that survives from
  `ParallelStats.tu_error` through to the CLI exit-mapping step (ADR-3) and
  the daemon `mark_done` path (ADR-4).
- `parallel_stats.tu_partial` is already truncated through
  `usize::try_into(...).unwrap_or(usize::MAX)` at line 204 — the same pattern
  is used for `partial_tu_count: usize`. Consistency wins.
- Tests use `PipelineStats::default()` widely; the new field must default to
  zero and not require updates at every call-site.

## Decision

1. **Add field** to `PipelineStats` (src/pipeline/mod.rs):
   ```rust
   pub struct PipelineStats {
       pub tu_count: usize,
       pub cache_hits: u64,
       pub partial_tu_count: usize,
       pub failed_tu_count: usize,   // NEW — TUs with libclang hard error
       pub nodes_written: u64,
       pub edges_written: u64,
   }
   ```
   Type: `usize`, matching `tu_count` and `partial_tu_count`. `Default` derive
   stays — zero is the correct unset value.

2. **Thread the counter** at src/pipeline/mod.rs:204, immediately after the
   existing `stats.partial_tu_count = …` line:
   ```rust
   stats.failed_tu_count = parallel_stats.tu_error
       .try_into().unwrap_or(usize::MAX);
   ```
   Same truncation pattern as `partial_tu_count`. No new threading is needed —
   `parallel_stats` is already in scope.

3. **Closing summary helper**. Extract a function in
   `src/pipeline/mod.rs` (next to `PipelineStats`):
   ```rust
   impl PipelineStats {
       pub fn closing_summary(&self) -> String {
           format!(
               "cxg-index: done — {} TUs | {} partial | {} failed | {} nodes | {} edges",
               self.tu_count,
               self.partial_tu_count,
               self.failed_tu_count,
               self.nodes_written,
               self.edges_written,
           )
       }
   }
   ```
   Replace the ad-hoc `eprintln!` at src/bin/index.rs:178 with
   `eprintln!("{}", stats.closing_summary());`. The function is the single
   source of truth for the format string — snapshot test in S2 AC-4 asserts on
   it directly without touching stderr.

4. **Phase 1 info line update** (src/pipeline/mod.rs:222 — already includes
   "{} failed TU(s)"). No change required; that log line already exists with
   the correct counter, only `PipelineStats` was missing it.

5. **No `SCHEMA_VERSION` bump.** The field affects the in-memory struct and
   stderr line only — not Parquet shards, not the wire-schema. Confirmed
   against requirements.md NFR row 3.

6. **`parse-summary.sh` audit** (deferred to developer, tracked by Open
   question 2). Architect decision: token order is fixed (TUs, partial,
   failed, nodes, edges); developer greps `tools/release/` for any consumer
   during PR and updates iff present. Not a blocking design decision.

## Alternatives considered

| Option | Trade-off | Verdict |
| ------ | --------- | ------- |
| **A. `u64` field (matches `nodes_written`)** | Forces `try_into` at every consumer that compares against `tu_count: usize`. AC-5 ratio uses `failed / total` which would mix integer widths. | rejected — width churn |
| **B. `usize` field, `closing_summary()` helper** (chosen) | Single chokepoint for AC-4 format; trivial snapshot test; matches `partial_tu_count` type. | **accepted** |
| **C. Build the summary string in CLI binary, no helper** | Duplicates the format in tests; harder to keep stable across daemon (which doesn't print it but exposes the same counters). | rejected — duplication |
| **D. New `SummaryLine` struct + Display impl** | Over-engineered for a single format string. AC-4 only requires one line; no other consumer. | rejected — scope |

## Consequences

Positive:
- Single source of truth for the summary line; snapshot test pins format.
- No call-site sweep: every existing `PipelineStats::default()` continues to
  work; `failed_tu_count` defaults to 0.
- ADR-3 (exit mapping) and ADR-4 (daemon field) consume the same `usize`
  field — no further plumbing.

Negative / follow-ups:
- One-time integration touch in src/bin/index.rs:178 (replaces ad-hoc line).
- Developer must grep `tools/release/` for `parse-summary` consumers and
  update iff present (Open question 2 from scenarios.md). Not blocking.
- Cache: this change does not affect `manifest.json` (no schema change);
  cache invalidation comes from ADR-1 only.

## References
- requirements.md §S2, AC-4; NFR rows 3, 4; Risks table row 3
- scenarios.md Feature S2 (3 scenarios)
- /Users/husam/workspace/cpp-indexer/src/pipeline/mod.rs:204 (where tu_error is dropped today)
- /Users/husam/workspace/cpp-indexer/src/pipeline/mod.rs:653 (PipelineStats struct)
- /Users/husam/workspace/cpp-indexer/src/bin/index.rs:178 (ad-hoc summary today)
- /Users/husam/workspace/cpp-indexer/src/pipeline/parallel.rs:71 (ParallelStats with tu_error)
- Cognee tag: task:tu-parse-fail, role:architect
