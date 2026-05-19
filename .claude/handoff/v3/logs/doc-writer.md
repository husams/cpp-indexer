run_id: tu-parse-fail-v3
stage: 8 of 8 — doc-writer
date: 2026-05-19

## Work performed

Extended `~/workspace/wiki/pages/code/cpp-indexer.md` with Issue 0001 fix section.

### Page changes

- Header metadata updated: `last_updated` 2026-05-18 → 2026-05-19; `sources` 7 → 14;
  `status` extended; `name` extended; `tags` extended with `issue-0001`, `tu-parse-fail`.
- REST table row for `GET /v1/jobs/{id}` updated with `failed_tu_count` / `status` note and
  anchor link to the new section.
- New section appended: "Issue 0001 fix — Silent TU Parse Failures" covering:
  - Root causes (Bug A: no failed count; Bug B: arg pollution).
  - S1: sanitise libclang args algorithm (deny-list, versioned drivers, source-repeat strip,
    cache invalidation consequence).
  - S2: closing summary format (verbatim from runbook).
  - S3: `--fail-on-tu-error` flag — values, exit semantics, exit-code collision note.
  - S4: daemon `failed_tu_count` + `status` fields — two example JSON bodies.
  - S5: spdlog smoke test invocation (verbatim from deploy-notes) + pre-flight command.
  - Issue 0001 ADRs table (ADR-0001-1..4, distinct from project ADRs 1–10).
  - Test posture table (all stories, counts, results).
- References section updated with v3 handoff paths, Issue 0001 doc, and operator runbook.

### Other files

- `~/workspace/wiki/index.md`: cpp-indexer entry extended with Issue 0001 fix summary.
- `~/workspace/wiki/log.md`: entry appended (`[2026-05-19] extend | ...`).
- `/Users/husam/workspace/cpp-indexer/.claude/handoff/v3/docs-changes.md`: written.

### Verification

All commands and format strings quoted verbatim from runbook.md and deploy-notes.md.
No commands, flags, or paths invented.
ADR numbering collision avoided: Issue 0001 ADRs prefixed 0001-N, separate from project ADRs 1–10.
M8-QD-1 (operator re-index) left unchanged; Issue 0001 QD-1 (versioned drivers, resolved) documented in test posture only.
PR pending — branch local marker written in page and index.

## Cognee tags

task:tu-parse-fail, role:doc-writer
