run_id: tu-parse-fail-v3
stage: 8 of 8 — doc-writer

---

Files written:
  ~/workspace/wiki/pages/code/cpp-indexer.md   (extended — Issue 0001 fix section appended)
  ~/workspace/wiki/index.md                     (cpp-indexer entry updated)
  ~/workspace/wiki/log.md                       (entry appended: [2026-05-19] extend | cpp-indexer Issue 0001 fix)
  /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/logs/doc-writer.md  (closing log)

Verification:
  All commands quoted verbatim from runbook.md and deploy-notes.md (tu-parse-fail-v3).
  Closing summary format: `cxg-index: done — N TUs | N partial | N failed | N nodes | N edges`
    source: runbook.md §1 (quoted verbatim).
  spdlog CI invocation: `cargo test --features test-mock --test spdlog_smoke -- --ignored`
    source: deploy-notes.md §CI Matrix (quoted verbatim).
  --fail-on-tu-error default and exit semantics: source runbook.md §3.
  Daemon status enum values: source runbook.md §4 (quoted verbatim).
  status field absent while in-flight: source runbook.md §4 (`skip_serializing_if Option::is_none`).
  Cache invalidation: source runbook.md §5 and deploy-notes.md §Cache Invalidation.
  No commands invented; no paths invented.

Cross-links:
  Wiki: ~/workspace/wiki/pages/code/cpp-indexer.md
  Issue 0001: /Users/husam/workspace/cpp-indexer/docs/issues/0001-silent-tu-parse-failures.md
  Operator runbook: /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/runbook.md
  M8 page: [[pages/code/cpp-indexer-m8-structured-attrs]]
  PRD: [[pages/planning/codexgraph-cpp-prd-v1]]

References:
  Handoff inputs read:
    /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/CHARTER.md
    /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/requirements.md
    /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/design.md
    /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/test-report.md
    /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/runbook.md
    /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/deploy-notes.md
  Cognee tags: task:tu-parse-fail, role:doc-writer
