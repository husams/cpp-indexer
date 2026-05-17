run_id: cpp-indexer-v1
handoff_dir: /Users/husam/workspace/cpp-indexer/.claude/handoff/v1/

Blackboard paths (authoritative):
  requirements:   /Users/husam/workspace/cpp-indexer/.claude/handoff/v1/requirements.md
  scenarios:      /Users/husam/workspace/cpp-indexer/.claude/handoff/v1/scenarios.md
  design:         /Users/husam/workspace/cpp-indexer/.claude/handoff/v1/design.md
  adrs:           /Users/husam/workspace/cpp-indexer/.claude/handoff/v1/adr-<n>.md   (one per decision)
  plan:           /Users/husam/workspace/cpp-indexer/.claude/handoff/v1/plan.md
  impl-notes:     /Users/husam/workspace/cpp-indexer/.claude/handoff/v1/implementation-notes.md
  impl-notes-per: /Users/husam/workspace/cpp-indexer/.claude/handoff/v1/logs/developer-<story-slug>.md
  test-report:    /Users/husam/workspace/cpp-indexer/.claude/handoff/v1/test-report.md
  test-report-per:/Users/husam/workspace/cpp-indexer/.claude/handoff/v1/logs/qa-engineer-<story-slug>.md
  deploy-notes:   /Users/husam/workspace/cpp-indexer/.claude/handoff/v1/deploy-notes.md
  runbook:        /Users/husam/workspace/cpp-indexer/.claude/handoff/v1/runbook.md
  docs-changes:   /Users/husam/workspace/cpp-indexer/.claude/handoff/v1/docs-changes.md
  logs:           /Users/husam/workspace/cpp-indexer/.claude/handoff/v1/logs/<role>.md

Project context:
  language: rust
  project_root: /Users/husam/workspace/cpp-indexer
  public_repo: https://github.com/husams/cpp-indexer (Apache-2.0, default branch main)
  scope: ENTIRE PROJECT per PRD v1.1 (M1 Foundations through M7 Ops + REST API → v1 GA)
  PRD: ~/workspace/wiki/pages/planning/codexgraph-cpp-prd-v1.md (v1.1 draft)
  engineering_plan: ~/workspace/wiki/pages/planning/codexgraph-cpp-libclang-rust.md (v1.1 draft)
  naming: public artifacts use "cpp-indexer" only. Internal wiki planning docs may still use codexgraph-cpp-* slug.
  parallelization: use git worktrees under /Users/husam/workspace/cpp-indexer/.worktrees/<story-slug>/ on branch story/<story-slug> for parallel-safe developer stories.

Locked-in decisions (from PRD v1.1 — do NOT re-litigate):
  - Rust + libclang 18.
  - BOTH Neo4j and IndraDB sinks mandatory in default binary (no Cargo feature gates). Shared GraphSink trait.
  - USR is global primary key.
  - Parquet shards stage Phase 1 output before DB writes.
  - Cross-repo EXTERNAL_REF edges with canonicalisation for system headers.
  - Ingestion granularity: file / directory / repo via positional path.
  - Auto-detect compile_commands.json by walking upward, probing build/, out/, cmake-build-*/, stopping at .git. Hard error on miss.
  - cxg-daemon first-class with REST control plane (POST /v1/ingest, GET /v1/jobs/{id}, POST /v1/reset, GET /v1/repos, GET /v1/status, GET /metrics), bearer-token auth on writes.
  - Git ingestion: POST /v1/ingest with git_url; clone into [workspace].dir; PAT via env var; host allowlist; SSH deferred to v2.
  - Binary surface: cxg-index, cxg-resolve-cross-repo, cxg-daemon.
  - Crate modules: api/ (axum REST), workspace/ (git2 clone manager), bootstrap/autodetect.rs.

Open PRD questions to RESOLVE in ADRs (not defer):
  - Q5 (cpp-mcp boundary) — architect MUST resolve in adr-1.md before plan dispatch. Blocker per project_status memory.
  - Q2 (build-config handling), Q3 (C++20 modules timing), Q4 (cross-repo schema versioning), Q7 (multi-tenant daemon), Q8 (Bazel compile_commands.json) — architect SHOULD resolve in ADRs where they affect M1–M7 scope; may explicitly defer with rationale if out of scope for v1 GA.
  - Q6 (git SSH auth) — accepted as deferred to v2.

Cross-stage invariants:
  I1. requirements.md MUST contain acceptance criteria before architect dispatch.
  I2. All adr-*.md Status MUST NOT be "proposed" before developer dispatch.
  I3. plan.md MUST list exit-criteria commands before any developer dispatch.
  I4. test-report.md MUST exist with no open QA_DEFECT entries before devops dispatch.

Traceability chain:
  story (requirements.md) → ADR (adr-N.md) → code (plan.md files-to-touch) → test (test-report.md)
  Every plan.md story MUST reference the AC IDs it satisfies.
  Every test in test-report.md MUST reference the scenario ID it covers.

Failure taxonomy (named codes; ownership in parentheses):
  MISSING_ACCEPTANCE_CRITERIA  — requirements.md has a story without AC (product-manager)
  ADR_UNRESOLVED               — adr-*.md Status == "proposed" at dev dispatch time (architect)
  MISSING_EXIT_CRITERIA        — plan.md story has no exit-criteria commands (senior-developer)
  TEST_FAIL                    — exit-gate test command non-zero (developer)
  LINT_FAIL                    — exit-gate lint command non-zero (developer)
  BUILD_FAIL                   — exit-gate build/formatter command non-zero (developer)
  REVIEW_BLOCK                 — review.md verdict == changes-requested with open finding-ids (senior-developer)
  QA_DEFECT                    — test-report.md lists unresolved defect IDs (qa-engineer)
  DEPLOY_DRIFT                 — deploy-notes.md context != current-context at apply time (devops)

Retry termination rule:
  - Developer retry: exits when all LINT_FAIL/TEST_FAIL/BUILD_FAIL codes clear (max 3 passes inside developer).
  - Developer-from-review retry: exits when all REVIEW_BLOCK finding-ids are addressed.
  - Developer-from-qa retry: exits when all QA_DEFECT ids in test-report.md are resolved.
  Max 3 retry passes per signal set; surface to user if not resolved after exhaustion.

Handoff rule: all cross-agent data passes as file paths; never inline content from one agent into another dispatch.

Git worktree convention (for parallel developer stories):
  base_branch: main
  worktree_root: /Users/husam/workspace/cpp-indexer/.worktrees/
  per-story: git worktree add .worktrees/<story-slug> -b story/<story-slug> main
  merge: developer commits + pushes its branch; coordinator (or user) merges to main between waves.
