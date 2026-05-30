export const meta = {
  name: 'dev-team-pipeline',
  description: 'Scaffold: the 8-role dev-team pipeline (PM → BA → Architect → Sr Dev → Developer → QA → DevOps → Doc-writer) as a Workflow orchestration',
  whenToUse: 'Demo/scaffold showing how to translate the sequential dev-team skill into a Workflow script. Run for real only after reviewing the side-effectful stages (DevOps deploys; Doc-writer writes the wiki + cognee).',
  phases: [
    { title: 'PM',         detail: 'product-manager → requirements.md + acceptance criteria' },
    { title: 'BA',         detail: 'business-analyst → scenarios.md (Gherkin)' },
    { title: 'Architect',  detail: 'architect → design.md + adr-*.md' },
    { title: 'Sr Dev',     detail: 'senior-developer → plan.md (3–7 stories)' },
    { title: 'Developer',  detail: 'one developer agent per story, fanned out' },
    { title: 'QA',         detail: 'qa-engineer → test-report.md' },
    { title: 'DevOps',     detail: 'devops → deploy-notes.md + runbook.md (SIDE-EFFECTFUL: deploys)' },
    { title: 'Doc-writer', detail: 'doc-writer → wiki page (SIDE-EFFECTFUL: writes wiki + cognee)' },
  ],
}

// ---------------------------------------------------------------------------
// HOW TO RUN
//   Workflow({ scriptPath: '.claude/workflows/dev-team-pipeline.js', args: {
//     slug: 'my-feature',
//     handoffDir: '/abs/path/to/project/.claude/handoff/v1',  // caller MUST mkdir -p this first
//     runId: 'my-feature-1',                                  // caller computes (no Date.now() in scripts)
//     requirements: 'free-text requirements, or a path the PM agent can read',
//     targetContext: 'admin@dev-cluster',                     // kubectl context for devops
//     skipDeploy: true,    // default-safe: skip the cluster-touching stage
//     skipDocs:   true,    // default-safe: skip the wiki/cognee-writing stage
//   }})
//
// WHY a script instead of turn-by-turn dispatch (the /dev-team skill):
//   • The plan lives in code, not Claude's context — intermediate results are
//     script variables, not replayed tokens.
//   • The developer stage fans out one agent PER STORY in parallel (the skill
//     does this sequentially to save the coordinator's context).
//   • Invariant gates I1–I4 are plain JS asserts on each role's STRUCTURED
//     return — the script has no filesystem access, so gates must read facts
//     out of the agent's return value, NOT out of requirements.md on disk.
//
// HANDOFF MODEL: every cross-role payload is a FILE PATH. Agents do the file
//   I/O (write requirements.md, read plan.md, …); the script only passes paths
//   in prompts. The caller creates handoffDir + CHARTER.md before invoking.
// ---------------------------------------------------------------------------

const A = args || {}
const HD = A.handoffDir || '<handoff-dir>'
const SLUG = A.slug || 'demo'
const CHARTER = `@${HD}/CHARTER.md`

// Minimal-return contracts (the dev-team "≤4 lines, no prose" rule, as schemas).
const PM_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['deliverable', 'status', 'storiesMissingAC'],
  properties: {
    deliverable: { type: 'string', description: 'path to requirements.md' },
    status: { enum: ['clear', 'blocked'] },
    storiesMissingAC: { type: 'array', items: { type: 'string' },
      description: 'story titles with no acceptance criteria (drives gate I1)' },
  },
}
const BA_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['deliverable', 'status', 'scenarioCount'],
  properties: {
    deliverable: { type: 'string', description: 'path to scenarios.md' },
    status: { enum: ['clear', 'blocked'] },
    scenarioCount: { type: 'integer' },
  },
}
const ARCH_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['deliverable', 'status', 'adrs'],
  properties: {
    deliverable: { type: 'string', description: 'path to design.md' },
    status: { enum: ['clear', 'blocked'] },
    adrs: { type: 'array', items: {
      type: 'object', additionalProperties: false,
      required: ['file', 'status'],
      properties: { file: { type: 'string' },
        status: { enum: ['accepted', 'proposed'] } }, // 'proposed' trips gate I2
    } },
  },
}
const STORY = {
  type: 'object', additionalProperties: false,
  required: ['title', 'filesToTouch', 'exitCriteria', 'parallelSafe'],
  properties: {
    title: { type: 'string' },
    filesToTouch: { type: 'array', items: { type: 'string' } },
    exitCriteria: { type: 'array', items: { type: 'string' },
      description: 'formatter/linter/test commands; empty array trips gate I3' },
    parallelSafe: { type: 'boolean' },
  },
}
const PLAN_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['deliverable', 'status', 'stories'],
  properties: {
    deliverable: { type: 'string', description: 'path to plan.md' },
    status: { enum: ['clear', 'blocked'] },
    stories: { type: 'array', items: STORY, minItems: 1 },
  },
}
const DEV_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['storySlug', 'status', 'unresolvedSignals'],
  properties: {
    storySlug: { type: 'string' },
    status: { enum: ['clear', 'blocked'] },
    unresolvedSignals: { type: 'array', items: { type: 'string' },
      description: 'LINT_FAIL/TEST_FAIL/BUILD_FAIL ids still open after 3 passes' },
  },
}
const QA_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['deliverable', 'status', 'openDefects'],
  properties: {
    deliverable: { type: 'string', description: 'path to test-report.md' },
    status: { enum: ['clear', 'blocked'] },
    openDefects: { type: 'array', items: { type: 'string' },
      description: 'QA_DEFECT ids with status open (drives gate I4)' },
  },
}
const DEVOPS_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['deliverable', 'status', 'targetContext'],
  properties: {
    deliverable: { type: 'string' },
    status: { enum: ['clear', 'blocked'] },
    targetContext: { type: 'string' },
  },
}
const DOC_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['wikiPage', 'status'],
  properties: { wikiPage: { type: 'string' }, status: { enum: ['clear', 'blocked'] } },
}

// A blocked gate stops the pipeline loudly — same contract as the skill's
// "surface the failure code and STOP" rule.
function gate(code, offenders) {
  if (offenders.length) {
    log(`GATE FAIL ${code}: ${offenders.join(', ')} — stopping pipeline`)
    throw new Error(`${code}: ${offenders.join(', ')}`)
  }
  log(`gate ok: no ${code}`)
}

const slice = (file) => `read: [${HD}/${file}]`

// === Role 1 — product-manager ============================================
phase('PM')
const pm = await agent(
  `task-slug: ${SLUG}\ncharter: ${CHARTER}\nrole: product-manager\n` +
  `requirements (raw): ${A.requirements || `${HD}/requirements-raw.md`}\n` +
  `deliver: ${HD}/requirements.md (scope, out-of-scope, acceptance criteria per story)`,
  { agentType: 'product-manager', schema: PM_SCHEMA, phase: 'PM', label: 'pm:requirements' },
)
// Gate I1 — every story must carry acceptance criteria before architecture.
gate('MISSING_ACCEPTANCE_CRITERIA', pm.storiesMissingAC)

// === Role 2 — business-analyst ===========================================
phase('BA')
const ba = await agent(
  `task-slug: ${SLUG}\ncharter: ${CHARTER}\nrole: business-analyst\n${slice('requirements.md')}\n` +
  `deliver: ${HD}/scenarios.md (Gherkin; cover boundaries + edge cases)`,
  { agentType: 'business-analyst', schema: BA_SCHEMA, phase: 'BA', label: 'ba:scenarios' },
)
log(`BA wrote ${ba.scenarioCount} scenarios`)

// === Role 3 — architect ==================================================
phase('Architect')
const arch = await agent(
  `task-slug: ${SLUG}\ncharter: ${CHARTER}\nrole: architect\n` +
  `read: [${HD}/requirements.md, ${HD}/scenarios.md]\n` +
  `deliver: ${HD}/design.md + ${HD}/adr-<n>.md (one ADR per cross-cutting decision; mark each accepted|proposed)\n` +
  `note: load the project's <lang>-conventions skill before writing the design`,
  { agentType: 'architect', schema: ARCH_SCHEMA, phase: 'Architect', label: 'arch:design' },
)
// Gate I2 — no ADR may still be "proposed" when developers start.
gate('ADR_UNRESOLVED', arch.adrs.filter(a => a.status === 'proposed').map(a => a.file))

// === Role 4 — senior-developer ===========================================
phase('Sr Dev')
const plan = await agent(
  `task-slug: ${SLUG}\ncharter: ${CHARTER}\nrole: senior-developer\n` +
  `read: [${HD}/requirements.md, ${HD}/scenarios.md, ${HD}/design.md, ${HD}/adr-*.md]\n` +
  `deliver: ${HD}/plan.md — 3–7 independently-shippable stories; each story has ` +
  `title, files-to-touch, exit-criteria (formatter/linter/test cmds), parallel-safe`,
  { agentType: 'senior-developer', schema: PLAN_SCHEMA, phase: 'Sr Dev', label: 'srdev:plan' },
)
// Gate I3 — every story must name its exit-criteria commands.
gate('MISSING_EXIT_CRITERIA', plan.stories.filter(s => !s.exitCriteria.length).map(s => s.title))

// === Role 5 — developer (per-story fan-out) ==============================
// parallel-safe stories fan out concurrently; the rest run sequentially so
// they don't race on shared files. (The /dev-team skill runs ALL of these
// sequentially to protect the coordinator's context — the script doesn't pay
// that cost, so it can parallelise the safe ones.)
phase('Developer')
const slug = (t) => t.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '')
const devPrompt = (s) =>
  `task-slug: ${SLUG}\ncharter: ${CHARTER}\nstory: "${s.title}"\n` +
  `read: [${HD}/plan.md (this story only), ${HD}/design.md, ${HD}/scenarios.md]\n` +
  `files-to-touch: ${s.filesToTouch.join(', ')}\n` +
  `exit-gate (run internally, max 3 passes): ${s.exitCriteria.join(' && ')}\n` +
  `log: ${HD}/logs/developer-${slug(s.title)}.md\n` +
  `note: load <lang>-conventions before writing any file`
const devOpts = (s) => ({ agentType: 'developer', schema: DEV_SCHEMA, phase: 'Developer',
  label: `dev:${slug(s.title)}` })

const safe = plan.stories.filter(s => s.parallelSafe)
const seq  = plan.stories.filter(s => !s.parallelSafe)

const devResults = []
// fan out the parallel-safe stories
devResults.push(...(await parallel(safe.map(s => () => agent(devPrompt(s), devOpts(s))))).filter(Boolean))
// then the order-sensitive ones, one at a time
for (const s of seq) devResults.push(await agent(devPrompt(s), devOpts(s)))

const devBlocked = devResults.filter(r => r && r.status === 'blocked')
if (devBlocked.length) {
  // The skill stops for user guidance here; the scaffold surfaces the signals
  // and stops the pipeline the same way the I-gates do.
  gate('DEV_SIGNALS_UNRESOLVED',
    devBlocked.flatMap(r => r.unresolvedSignals.map(sig => `${r.storySlug}:${sig}`)))
}
log(`developer: ${devResults.length} stories, all exit gates clear`)

// === Role 6 — qa-engineer (consolidated) =================================
// One QA agent covers all stories sharing a runner/fixture set (the skill's
// default). It reads plan.md + the developer logs from the handoff dir itself.
phase('QA')
const qa = await agent(
  `task-slug: ${SLUG}\ncharter: ${CHARTER}\nrole: qa-engineer\n` +
  `story-slugs: ${devResults.map(r => r.storySlug).join(', ')}\n` +
  `read: ${HD}/plan.md + ${HD}/logs/developer-*.md\n` +
  `deliver: ${HD}/test-report.md (add ≥1 BDD/property/mutation test, or justify in log)`,
  { agentType: 'qa-engineer', schema: QA_SCHEMA, phase: 'QA', label: 'qa:test-report' },
)
// Gate I4 — no open QA defects before deploy.
gate('QA_DEFECT', qa.openDefects)

// === Role 7 — devops (SIDE-EFFECTFUL — deploys to a real cluster) ========
phase('DevOps')
let devops = null
if (A.skipDeploy !== false) {
  log('DevOps SKIPPED (skipDeploy default-true). Set skipDeploy:false to deploy. ' +
      'CLUSTER GATE: confirm kubectl context before enabling — never hs-cluster without YES.')
} else {
  devops = await agent(
    `task-slug: ${SLUG}\ncharter: ${CHARTER}\nrole: devops\n` +
    `read: [${HD}/design.md, ${HD}/implementation-notes.md, ${HD}/plan.md]\n` +
    `target-context: ${A.targetContext || '<REQUIRED>'}\n` +
    `deliver: ${HD}/deploy-notes.md + ${HD}/runbook.md\n` +
    `MUST verify \`kubectl config current-context\` before applying anything; ` +
    `deploy only to the target-context above; do NOT touch hs-cluster.`,
    { agentType: 'devops', schema: DEVOPS_SCHEMA, phase: 'DevOps', label: 'devops:deploy' },
  )
}

// === Role 8 — doc-writer (SIDE-EFFECTFUL — writes wiki + cognee) =========
phase('Doc-writer')
let docs = null
if (A.skipDocs !== false) {
  log('Doc-writer SKIPPED (skipDocs default-true). Set skipDocs:false to write ' +
      `the wiki page (~/workspace/wiki/pages/code/${SLUG}.md) and run cognee ingest.`)
} else {
  docs = await agent(
    `task-slug: ${SLUG}\ncharter: ${CHARTER}\nrole: doc-writer\n` +
    `read: all ${HD}/*.md deliverables\n` +
    `deliver: ~/workspace/wiki/pages/code/${SLUG}.md + ${HD}/docs-changes.md; ` +
    `update wiki index.md + append log.md; run cognee ingest in closing protocol`,
    { agentType: 'doc-writer', schema: DOC_SCHEMA, phase: 'Doc-writer', label: 'doc:wiki' },
  )
}

// Final punch list — the script returns structured facts, not prose.
return {
  runId: A.runId || `${SLUG}-?`,
  handoffDir: HD,
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
