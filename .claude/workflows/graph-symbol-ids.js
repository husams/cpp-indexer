export const meta = {
  name: 'graph-symbol-ids',
  description: 'Dev-team pipeline: shrink the CodexGraph by storing compact integer IDs in the graph and keeping the USR/filename<->ID maps in SQLite behind a configurable LRU cache',
  whenToUse: 'Implement the "graph stores IDs, SQLite holds the symbol/file maps, LRU cache in front" feature for cpp-indexer. Author-and-run. DevOps + Doc-writer stages are skip-by-default (set skipDeploy:false / skipDocs:false to enable).',
  phases: [
    { title: 'PM',         detail: 'product-manager → requirements.md + acceptance criteria' },
    { title: 'BA',         detail: 'business-analyst → scenarios.md (Gherkin)' },
    { title: 'Architect',  detail: 'architect → design.md + adr-*.md (ID-namespace + SCHEMA_VERSION ADRs)' },
    { title: 'Sr Dev',     detail: 'senior-developer → plan.md (3–7 stories)' },
    { title: 'Developer',  detail: 'one developer agent per story, fanned out' },
    { title: 'QA',         detail: 'qa-engineer → test-report.md' },
    { title: 'DevOps',     detail: 'devops → deploy-notes.md + runbook.md (SIDE-EFFECTFUL — skip-by-default)' },
    { title: 'Doc-writer', detail: 'doc-writer → wiki page (SIDE-EFFECTFUL — skip-by-default)' },
  ],
}

// ---------------------------------------------------------------------------
// HOW TO RUN
//   1. mkdir -p <project>/.claude/handoff/vN/logs   (caller does this)
//   2. write <handoff>/CHARTER.md                   (caller does this)
//   3. Workflow({ scriptPath: '.claude/workflows/graph-symbol-ids.js', args: {
//        slug: 'graph-symbol-ids',
//        handoffDir: '/Users/husam/workspace/cpp-indexer/.claude/handoff/v4',
//        runId: 'graph-symbol-ids-4',     // caller computes — no Date.now() in scripts
//        lang: 'rust',
//        targetContext: 'admin@dev-cluster',
//        skipDeploy: true,                // default-safe
//        skipDocs:   true,                // default-safe
//      }})
//
// WHY a feature-specific script (vs the generic dev-team-pipeline.js scaffold):
//   the whole leverage of a dev-team run is the REQUIREMENTS text + CHARTER
//   constraints. This script bakes in requirements grounded in the real code
//   (USR strings on nodes/edges, both sinks, parallel TU visit, Phase 5
//   cross-repo USR matching) plus the two decisions the user already made:
//     • ID namespace: PER-REPO; cross-repo resolution stays in USR-space and
//       integer IDs are applied only at final write.
//     • sink scope: BOTH neo4j.rs and indradb.rs.
//   so the PM/architect cannot silently pick the wrong design.
// ---------------------------------------------------------------------------

const A = args || {}
const HD = A.handoffDir || '/Users/husam/workspace/cpp-indexer/.claude/handoff/v4'
const SLUG = A.slug || 'graph-symbol-ids'
const LANG = A.lang || 'rust'
const CHARTER = `@${HD}/CHARTER.md`

// The baked-in feature brief the product-manager turns into requirements.md.
// Every constraint here is grounded in the cpp-indexer source; the two
// user decisions (per-repo IDs / both sinks) are stated as NON-NEGOTIABLE.
const REQUIREMENTS = `
FEATURE: Shrink the CodexGraph by storing compact integer IDs in the graph and
keeping the symbol/filename maps in SQLite, fronted by a configurable LRU cache.

PROBLEM: The graph (both sinks) keys every node by a libclang USR string (\`usr\`)
and every edge by \`src_usr\`/\`dst_usr\` (see src/schema/nodes.rs, edges.rs,
arrow.rs). Filenames are stored as strings too. These long strings repeat on
every node/edge and dominate graph size.

SOLUTION: A per-repo SQLite database is the durable source of truth for two maps:
  - symbols(id INTEGER PRIMARY KEY, usr TEXT NOT NULL UNIQUE)
  - files(id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE)
The graph stores ONLY the integer IDs (symbol ids on nodes, src/dst symbol ids
on edges, file id where filenames were stored). A configurable-size LRU cache
sits in front of the SQLite lookups for both directions (usr->id and id->usr,
path->id and id->path).

NON-NEGOTIABLE DESIGN DECISIONS (already made — do NOT re-litigate, state them as
fixed and write the corresponding ADRs as ACCEPTED, not proposed):
  D1. ID NAMESPACE = PER-REPO. IDs are allocated per indexed repo. Phase 5
      cross-repo resolution (src/resolve/cross_repo.rs, EXTERNAL_REF) MUST
      continue to match references in USR-STRING space; integer IDs are applied
      only at the final per-repo write. Do NOT compare integer IDs across repos.
  D2. SINK SCOPE = BOTH. Apply the ID-only graph + SQLite map to the Neo4j sink
      (src/sink/neo4j.rs) AND the IndraDB sink (src/sink/indradb.rs).

HARD CONSTRAINTS (turn each into at least one acceptance criterion):
  C1. Read path too, not just write. "Graph stores only IDs" means every consumer
      that returns symbols/filenames must resolve ID->symbol from SQLite: the
      query/read layer, the daemon, and the cpp-mcp tool surface
      (get_definition / get_references / get_ast / query_graphdb, etc.). Scope
      and cover these, or explicitly list any deferred to a follow-up.
  C2. SCHEMA_VERSION bump. Per ADR-9 (src/schema/version.rs), changing node/edge
      representation MUST bump SCHEMA_VERSION in the same change.
  C3. ID stability + durability across re-index. SQLite is the source of truth;
      a re-index must reuse existing IDs for known USRs/paths so existing graph
      refs never break. get-or-insert semantics, not truncate-and-realloc.
  C4. Concurrency. TUs visit in parallel (src/pipeline/parallel.rs). The USR->ID
      and path->ID allocators and the shared LRU cache MUST be thread-safe.
      Reuse src/sink/lock.rs patterns where applicable.
  C5. LRU cache specifics: configurable maximum size via CLI flag + env var +
      config struct (src/config/mod.rs, src/config/env.rs); write-through so
      eviction is always safe (SQLite is authoritative); caches both directions.
      Sensible default size; size 0 / disabled path must still be correct.
  C6. Config surface: SQLite database path is configurable (CLI + env + config),
      with a sensible default location relative to the workspace/output dir.
  C7. Migration/compat: define behavior against an existing graph written with
      USR strings (either a reset/re-index requirement or a documented migration).
      Do not silently corrupt an existing graph.

OUT OF SCOPE (state explicitly): no gRPC work (that is M9/M10); no change to the
libclang visit semantics beyond emitting IDs; no new node/edge KINDS.

SUCCESS CRITERIA: measurable graph-size reduction on a representative repo (state
a target, e.g. node/edge byte-size or DB size delta); all existing tests green
(except the known-unrelated schema_drift failure); new tests for the SQLite map,
the LRU cache (hit/miss/eviction/size-0), per-repo ID stability across re-index,
and both-sink round-trip (write IDs -> read back -> resolve to original USR/path).
`.trim()

// ---- minimal-return schemas (the dev-team "≤4 lines, no prose" contracts) ----
const PM_SCHEMA = { type: 'object', additionalProperties: false,
  required: ['deliverable', 'status', 'storiesMissingAC'],
  properties: {
    deliverable: { type: 'string' }, status: { enum: ['clear', 'blocked'] },
    storiesMissingAC: { type: 'array', items: { type: 'string' } } } }
const BA_SCHEMA = { type: 'object', additionalProperties: false,
  required: ['deliverable', 'status', 'scenarioCount'],
  properties: {
    deliverable: { type: 'string' }, status: { enum: ['clear', 'blocked'] },
    scenarioCount: { type: 'integer' } } }
const ARCH_SCHEMA = { type: 'object', additionalProperties: false,
  required: ['deliverable', 'status', 'adrs'],
  properties: {
    deliverable: { type: 'string' }, status: { enum: ['clear', 'blocked'] },
    adrs: { type: 'array', items: { type: 'object', additionalProperties: false,
      required: ['file', 'topic', 'status'],
      properties: { file: { type: 'string' }, topic: { type: 'string' },
        status: { enum: ['accepted', 'proposed'] } } } } } }
const STORY = { type: 'object', additionalProperties: false,
  required: ['title', 'filesToTouch', 'exitCriteria', 'parallelSafe'],
  properties: {
    title: { type: 'string' },
    filesToTouch: { type: 'array', items: { type: 'string' } },
    exitCriteria: { type: 'array', items: { type: 'string' } },
    parallelSafe: { type: 'boolean' } } }
const PLAN_SCHEMA = { type: 'object', additionalProperties: false,
  required: ['deliverable', 'status', 'stories'],
  properties: {
    deliverable: { type: 'string' }, status: { enum: ['clear', 'blocked'] },
    stories: { type: 'array', items: STORY, minItems: 1 } } }
const DEV_SCHEMA = { type: 'object', additionalProperties: false,
  required: ['storySlug', 'status', 'unresolvedSignals'],
  properties: {
    storySlug: { type: 'string' }, status: { enum: ['clear', 'blocked'] },
    unresolvedSignals: { type: 'array', items: { type: 'string' } } } }
const QA_SCHEMA = { type: 'object', additionalProperties: false,
  required: ['deliverable', 'status', 'openDefects'],
  properties: {
    deliverable: { type: 'string' }, status: { enum: ['clear', 'blocked'] },
    openDefects: { type: 'array', items: { type: 'string' } } } }
const DEVOPS_SCHEMA = { type: 'object', additionalProperties: false,
  required: ['deliverable', 'status', 'targetContext'],
  properties: {
    deliverable: { type: 'string' }, status: { enum: ['clear', 'blocked'] },
    targetContext: { type: 'string' } } }
const DOC_SCHEMA = { type: 'object', additionalProperties: false,
  required: ['wikiPage', 'status'],
  properties: { wikiPage: { type: 'string' }, status: { enum: ['clear', 'blocked'] } } }

// A blocked gate stops the pipeline loudly (the skill's "surface the code and STOP").
function gate(code, offenders) {
  if (offenders.length) {
    log(`GATE FAIL ${code}: ${offenders.join(', ')} — stopping pipeline`)
    throw new Error(`${code}: ${offenders.join(', ')}`)
  }
  log(`gate ok: no ${code}`)
}
const langNote = `note: load the \`${LANG}-conventions\` skill before writing any code`
// libclang env (from AGENTS.md) — without these, cargo test SIGABRTs on macOS.
// Prepended to every exit-gate command so the gates actually run tests.
const LIBCLANG_ENV =
  'LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib ' +
  'DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib'

// === Role 1 — product-manager ============================================
phase('PM')
const pm = await agent(
  `task-slug: ${SLUG}\ncharter: ${CHARTER}\nrole: product-manager\n` +
  `requirements (raw, authoritative — preserve every NON-NEGOTIABLE decision and HARD CONSTRAINT):\n` +
  `<<<\n${REQUIREMENTS}\n>>>\n` +
  `deliver: ${HD}/requirements.md — scope, out-of-scope, and acceptance criteria per story. ` +
  `Decisions D1 (per-repo IDs / USR-space cross-repo) and D2 (both sinks) are FIXED inputs, not open questions. ` +
  `Every HARD CONSTRAINT C1–C7 must map to ≥1 acceptance criterion.`,
  { agentType: 'product-manager', schema: PM_SCHEMA, phase: 'PM', label: 'pm:requirements' },
)
gate('MISSING_ACCEPTANCE_CRITERIA', pm.storiesMissingAC)

// === Role 2 — business-analyst ===========================================
phase('BA')
const ba = await agent(
  `task-slug: ${SLUG}\ncharter: ${CHARTER}\nrole: business-analyst\nread: [${HD}/requirements.md]\n` +
  `deliver: ${HD}/scenarios.md (Gherkin). Cover boundaries + edge cases: LRU eviction, cache size 0, ` +
  `unknown USR, re-index ID stability, concurrent get-or-insert, both-sink round-trip, ` +
  `cross-repo EXTERNAL_REF still resolves in USR-space, existing USR-string graph migration/reset.`,
  { agentType: 'business-analyst', schema: BA_SCHEMA, phase: 'BA', label: 'ba:scenarios' },
)
log(`BA wrote ${ba.scenarioCount} scenarios`)

// === Role 3 — architect ==================================================
phase('Architect')
const arch = await agent(
  `task-slug: ${SLUG}\ncharter: ${CHARTER}\nrole: architect\n` +
  `read: [${HD}/requirements.md, ${HD}/scenarios.md]\n` +
  `deliver: ${HD}/design.md + one ${HD}/adr-<n>.md per cross-cutting decision. ` +
  `REQUIRED ADR TOPICS (each must be Status: accepted, NOT proposed):\n` +
  `  • ADR "id-namespace": record PER-REPO IDs with cross-repo resolution staying in USR-space (decision D1 — already made).\n` +
  `  • ADR "schema-version-bump": SCHEMA_VERSION bump per ADR-9 (constraint C2).\n` +
  `  • ADR "sqlite-map-and-lru": SQLite as durable source of truth + thread-safe allocator + write-through LRU (C3,C4,C5).\n` +
  `  • ADR "existing-graph-migration": reset-vs-migrate against a USR-string graph (C7).\n` +
  `${langNote}`,
  { agentType: 'architect', schema: ARCH_SCHEMA, phase: 'Architect', label: 'arch:design' },
)
gate('ADR_UNRESOLVED', arch.adrs.filter(a => a.status === 'proposed').map(a => a.file))

// === Role 4 — senior-developer ===========================================
phase('Sr Dev')
const plan = await agent(
  `task-slug: ${SLUG}\ncharter: ${CHARTER}\nrole: senior-developer\n` +
  `read: [${HD}/requirements.md, ${HD}/scenarios.md, ${HD}/design.md, ${HD}/adr-*.md]\n` +
  `deliver: ${HD}/plan.md — 3–7 independently-shippable stories. Suggested slices (split/merge as you see fit, ` +
  `keep them small): (a) SQLite store + schema + per-repo get-or-insert allocator; (b) thread-safe ` +
  `configurable write-through LRU cache (both directions); (c) config/CLI/env wiring for cache size + db path; ` +
  `(d) write path — emit integer IDs from both sinks (neo4j.rs + indradb.rs) + SCHEMA_VERSION bump; ` +
  `(e) read/resolve path — ID->symbol for query layer + daemon + cpp-mcp tools; ` +
  `(f) migration/reset behavior for an existing USR-string graph. ` +
  `Each story needs: title, files-to-touch (explicit paths under src/), ` +
  `exit-criteria (cargo fmt --check && cargo clippy -- -D warnings && cargo test <scope>), parallel-safe.\n` +
  `${langNote}`,
  { agentType: 'senior-developer', schema: PLAN_SCHEMA, phase: 'Sr Dev', label: 'srdev:plan' },
)
gate('MISSING_EXIT_CRITERIA', plan.stories.filter(s => !s.exitCriteria.length).map(s => s.title))

// === Role 5 — developer (SEQUENTIAL, shared main tree) ===================
// These stories are a DEPENDENCY CHAIN: read/resolve (Story 4) needs the real
// SymbolAllocator from Story 1; write path (Story 3) needs the schema/store;
// verification (Story 6) needs everything. They also share files (arrow.rs,
// resolve/mod.rs, Cargo.toml) and a "Cross-story API contract" in plan.md.
// Therefore: NO worktree isolation (would hide each story from the next) and
// NO parallelism (N+1 needs N). Run strictly in plan order in the main tree so
// each developer builds on the previous one's committed-to-disk code.
phase('Developer')
const slug = (t) => t.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '')
const devPrompt = (s) =>
  `task-slug: ${SLUG}\ncharter: ${CHARTER}\nstory: "${s.title}"\n` +
  `read: [${HD}/plan.md (this story + the "Cross-story API contract" section), ${HD}/design.md, ${HD}/scenarios.md]\n` +
  `files-to-touch: ${s.filesToTouch.join(', ')}\n` +
  `IMPORTANT: earlier stories already landed in this working tree — REUSE their real types ` +
  `(e.g. resolve/symbol_map.rs SymbolAllocator); do NOT re-create modules that already exist.\n` +
  `exit-gate (run internally, max 3 passes): ${LIBCLANG_ENV} sh -c '${s.exitCriteria.join(' && ')}'\n` +
  `log: ${HD}/logs/developer-${slug(s.title)}.md\n${langNote}`
const devOpts = (s) => ({ agentType: 'developer', schema: DEV_SCHEMA, phase: 'Developer',
  label: `dev:${slug(s.title)}` })

const devResults = []
for (const s of plan.stories) devResults.push(await agent(devPrompt(s), devOpts(s)))

const devBlocked = devResults.filter(r => r && r.status === 'blocked')
if (devBlocked.length) {
  gate('DEV_SIGNALS_UNRESOLVED',
    devBlocked.flatMap(r => r.unresolvedSignals.map(sig => `${r.storySlug}:${sig}`)))
}
log(`developer: ${devResults.length} stories, all exit gates clear`)

// === Role 6 — qa-engineer (consolidated) =================================
phase('QA')
const qa = await agent(
  `task-slug: ${SLUG}\ncharter: ${CHARTER}\nrole: qa-engineer\n` +
  `story-slugs: ${devResults.map(r => r.storySlug).join(', ')}\n` +
  `read: ${HD}/plan.md + ${HD}/logs/developer-*.md\n` +
  `IMPORTANT: prefix every cargo invocation with \`${LIBCLANG_ENV}\` or cargo test SIGABRTs (libclang).\n` +
  `deliver: ${HD}/test-report.md. Must include tests for: SQLite get-or-insert idempotency, ` +
  `LRU hit/miss/eviction + size-0 path, per-repo ID stability across a simulated re-index, ` +
  `both-sink ID round-trip (write ID -> read -> resolve to original USR/path), and cross-repo ` +
  `EXTERNAL_REF still resolving in USR-space. Add ≥1 BDD/property/mutation test or justify in log.`,
  { agentType: 'qa-engineer', schema: QA_SCHEMA, phase: 'QA', label: 'qa:test-report' },
)
gate('QA_DEFECT', qa.openDefects)

// === Role 7 — devops (SIDE-EFFECTFUL — deploys to a real cluster) ========
phase('DevOps')
let devops = null
if (A.skipDeploy === false) {
  devops = await agent(
    `task-slug: ${SLUG}\ncharter: ${CHARTER}\nrole: devops\n` +
    `read: [${HD}/design.md, ${HD}/implementation-notes.md, ${HD}/plan.md]\n` +
    `target-context: ${A.targetContext || '<REQUIRED>'}\n` +
    `deliver: ${HD}/deploy-notes.md + ${HD}/runbook.md\n` +
    `MUST verify \`kubectl config current-context\` before applying anything; deploy only to the ` +
    `target-context above; do NOT touch hs-cluster. Note: SCHEMA_VERSION bumped — runbook MUST cover ` +
    `the re-index / reset step for any existing USR-string graph.`,
    { agentType: 'devops', schema: DEVOPS_SCHEMA, phase: 'DevOps', label: 'devops:deploy' },
  )
} else {
  log('DevOps SKIPPED (skipDeploy default-true). Set skipDeploy:false to deploy. ' +
      'CLUSTER GATE: confirm kubectl context first — never hs-cluster without YES.')
}

// === Role 8 — doc-writer (SIDE-EFFECTFUL — writes wiki + cognee) =========
phase('Doc-writer')
let docs = null
if (A.skipDocs === false) {
  docs = await agent(
    `task-slug: ${SLUG}\ncharter: ${CHARTER}\nrole: doc-writer\n` +
    `read: all ${HD}/*.md deliverables\n` +
    `deliver: ~/workspace/wiki/pages/code/${SLUG}.md + ${HD}/docs-changes.md; update wiki index.md + ` +
    `append log.md; cross-link [[pages/code/cpp-indexer]]; run cognee ingest in closing protocol`,
    { agentType: 'doc-writer', schema: DOC_SCHEMA, phase: 'Doc-writer', label: 'doc:wiki' },
  )
} else {
  log('Doc-writer SKIPPED (skipDocs default-true). Set skipDocs:false to write ' +
      `the wiki page (~/workspace/wiki/pages/code/${SLUG}.md) and run cognee ingest.`)
}

// Final punch list — structured facts, not prose.
return {
  runId: A.runId || `${SLUG}-?`,
  handoffDir: HD,
  decisions: { idNamespace: 'per-repo (cross-repo resolves in USR-space)', sinks: 'neo4j + indradb' },
  gates: { I1: 'MISSING_ACCEPTANCE_CRITERIA ok', I2: 'ADR_UNRESOLVED ok',
           I3: 'MISSING_EXIT_CRITERIA ok', I4: 'QA_DEFECT ok' },
  deliverables: {
    requirements: pm.deliverable, scenarios: ba.deliverable, design: arch.deliverable,
    plan: plan.deliverable, testReport: qa.deliverable,
    deploy: devops ? devops.deliverable : 'skipped',
    wiki: docs ? docs.wikiPage : 'skipped',
  },
  stories: devResults.map(r => ({ slug: r.storySlug, status: r.status })),
}
