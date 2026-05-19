# Design — Issue 0001 fix (tu-parse-fail)

run_id: tu-parse-fail-v3
stage: 3 of 8 — architect
upstream: requirements.md (S1–S5, AC-1…AC-7), scenarios.md (Gherkin features S1–S5)
downstream: senior-developer reads this + adr-1..adr-4.md

---

## 1. Decision summary

| ADR | Decision | AC covered |
| --- | -------- | ---------- |
| ADR-1 | `sanitize_libclang_args` — deny-list strip of driver / `-c`/`-o` pair / source-repeat; basename-only driver match incl. cross-compiler suffixes; everything starting with `-` passes through | AC-1, AC-2 |
| ADR-2 | Add `PipelineStats.failed_tu_count: usize`; thread from `ParallelStats.tu_error`; centralise the closing summary line in `PipelineStats::closing_summary()` | AC-4 |
| ADR-3 | New `--fail-on-tu-error <RATIO\|never>` flag; custom `FromStr` enum; `main` returns `ExitCode`; threshold check guarded by `failed > 0`; `total == 0` → exit 0 | AC-5 |
| ADR-4 | Daemon: additive `failed_tu_count: u64` (serde default) + `status: Option<JobOutcome>` (skip when None); extend `mark_done_with_counts` signature; `state` semantics unchanged | AC-6, AC-7 |

**All four ADRs Status: accepted.** No proposed/unresolved decisions remain.

---

## 2. Architecture overview

```
                    compile_commands.json
                            │
                            ▼
            ┌───────────────────────────────┐
            │ bootstrap::compile_commands   │
            │   resolve_args   (raw split)  │
            │   sanitize_libclang_args ◀── ADR-1
            │   parse → Vec<TuEntry>        │
            └───────────────────────────────┘
                            │
                            ▼
            ┌───────────────────────────────┐
            │ pipeline::run                 │
            │   Phase 0 → 1 → 2 → 3 → 4     │
            │   ParallelStats { tu_ok,      │
            │                   tu_partial, │
            │                   tu_error }  │  ◀── existing
            │   stats.failed_tu_count = ▲   │  ◀── ADR-2 thread-through
            │                  tu_error     │
            └───────────────────────────────┘
                            │
            ┌───────────────┴────────────────┐
            ▼                                ▼
  ┌─────────────────────┐         ┌─────────────────────┐
  │ bin/index.rs (CLI)  │         │ bin/daemon.rs       │
  │  closing_summary()  │◀ ADR-2  │  run_job → mark_done│
  │  fail_on_tu_error   │◀ ADR-3  │  _with_counts(...,  │
  │    .exit_code(…)    │         │  failed, ...)       │◀ ADR-4
  │  → ExitCode         │         │                     │
  └─────────────────────┘         │ JobRecord {         │
                                  │  state,             │
                                  │  status: Option<…>, │◀ ADR-4
                                  │  failed_tu_count    │
                                  │ }                   │
                                  └─────────────────────┘
                                            │
                                            ▼
                                   GET /v1/jobs/{id}
```

## 3. Files to touch (input to senior-developer plan.md)

| Path | Change | AC | ADR |
| ---- | ------ | -- | --- |
| `src/bootstrap/compile_commands.rs` | Add `sanitize_libclang_args(raw_args, file, dir)`; call inside `parse()` after `resolve_args`; extend unit-test module with the AC-2 table | AC-1, AC-2 | ADR-1 |
| `src/pipeline/mod.rs` | Add `failed_tu_count: usize` to `PipelineStats`; thread from `parallel_stats.tu_error` at L204; add `impl PipelineStats { fn closing_summary(&self) }` + snapshot test | AC-4 | ADR-2 |
| `src/bin/index.rs` | Add `FailOnTuError` enum + `FromStr`; add `#[arg(long)]` field; change `main` return to `Result<ExitCode>`; replace ad-hoc summary line with `stats.closing_summary()`; CLI integration tests for all AC-5 ratio cases | AC-5 | ADR-3 |
| `src/api/jobs.rs` | Add `failed_tu_count: u64 #[serde(default)]` + `status: Option<JobOutcome> #[serde(skip_serializing_if = "Option::is_none")]` to `JobRecord`; add `JobOutcome` enum; extend `mark_done_with_counts` signature; unit tests for three status transitions + legacy-record deserialise | AC-6, AC-7 | ADR-4 |
| `src/bin/daemon.rs` | Update single call site at L135 to pass `stats.failed_tu_count` | AC-6 | ADR-4 |
| `tests/integration/spdlog_smoke.rs` (new file) | `#[ignore]`-gated integration test cloning spdlog and asserting `ok_tu_count >= 6`; `#[cfg(not(target_os = "windows"))]` | AC-3 | (no ADR — pure test) |
| `.github/workflows/*.yml` (existing CI file — devops scope, not developer) | Add `cargo test --ignored spdlog_smoke` job on macOS arm64 + Linux x86_64 | AC-3 | (devops) |
| `tools/release/parse-summary.sh` (audit only) | grep first; update iff exists and parses the line | AC-4 | ADR-2 |

No changes to: Parquet schemas, `SCHEMA_VERSION`, wire schema beyond the
additive job-status fields, `manifest.json` format. Sanitisation does
invalidate cached manifest entries on first deploy (one-time re-parse).

## 4. Algorithm spec (consolidated from ADRs)

### 4.1 sanitize_libclang_args (ADR-1)

```text
input:  raw_args: &[String], canonical_file: &Path, directory: &Path
output: Vec<String> (sanitised flags only)

1. if raw_args.is_empty() → return []
2. let mut i = 0; let mut out = Vec::with_capacity(raw_args.len())
3. if !raw_args[0].starts_with('-') && is_driver_basename(&raw_args[0]):
       i = 1                                   # skip leading driver
4. while i < raw_args.len():
       t = &raw_args[i]
       if t == "-c" || t == "-o":
           i += 2                              # skip flag + its value
           continue
       out.push(t.clone()); i += 1
5. # source-repeat strip
   out.retain(|t| {
       if t.starts_with('-') { return true; }   # never strip flags
       let candidate = if Path::new(t).is_absolute() {
           Path::new(t).to_path_buf()
       } else {
           directory.join(t)
       };
       canonicalise_or_self(&candidate) != *canonical_file
   })
6. return out

is_driver_basename(token):
    let stem = Path::new(token).file_name()?.to_str()?;
    matches!(stem, "cc" | "c++" | "clang" | "clang++" | "gcc" | "g++")
      || stem.ends_with("-gcc") || stem.ends_with("-g++")
      || stem.ends_with("-clang") || stem.ends_with("-clang++")
```

Complexity: O(n) tokens; one `canonicalize` syscall per non-flag candidate
(typically 0 or 1 per TU). Budget: ≤5 µs/TU is comfortable.

### 4.2 closing_summary (ADR-2)

```rust
impl PipelineStats {
    pub fn closing_summary(&self) -> String {
        format!(
            "cxg-index: done — {} TUs | {} partial | {} failed | {} nodes | {} edges",
            self.tu_count, self.partial_tu_count, self.failed_tu_count,
            self.nodes_written, self.edges_written,
        )
    }
}
```

### 4.3 exit_code (ADR-3)

```rust
impl FailOnTuError {
    fn exit_code(&self, failed: usize, total: usize) -> u8 {
        match self {
            Self::Never => 0,
            Self::Ratio(r) => {
                if failed == 0 { return 0; }
                let ratio = failed as f64 / total as f64;
                if ratio >= *r { 2 } else { 0 }
            }
        }
    }
}
```

### 4.4 JobOutcome derivation (ADR-4)

```rust
rec.status = Some(if failed == 0 {
    JobOutcome::Completed
} else if failed >= total {
    JobOutcome::Failed
} else {
    JobOutcome::CompletedWithErrors
});
```

## 5. Cross-cutting open questions — resolved

| Q | Resolution | ADR |
| - | ---------- | --- |
| Open Q1: `total == 0` exit/status | CLI exits 0 (`failed == 0` short-circuits); daemon status = `Completed` | ADR-3, ADR-4 |
| Open Q2: `parse-summary.sh` audit | Developer greps `tools/release/`; updates iff exists. Token order is fixed (TUs, partial, failed, nodes, edges). | ADR-2 |
| Open Q3: `--fail-on-tu-error` Clap shape | Custom `FromStr` enum on a single flag | ADR-3 |
| Open Q4: Flag-prefix whitelist completeness | Deny-list, not allow-list. Anything starting with `-` passes through. | ADR-1 |
| Open Q5: Daemon `GET /v1/jobs/{id}` struct location | `JobRecord` at `src/api/jobs.rs:97`; route at `src/api/routes.rs`; modified in-place with additive fields | ADR-4 |

## 6. Non-functional requirements — design satisfies

- **No new dependencies.** Uses stdlib (`std::path`, `std::process::ExitCode`,
  `std::str::FromStr`), existing `serde`, existing `clap` derive.
- **Sanitisation cost ≤5 µs/TU.** Closed-form predicate, no regex, ≤1
  `canonicalize` syscall per non-flag candidate. Empirically negligible vs
  the libclang parse cost that dominates the loop.
- **No Parquet/SCHEMA_VERSION change.** Read-side fixes only.
- **Wire schema back-compat.** Two additive fields with serde default /
  skip_serializing_if; AC-7 covered.

## 7. Test surface (input to QA scenario→test mapping)

| Scenario tag | Test location | Type |
| ------------ | ------------- | ---- |
| @AC-1 (S1, 6 scenarios) | `src/bootstrap/compile_commands.rs::tests` | unit |
| @AC-2 (S1, 9 Examples) | same — `#[test]` table | unit (table) |
| @AC-4 (S2, 3 scenarios) | `src/pipeline/mod.rs::tests` + `closing_summary` snapshot | unit |
| @AC-5 (S3, 10 scenarios) | `tests/integration/cli_fail_on_tu_error.rs` (new) using `assert_cmd` (already a dev-dep — developer to verify) | integration |
| @AC-6 (S4, 4 scenarios) | `src/api/jobs.rs::tests` + `tests/integration/api_jobs_status.rs` (new) | unit + integration |
| @AC-7 (S4, 2 scenarios) | `src/api/jobs.rs::tests` (serde roundtrip from legacy JSON literal) | unit |
| @AC-3 (S5, 3 scenarios) | `tests/integration/spdlog_smoke.rs` (new, `#[ignore]`) | integration |

## 8. Exit-criteria commands (input to plan.md)

- `cargo fmt --all -- --check`
- `cargo clippy --workspace --all-targets -- -D warnings`
- `cargo test --workspace` (all unit + non-ignored integration green)
- `cargo test --ignored spdlog_smoke` (gated; runs in CI matrix per ADR-3 scope)

## 9. Risk register (delta on top of requirements.md Risks)

| Risk | Mitigation | Owner |
| ---- | ---------- | ----- |
| Cache invalidation: existing `manifest.json` entries hash raw args → all TUs re-parse once after deploy | Document in runbook; one-shot cost; cache repopulates on next run | devops/doc-writer |
| Clap parse-error exit code 2 collides with threshold exit 2 | Stderr message disambiguates; acceptable (both "non-success") | architect (accepted) |
| `mark_done_with_counts` signature change breaks any internal caller | Single caller (`src/bin/daemon.rs:135`); update in same PR | senior-developer |
| `failed > total` edge if invariants drift | Guarded by `failed >= total → Failed`; clamped semantics | architect (accepted) |

## References
- /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/CHARTER.md
- /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/requirements.md
- /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/scenarios.md
- /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/adr-1.md
- /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/adr-2.md
- /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/adr-3.md
- /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/adr-4.md
- /Users/husam/workspace/cpp-indexer/docs/issues/0001-silent-tu-parse-failures.md
- Wiki: `[[pages/code/cpp-indexer]]`
- Cognee tag: `task:tu-parse-fail`, `role:architect`
