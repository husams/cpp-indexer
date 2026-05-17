# Product Manager Session Log — cpp-indexer v1

date: 2026-05-17
task-slug: cpp-indexer
run_id: cpp-indexer-v1
agent: product-manager

## Inputs read

1. `/Users/husam/workspace/cpp-indexer/.claude/handoff/v1/CHARTER.md` — locked-in decisions, cross-stage invariants, failure codes, open questions list
2. `/Users/husam/workspace/cpp-indexer/.claude/handoff/v1/requirements-raw.md` — milestone scope, locked decisions summary, open questions
3. `/Users/husam/workspace/wiki/pages/planning/codexgraph-cpp-prd-v1.md` — PRD v1.1 (21 user stories, FR-S*/FR-P*/FR-C*/FR-B*/FR-CLI*/FR-AD*/FR-API*/FR-A*, NFRs §7, milestones §9, dependencies §10)
4. `/Users/husam/workspace/wiki/pages/planning/codexgraph-cpp-libclang-rust.md` — engineering plan v1.1 (5-phase architecture, schema tables, module layout, Cargo.toml, milestones M1–M7)

## Skills loaded

- None (advisor called directly; no cognee-memory query needed as memory.md had current status)

## Advisor consultation

Called advisor after orientation (reading all 4 source files) but before writing. Key guidance received:
- Map PRD US/FR IDs to milestones; do not invent new stories
- Use per-milestone AC counter: `AC-M<N>-<seq>`
- NFRs from PRD §7 belong as ACs on M3 (perf) and M7 (API latency) stories
- Q5 is a hard blocker on all M6 stories; surface it on every M6 story
- Q3, Q2, Q4, Q7, Q8 — call out where they affect scope; Q6 accepted as deferred
- Priority: P0 for milestone-exit-critical, P1 for in-milestone required, P2 for nice-to-have
- Closing protocol is mandatory

## Decisions made

1. **Story count**: 7 stories for M1, 4 for M2, 5 for M3, 4 for M4, 4 for M5, 4 for M6, 7 for M7 = 35 stories total, 27 P0, 8 P1, 0 P2 (no P2s identified; FR-A2 example.txt was marked P1 not P2 given agent integration dependency)
2. **AC ID scheme**: AC-M<milestone>-<seq> with global counter per milestone (not per story). Allows traceability chain from plan.md story → specific AC IDs.
3. **Priority rule**: stated once at top of requirements.md; P0 = demo gate blocker, P1 = in-milestone required, P2 = nice-to-have.
4. **Public naming**: all story text uses `cxg-index`, `cxg-daemon`, `cxg-resolve-cross-repo` — no "codexgraph" in user-facing strings.
5. **Q5 handling**: flagged as hard blocker on all M6 stories with note that architect MUST resolve in adr-1.md before M6 developer dispatch.
6. **Q8 (Bazel)**: PM decision recorded — explicitly out of scope for v1; document in README.
7. **IndraDB proviso**: added open question note on M1-S6 that IndraDB gRPC endpoint must be provisioned by devops before M1 integration tests can run.

## Problems hit

None. All source material was available and consistent. Minor reconciliation: engineering plan §M7 referred to "cxg-watch" daemon mode, but PRD v1.1 §9 and §6.7 use "cxg-daemon" — used PRD v1.1 as authoritative (v1.1 is the later revision).

## Output

`/Users/husam/workspace/cpp-indexer/.claude/handoff/v1/requirements.md` — 35 stories, 27 P0, 8 P1, 27 AC IDs (AC-M1-1 through AC-M7-27), open questions table, cross-milestone dependency table.

## Invariant check (I1)

Every story in requirements.md has at least one explicit AC entry. Verified before closing.
