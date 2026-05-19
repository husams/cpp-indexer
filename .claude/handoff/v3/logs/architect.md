# architect log — tu-parse-fail v3

Date: 2026-05-19
Stage: 3 of 8

## Inputs consumed
- CHARTER.md
- requirements.md (S1–S5, AC-1…AC-7)
- scenarios.md (Gherkin features, 5 open questions)
- src/bootstrap/compile_commands.rs (resolve_args at L92)
- src/pipeline/mod.rs (PipelineStats at L653; tu_error drop at L204)
- src/pipeline/parallel.rs (ParallelStats at L71)
- src/bin/index.rs (main + ad-hoc summary at L178)
- src/bin/daemon.rs (worker at L120; mark_done_with_counts call at L135)
- src/api/jobs.rs (JobRecord at L97, mark_done_with_counts at L334)

## Deliverables
- design.md
- adr-1.md (sanitize_libclang_args; deny-list strip; basename driver match) — accepted
- adr-2.md (PipelineStats.failed_tu_count + closing_summary) — accepted
- adr-3.md (--fail-on-tu-error FromStr enum; ExitCode mapping; failed>0 guard) — accepted
- adr-4.md (JobRecord additive failed_tu_count + status: Option<JobOutcome>) — accepted

## Open questions resolved
- Q1 (0/0 ratio/status): exit 0 / status Completed
- Q3 (Clap shape): custom FromStr enum on a single flag
- Q4 (allow vs deny list): deny-list — `-`-prefixed tokens always pass
- Q5 (daemon struct location): src/api/jobs.rs:97 JobRecord, modified additively

## Deferred (not blocking)
- Q2 (parse-summary.sh audit): developer task during PR
- CI matrix wiring: devops

## Failure codes emitted
none — all four ADRs accepted before return.
