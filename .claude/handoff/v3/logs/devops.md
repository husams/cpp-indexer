run_id: tu-parse-fail-v3
stage: 7 of 8 — devops
date: 2026-05-19
target-context: NONE (no-deploy; documentation-only)

## Actions taken

- Read CHARTER.md, design.md, implementation-notes.md (S1–S5, S1-QD1), test-report.md, plan.md.
- Confirmed test-report.md has no open QA_DEFECT entries (QD-1 status: closed).
- Confirmed I4 invariant satisfied before writing deliverables.
- No cluster apply performed; target-context is NONE per dispatch.
- No kubectl, ArgoCD, or Vault operations executed.

## Deliverables written

- /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/deploy-notes.md
  - Sections: scope, back-compat, cache invalidation, CI matrix (spdlog_smoke), pre-existing schema_drift, release checklist.
  - Calls out false-green risk on misconfigured spdlog_smoke CI runner.
- /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/runbook.md
  - Sections: closing summary format, exit-code table (including clap collision note), --fail-on-tu-error semantics, daemon GET /v1/jobs/{id} new fields with example JSON, cache invalidation, rollback, on-call notes.

## Key findings

- spdlog_smoke test skips gracefully on missing tools — CI must pre-flight assert git/cmake/c++ presence; do not rely on test to catch misconfiguration.
- --fail-on-tu-error default 1.0 is back-compatible; existing partial-failure runs still exit 0.
- Daemon wire schema: failed_tu_count and status are additive; no rolling-restart ordering required.
- Cache invalidation is one-shot on first post-upgrade run; manifest.json format unchanged but content differs.
- Pre-existing schema_drift failure confirmed not introduced by this branch.

## Failure codes

No DEPLOY_DRIFT (target-context: NONE, no apply attempted).
No other failure codes triggered.

## References

- CHARTER: /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/CHARTER.md
- design.md, implementation-notes.md, test-report.md, plan.md — handoff/v3/
- adr-1.md through adr-4.md — handoff/v3/
- Cognee tags: task:tu-parse-fail, role:devops
