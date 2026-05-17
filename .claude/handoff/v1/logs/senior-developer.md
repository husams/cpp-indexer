# senior-developer log — cpp-indexer v1 plan

date: 2026-05-17
mode: plan
deliver: /Users/husam/workspace/cpp-indexer/.claude/handoff/v1/plan.md

## Inputs verified
- CHARTER.md, requirements.md (M1–M7 AC-IDs), design.md, scenarios.md (referenced via design)
- adr-1 .. adr-10: ALL `Status: accepted` — pre-condition CHARTER I2 satisfied.
- rust-conventions skill loaded; canonical toolchain (`cargo fmt`, `cargo clippy --all-targets --all-features -- -D warnings`, `cargo nextest run`, `cargo build`) baked into every story's exit criteria.

## Plan shape
- 39 stories grouped into 20 waves.
- 21 stories marked `parallel-safe: true` (disjoint files-to-touch).
- W0 is the mandatory `S01-init-crate` foundation (Cargo.toml with libclang/clang/axum/tokio/rayon/neo4rs/indradb-proto/git2/arrow/parquet/etc., CI scaffold, three bin targets, module stubs per design §3).
- Every story carries: title, AC-IDs, files-to-touch (absolute or repo-relative per design layout), tests, exit-criteria with explicit commands, ADR/requirements references, parallel-safe flag.
- Milestone exit gates (S13, S16, S21, S25, S29, S32, S39) are non-parallel and produce signal for next wave.

## Open items for developer wave dispatch
- S11/S12/S13/S16/S23/S25/S29/S39 integration tests gated `#[ignore]` until Neo4j+IndraDB compose stack provisioned (devops dependency noted in requirements §10).
- Boost.Optional / Chromium / LLVM fixtures must be acquired before S16/S21/S29 can pass — checklist files committed in those stories.
- S32 (M6 agent gate) is manual; depends on external CodexGraph Streamlit agent.

## Failure modes covered
- MISSING_EXIT_CRITERIA: every story has ≥3 exit commands.
- Worktree convention from CHARTER applied for all parallel-safe stories.

## Cognee
- Ingest queued after this log written.
