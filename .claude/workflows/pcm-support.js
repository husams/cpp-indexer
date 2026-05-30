export const meta = {
  name: 'pcm-support',
  description: 'Dev-team pipeline to implement PCM (Clang precompiled-module / -fmodules / .pcm) support in the cpp-indexer parse path',
  whenToUse: 'Author-and-run instance of the 8-role dev-team pipeline, scoped to the PCM-support requirement added to the compact-ingest spec on 2026-05-30. DevOps + Doc-writer stages are skip-by-default (set skipDeploy:false / skipDocs:false to enable the side-effectful stages).',
  phases: [
    { title: 'PM',         detail: 'product-manager → requirements.md + acceptance criteria for PCM support' },
    { title: 'BA',         detail: 'business-analyst → scenarios.md (Gherkin: -fmodules / -fmodule-file= / -fprebuilt-module-path TUs)' },
    { title: 'Architect',  detail: 'architect → design.md + adr-*.md (dispatch wiring, probe extension, no schema bump)' },
    { title: 'Sr Dev',     detail: 'senior-developer → plan.md (3–7 stories)' },
    { title: 'Developer',  detail: 'one developer agent per story, fanned out' },
    { title: 'QA',         detail: 'qa-engineer → test-report.md (mixed-flag TU fixtures; silent-partial-parse guard)' },
    { title: 'DevOps',     detail: 'devops → deploy-notes.md (SIDE-EFFECTFUL — skip by default)' },
    { title: 'Doc-writer', detail: 'doc-writer → wiki page (SIDE-EFFECTFUL — skip by default)' },
  ],
}

// ---------------------------------------------------------------------------
// HOW TO RUN
//   1. Pick a handoff dir and create it + a CHARTER.md first (scripts have no fs):
//        mkdir -p .claude/handoff/pcm/logs
//        echo "PCM support for cpp-indexer parse path" > .claude/handoff/pcm/CHARTER.md
//   2. Workflow({ scriptPath: '.claude/workflows/pcm-support.js', args: {
//        slug: 'pcm-support',
//        handoffDir: '/Users/husam/workspace/cpp-indexer/.claude/handoff/pcm',
//        runId: 'pcm-support-1',                              // caller computes (no Date.now() in scripts)
//        targetContext: 'admin@dev-cluster',                 // only needed if skipDeploy:false
//        skipDeploy: true,   // default-safe
//        skipDocs:   true,   // default-safe
//      }})
//
// This is a thin, grounded instance of dev-team-pipeline.js: same gate model and
// handoff-by-file-path convention, but the PM/Architect prompts are pre-loaded
// with the VERIFIED codebase facts so the pipeline doesn't re-derive them.
// ---------------------------------------------------------------------------

const A = args || {}
const HD = A.handoffDir || '/Users/husam/workspace/cpp-indexer/.claude/handoff/pcm'
const SLUG = A.slug || 'pcm-support'
const CHARTER = `@${HD}/CHARTER.md`
const SPEC = '~/workspace/wiki/pages/planning/cpp-indexer-compact-ingest-path.md'

// Verified findings — injected into the early prompts so agents build on fact, not guesswork.
const FACTS = [
  'Feasible, ~2–3 days, NO schema changes (MODULE node + module_interface attr already exist).',
  'C++20 module parser already exists at src/visit/modules_cpp20.rs: parse_module_tu() :203, is_module_tu() :168, probe_cpp20_support() :66 — NOT yet wired into the main pipeline.',
  'Compiler-arg passthrough already preserves -fmodules / -fmodule-file= / -fprebuilt-module-path via sanitize_libclang_args() (src/bootstrap/compile_commands.rs:63) and filter_compiler_args() (src/pipeline/parallel.rs:160).',
  'Core work: add dispatch at src/pipeline/parallel.rs:176 to route module/PCM TUs to parse_module_tu(); extend is_module_tu()/probe to detect PCM flags; tests + docs.',
  'Requires libclang 18+; PCM support varies by distro — use the same best-effort approach as the C++20 module path.',
  'RISK: missing/invalid .pcm files cause silent partial TU parses — same failure family as open Issue 0001. Acceptance MUST require a loud diagnostic, not exit-0.',
  'PCM is CPU-bound, not RAM-bound — it does NOT advance the ingest-RAM goals of the parent spec; treat it as an orthogonal parse-capability requirement.',
].map((f, i) => `  ${i + 1}. ${f}`).join('\n')

// === Minimal-return contracts (dev-team "≤4 lines, no prose" rule, as schemas) ===
const PM_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['deliverable', 'status', 'storiesMissingAC'],
  properties: {
    deliverable: { type: 'string', description: 'path to requirements.md' },
    status: { enum: ['clear', 'blocked'] },
    storiesMissingAC: { type: 'array', items: { type: 'string' } },
  },
}
const BA_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['deliverable', 'status', 'scenarioCount'],
  properties: {
    deliverable: { type: 'string' }, status: { enum: ['clear', 'blocked'] },
    scenarioCount: { type: 'integer' },
  },
}
const ARCH_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['deliverable', 'status', 'adrs', 'schemaBump'],
  properties: {
    deliverable: { type: 'string' }, status: { enum: ['clear', 'blocked'] },
    adrs: { type: 'array', items: {
      type: 'object', additionalProperties: false, required: ['file', 'status'],
      properties: { file: { type: 'string' }, status: { enum: ['accepted', 'proposed'] } } } },
    schemaBump: { type: 'boolean', description: 'must be false — PCM needs no schema change (gate I2b)' },
  },
}
const STORY = {
  type: 'object', additionalProperties: false,
  required: ['title', 'filesToTouch', 'exitCriteria', 'parallelSafe'],
  properties: {
    title: { type: 'string' },
    filesToTouch: { type: 'array', items: { type: 'string' } },
    exitCriteria: { type: 'array', items: { type: 'string' } },
    parallelSafe: { type: 'boolean' },
  },
}
const PLAN_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['deliverable', 'status', 'stories'],
  properties: {
    deliverable: { type: 'string' }, status: { enum: ['clear', 'blocked'] },
    stories: { type: 'array', items: STORY, minItems: 1 },
  },
}
const DEV_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['storySlug', 'status', 'unresolvedSignals'],
  properties: {
    storySlug: { type: 'string' }, status: { enum: ['clear', 'blocked'] },
    unresolvedSignals: { type: 'array', items: { type: 'string' } },
  },
}
const QA_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['deliverable', 'status', 'openDefects'],
  properties: {
    deliverable: { type: 'string' }, status: { enum: ['clear', 'blocked'] },
    openDefects: { type: 'array', items: { type: 'string' } },
  },
}
const DEVOPS_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['deliverable', 'status', 'targetContext'],
  properties: {
    deliverable: { type: 'string' }, status: { enum: ['clear', 'blocked'] },
    targetContext: { type: 'string' },
  },
}
const DOC_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['wikiPage', 'status'],
  properties: { wikiPage: { type: 'string' }, status: { enum: ['clear', 'blocked'] } },
}

function gate(code, offenders) {
  if (offenders.length) {
    log(`GATE FAIL ${code}: ${offenders.join(', ')} — stopping pipeline`)
    throw new Error(`${code}: ${offenders.join(', ')}`)
  }
  log(`gate ok: no ${code}`)
}

// === Role 1 — product-manager ============================================
phase('PM')
const pm = await agent(
  `task-slug: ${SLUG}\ncharter: ${CHARTER}\nrole: product-manager\n` +
  `feature: PCM (Clang precompiled-module) support in the cpp-indexer parse path.\n` +
  `source spec (read the PCM section): ${SPEC}\n` +
  `verified facts to build on:\n${FACTS}\n` +
  `deliver: ${HD}/requirements.md (scope, out-of-scope, acceptance criteria per story). ` +
  `One acceptance criterion MUST be: a missing/invalid .pcm produces a LOUD diagnostic and non-zero/error signal, never a silent exit-0 partial parse (Issue 0001 family).`,
  { agentType: 'product-manager', schema: PM_SCHEMA, phase: 'PM', label: 'pm:requirements' },
)
gate('MISSING_ACCEPTANCE_CRITERIA', pm.storiesMissingAC)

// === Role 2 — business-analyst ===========================================
phase('BA')
const ba = await agent(
  `task-slug: ${SLUG}\ncharter: ${CHARTER}\nrole: business-analyst\nread: [${HD}/requirements.md]\n` +
  `deliver: ${HD}/scenarios.md (Gherkin). Cover: TU with -fmodules only; TU with -fmodule-file=name=path; ` +
  `TU with -fprebuilt-module-path=dir; missing .pcm on disk; libclang < 18 (capability probe fails gracefully); ` +
  `mixed module + non-module TUs in one compile_commands.json.`,
  { agentType: 'business-analyst', schema: BA_SCHEMA, phase: 'BA', label: 'ba:scenarios' },
)
log(`BA wrote ${ba.scenarioCount} scenarios`)

// === Role 3 — architect ==================================================
phase('Architect')
const arch = await agent(
  `task-slug: ${SLUG}\ncharter: ${CHARTER}\nrole: architect\nread: [${HD}/requirements.md, ${HD}/scenarios.md]\n` +
  `verified facts:\n${FACTS}\n` +
  `deliver: ${HD}/design.md + ${HD}/adr-<n>.md. Decisions to settle: (a) dispatch point at ` +
  `src/pipeline/parallel.rs:176 routing module/PCM TUs to parse_module_tu(); (b) how is_module_tu()/probe ` +
  `detect PCM flags; (c) the loud-diagnostic strategy for missing/invalid .pcm. Reuse existing MODULE node — ` +
  `confirm NO schema bump (return schemaBump:false). Mark each ADR accepted|proposed.\n` +
  `note: load the rust-conventions skill before writing the design.`,
  { agentType: 'architect', schema: ARCH_SCHEMA, phase: 'Architect', label: 'arch:design' },
)
gate('ADR_UNRESOLVED', arch.adrs.filter(a => a.status === 'proposed').map(a => a.file))
gate('UNEXPECTED_SCHEMA_BUMP', arch.schemaBump ? ['design proposes a schema change; PCM should need none'] : [])

// === Role 4 — senior-developer ===========================================
phase('Sr Dev')
const plan = await agent(
  `task-slug: ${SLUG}\ncharter: ${CHARTER}\nrole: senior-developer\n` +
  `read: [${HD}/requirements.md, ${HD}/scenarios.md, ${HD}/design.md, ${HD}/adr-*.md]\n` +
  `deliver: ${HD}/plan.md — 3–7 independently-shippable stories; each with title, files-to-touch, ` +
  `exit-criteria (cargo fmt/clippy/test cmds with the project's libclang env baked in), parallel-safe flag.`,
  { agentType: 'senior-developer', schema: PLAN_SCHEMA, phase: 'Sr Dev', label: 'srdev:plan' },
)
gate('MISSING_EXIT_CRITERIA', plan.stories.filter(s => !s.exitCriteria.length).map(s => s.title))

// === Role 5 — developer (per-story fan-out) ==============================
phase('Developer')
const slug = (t) => t.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '')
const devPrompt = (s) =>
  `task-slug: ${SLUG}\ncharter: ${CHARTER}\nstory: "${s.title}"\n` +
  `read: [${HD}/plan.md (this story only), ${HD}/design.md, ${HD}/scenarios.md]\n` +
  `files-to-touch: ${s.filesToTouch.join(', ')}\n` +
  `exit-gate (run internally, max 3 passes): ${s.exitCriteria.join(' && ')}\n` +
  `log: ${HD}/logs/developer-${slug(s.title)}.md\n` +
  `note: load rust-conventions before writing any file; bake the libclang env into every cargo invocation.`
const devOpts = (s) => ({ agentType: 'developer', schema: DEV_SCHEMA, phase: 'Developer', label: `dev:${slug(s.title)}` })

const safe = plan.stories.filter(s => s.parallelSafe)
const seq  = plan.stories.filter(s => !s.parallelSafe)
const devResults = []
devResults.push(...(await parallel(safe.map(s => () => agent(devPrompt(s), devOpts(s))))).filter(Boolean))
for (const s of seq) devResults.push(await agent(devPrompt(s), devOpts(s)))

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
  `deliver: ${HD}/test-report.md. MUST include a fixture proving a missing/invalid .pcm yields a loud ` +
  `diagnostic (assert it is NOT a silent exit-0), plus a mixed module+non-module compile_commands.json case.`,
  { agentType: 'qa-engineer', schema: QA_SCHEMA, phase: 'QA', label: 'qa:test-report' },
)
gate('QA_DEFECT', qa.openDefects)

// === Role 7 — devops (SIDE-EFFECTFUL — skip by default) ==================
phase('DevOps')
let devops = null
if (A.skipDeploy !== false) {
  log('DevOps SKIPPED (skipDeploy default-true). Set skipDeploy:false to enable.')
} else {
  devops = await agent(
    `task-slug: ${SLUG}\ncharter: ${CHARTER}\nrole: devops\n` +
    `read: [${HD}/design.md, ${HD}/plan.md]\ntarget-context: ${A.targetContext || '<REQUIRED>'}\n` +
    `deliver: ${HD}/deploy-notes.md + ${HD}/runbook.md. Verify \`kubectl config current-context\` first; ` +
    `deploy only to target-context; do NOT touch hs-cluster.`,
    { agentType: 'devops', schema: DEVOPS_SCHEMA, phase: 'DevOps', label: 'devops:deploy' },
  )
}

// === Role 8 — doc-writer (SIDE-EFFECTFUL — skip by default) ===============
phase('Doc-writer')
let docs = null
if (A.skipDocs !== false) {
  log('Doc-writer SKIPPED (skipDocs default-true). Set skipDocs:false to write the wiki page + cognee ingest.')
} else {
  docs = await agent(
    `task-slug: ${SLUG}\ncharter: ${CHARTER}\nrole: doc-writer\nread: all ${HD}/*.md deliverables\n` +
    `deliver: ~/workspace/wiki/pages/code/${SLUG}.md + ${HD}/docs-changes.md; cross-link the PCM section of ` +
    `${SPEC}; update wiki index.md + append log.md; run cognee ingest in the closing protocol.`,
    { agentType: 'doc-writer', schema: DOC_SCHEMA, phase: 'Doc-writer', label: 'doc:wiki' },
  )
}

return {
  runId: A.runId || `${SLUG}-?`,
  handoffDir: HD,
  gates: { I1: 'MISSING_ACCEPTANCE_CRITERIA ok', I2: 'ADR_UNRESOLVED ok',
           I2b: 'UNEXPECTED_SCHEMA_BUMP ok', I3: 'MISSING_EXIT_CRITERIA ok', I4: 'QA_DEFECT ok' },
  deliverables: {
    requirements: pm.deliverable, scenarios: ba.deliverable, design: arch.deliverable,
    plan: plan.deliverable, testReport: qa.deliverable,
    deploy: devops ? devops.deliverable : 'skipped',
    wiki: docs ? docs.wikiPage : 'skipped',
  },
  stories: devResults.map(r => ({ slug: r.storySlug, status: r.status })),
}
