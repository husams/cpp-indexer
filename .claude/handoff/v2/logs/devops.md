---
run_id: cpp-indexer-m8-v2
role: devops
date: 2026-05-18
task-slug: cpp-indexer-m8
---

# Devops Agent Log — M8

## Inputs read

- CHARTER.md: confirmed I4 (no open QA_DEFECT blocking dispatch — QD-3 is resolved, QD-1 is a
  known open item passed through for devops resolution)
- test-report.md: QD-1 open (template_params/template_args absent from live Neo4j); QD-3 resolved
- design.md: v5 schema, 10 node fields + 2 edge fields, 3 binaries (cxg-daemon, cxg-index, cxg-resolve-cross-repo)
- implementation-notes.md: S44–S46 deviations (JSON string fallback for List<Map>, chunking arithmetic, schema_drift path)
- plan.md: binary targets confirmed via Cargo.toml [[bin]] entries
- docs/runbooks/staging-recovery.md: §6 Full Re-Index recipe (AC-S46-2 confirmation pass)

## Probes run (all read-only)

| Probe | Result |
|-------|--------|
| Cargo.toml [[bin]] entries | 3 binaries: cxg-daemon, cxg-index, cxg-resolve-cross-repo |
| target/release/ contents | All 3 binaries present (built 2026-05-17) |
| leveldb corpus | NOT PRESENT at ~/workspace/leveldb |
| cxg-daemon process | NOT RUNNING |
| cxg-daemon.toml | NOT PRESENT (any location) |
| TCP 192.168.1.200:7687 | OPEN (Bolt reachable) |
| kubectl current-context | admin@hs-cluster |
| neo4j-auth-external secret | Present in namespace infrastructure, key NEO4J_AUTH |
| Disk free | ~40 GiB |
| RAM (macOS vm_stat) | ~49 MiB free pages, ~1.8 GiB inactive |

## Decision

Re-index not executed: two blocking pre-conditions absent (no daemon running, no leveldb corpus).
QD-1 remains open. deploy-notes.md status: `blocked:QD-1-unresolved`.

## Deliverables written

- .claude/handoff/v2/runbook.md — 8-step re-index operator runbook
- .claude/handoff/v2/deploy-notes.md — environment probe results, gap documentation, test command
- .claude/handoff/v2/logs/devops.md — this file

## No cluster mutations

No `kubectl apply` was run. No Vault paths were changed. No GitLab CI was modified.
M8 is a LOCAL Rust CLI milestone with Bolt sink writes. DEPLOY_DRIFT not triggered.
