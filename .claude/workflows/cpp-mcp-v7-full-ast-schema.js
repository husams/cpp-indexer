export const meta = {
  name: 'cpp-mcp-v7-full-ast-schema',
  description: 'Dev-team pipeline to implement the v7 full-C++-AST graph schema (Type/Parameter/Enumerator/Concept nodes + ~13 new type/template/enum/namespace edges) in cpp-indexer, schema_version 6→7, additive & non-breaking',
  whenToUse: 'Author-and-run instance of the 8-role dev-team pipeline, scoped to the v7 full-AST-schema PRD ([[pages/planning/cpp-mcp-v7-full-ast-schema]]). Indexer-side (schema emission) by default; the cross-repo cpp-mcp query/describe surface is a skip-by-default story. DevOps + Doc-writer are also skip-by-default.',
  phases: [
    { title: 'PM',         detail: 'product-manager → requirements.md (reconciles stale PRD vs live v6 schema)' },
    { title: 'BA',         detail: 'business-analyst → scenarios.md (Gherkin: the 8 reference questions + v6-read tolerance)' },
    { title: 'Architect',  detail: 'architect → design.md + adr-*.md (Type-node dedupe, naming reconciliations, version bump, cross-repo split)' },
    { title: 'Sr Dev',     detail: 'senior-developer → plan.md (stories along rollout S1–S6, dependency-ordered)' },
    { title: 'Developer',  detail: 'one developer agent per story, SEQUENTIAL shared-tree (dependency chain)' },
    { title: 'QA',         detail: 'qa-engineer → test-report.md (≥1 test/new-edge, ≥1/new-prop, exporter round-trip, v6 read-tolerance)' },
    { title: 'DevOps',     detail: 'devops → deploy-notes.md + runbook.md (SIDE-EFFECTFUL — skip by default)' },
    { title: 'Doc-writer', detail: 'doc-writer → wiki page + SCHEMA.md (SIDE-EFFECTFUL — skip by default)' },
  ],
}

// ---------------------------------------------------------------------------
// HOW TO RUN
//   1. Create the handoff dir + a CHARTER.md first (scripts have no fs access):
//        mkdir -p .claude/handoff/v7-ast-schema/logs
//        printf '%s\n' "v7 full-C++-AST graph schema for cpp-indexer (PRD: cpp-mcp-v7-full-ast-schema)" \
//          > .claude/handoff/v7-ast-schema/CHARTER.md
//   2. Workflow({ scriptPath: '.claude/workflows/cpp-mcp-v7-full-ast-schema.js', args: {
//        slug: 'cpp-mcp-v7-full-ast-schema',
//        handoffDir: '/Users/husam/workspace/cpp-indexer/.claude/handoff/v7-ast-schema',
//        runId: 'cpp-mcp-v7-1',          // caller computes — no Date.now() in scripts
//        lang: 'rust',
//        targetContext: 'admin@dev-cluster',
//        skipMcpRepo: true,   // default-safe: indexer-side only; cpp-mcp query surface is a follow-up
//        skipDeploy:  true,   // default-safe
//        skipDocs:    true,   // default-safe
//      }})
//
// RESUMABILITY: S1–S6 is large and dependency-chained; one run may not land all six
// stories. Each stage returns structured facts only, so re-invoke with
// { scriptPath, resumeFromRunId: '<runId from the result>' } to reuse cached
// PM/BA/Architect/Sr-Dev planning stages and re-run only the developer/QA tail.
//
// WHY a feature-specific script (vs the generic dev-team-pipeline.js scaffold):
// the PRD ([[pages/planning/cpp-mcp-v7-full-ast-schema]]) was written 2026-05-17
// against schema_version 1 and is STALE. The live codebase is at SCHEMA_VERSION=6
// and already EMITS several nodes/edges the PRD lists as "new" (verified against
// src/schema + src/visit/shallow.rs @ commit d08d6a1, 2026-05-31). This script
// bakes in the VERIFIED three-way delta so PM/Architect implement the real gap,
// not the stale PRD's claimed gap, and so the bump target is 7 (not the PRD's "2").
// ---------------------------------------------------------------------------

const A = args || {}
const HD = A.handoffDir || '/Users/husam/workspace/cpp-indexer/.claude/handoff/v7-ast-schema'
const SLUG = A.slug || 'cpp-mcp-v7-full-ast-schema'
const LANG = A.lang || 'rust'
const CHARTER = `@${HD}/CHARTER.md`
const PRD = '~/workspace/wiki/pages/planning/cpp-mcp-v7-full-ast-schema.md'
const MCP_REPO = '~/workspace/cpp-mcp'

// The baked-in feature brief the product-manager turns into requirements.md.
// Every fact is grounded in cpp-indexer source as of commit d08d6a1 (2026-05-31).
const REQUIREMENTS = `
FEATURE: Extend the cpp-indexer CodexGraph schema so a single read-only query can
answer any structural question derivable from libclang's AST (PRD target: ≥95% of
C++ relations) without re-parsing source. Additive and NON-BREAKING.

PRD SOURCE: ${PRD}  (read it — but it is STALE; reconcile against the facts below.)

>>> STALE-PRD RECONCILIATION (NON-NEGOTIABLE — the whole point of this run) <<<
The PRD was authored against schema_version 1. The live codebase is at
SCHEMA_VERSION = 6 (src/schema/version.rs) and already implements much of what the
PRD calls "new". Build on the VERIFIED three-way delta below, NOT the PRD's prose:

  ALREADY DONE — do NOT re-implement (defined in src/schema AND emitted in
  src/visit/shallow.rs as of commit d08d6a1):
    • Nodes: Class, Method, Field, GlobalVariable, Enum, Namespace, TemplateDef,
      Specialization, Typedef, Header, Module, Macro, Repo.
      → The PRD's "split Variable→Field/GlobalVariable" (S1) and "Enum+Enumerator
        currently missing entirely" are WRONG for nodes: Field, GlobalVariable, and
        Enum already exist and are emitted. (Enumerator does NOT — see below.)
    • Edges: Contains, HasMethod, HasField, Inherits, Uses, Calls, Includes,
      Overrides, Instantiates, Specializes, FriendOf, AdlCandidate, BelongsToRepo,
      ExternalRef, ExpandsTo.
      → PRD's "new" OVERRIDES / FRIEND_OF / INSTANTIATES / SPECIALIZES already
        exist and are emitted (INHERITS/CALLS/INSTANTIATES + full SPECIALIZES were
        wired up in commit d08d6a1, today).

  PROPERTIES-ONLY DELTA — kind exists & emitted; v7 only adds attributes:
    • INHERITS edge: add \`access\` (public/protected/private) + \`is_virtual\`.

  NAMING RECONCILIATIONS — the architect MUST settle these as ADRs (do not blindly
  add a duplicate kind):
    • PRD "REFERENCES" edge vs the existing \`Uses\` edge — decide rename/alias/keep.
    • PRD "MEMBER_OF (+access)" vs the existing \`HasMethod\`/\`HasField\` edges —
      the access property likely belongs on HasMethod/HasField, not a new edge.

  GENUINELY ABSENT — these ARE the real v7 work (new kind + emit + read-tolerance):
    • Nodes: Type, Parameter, Enumerator, Concept.
    • Edges: Returns, HasParam, OfType, PointsTo, RefersTo, AliasOf, TemplateParam,
      TemplateArg, ConstrainedBy, UsesNamespace, UsesDeclaration, EnumeratorOf,
      UnderlyingType.
    • New props on existing nodes: Function (signature, is_template, is_virtual,
      is_override, is_constexpr, is_noexcept, is_deleted, is_defaulted,
      cv_qualifiers, ref_qualifier); Class (is_template, is_final, is_abstract,
      record_kind); Field/GlobalVariable (is_static, is_const, is_constexpr,
      storage_class).

HARD CONSTRAINTS (each becomes ≥1 acceptance criterion):
  C1. SCHEMA_VERSION bump = 6 → 7 (NOT the PRD's "2"; it is a monotonic counter in
      src/schema/version.rs). Bump SCHEMA_VERSION_TAG too. The schema_drift test
      (tests/schema_drift.rs) + schema_version_bump gate MUST stay green — they
      fail the build if version, prompt/graph_database/cpp/schema.txt, and
      docs/schema/SCHEMA.md drift out of sync, so update all three together.
  C2. NON-BREAKING / READ-TOLERANCE. v6 graphs remain readable: new fields are
      OPTIONAL on read; the read/exporter path (src/schema/arrow.rs) tolerates
      their absence. No existing node/edge kind is removed or renamed without an
      accepted ADR + back-compat alias.
  C3. Emit from the real visit path. New nodes/edges/props are produced in
      src/visit/shallow.rs (and helpers under src/visit/) from libclang cursors,
      and flow through src/schema/{nodes.rs,edges.rs,arrow.rs} to BOTH sinks
      (src/sink/neo4j.rs AND src/sink/indradb.rs).
  C4. Type-node explosion mitigation (PRD risk): canonicalize Type nodes by
      spelling and dedupe at the exporter so templated code does not blow up the
      graph. State the dedupe key.
  C5. libclang fidelity: template-arg recovery on dependent types is lossy.
      Document precisely what is and is NOT extracted; degrade gracefully (no
      silent total-TU failure — Issue 0001 family).
  C6. Ordered traversal: TEMPLATE_ARG / HAS_PARAM are ordered by \`index\`. The
      IndraDB sink's verb subset may need an ordered-by-property read path —
      scope it or explicitly defer with justification.
  C7. CROSS-REPO BOUNDARY. The MCP tool surface that answers PRD AC#1
      (describe_graph_schema returns the new version + totals) and AC#2 (the 8
      reference questions via query_graphdb) lives in the SEPARATE sibling repo
      ${MCP_REPO}, NOT in cpp-indexer. This run is INDEXER-SCOPED by default:
      land the emitted schema here; the cpp-mcp query/describe work is a
      skip-by-default follow-up story (skipMcpRepo).

THE 8 REFERENCE QUESTIONS (coverage target — drive BA scenarios + QA from these):
  1 Is type X an instance of template Y?     (X)-[:INSTANTIATES]->(Y)
  2 Members of class X (+access filter)       HasMethod/HasField + access
  3 Signature of function Y                    Y.signature OR HAS_PARAM+RETURNS
  4 Return type of X                           (X)-[:RETURNS]->(:Type)
  5 Where is X referenced                      Uses/REFERENCES
  6 Classes inheriting X (+access/virtual)     INHERITS (+new props)
  7 Base class of Y                            INHERITS
  8 Template args of X (ordered by index)      (X)-[:TEMPLATE_ARG]->(:Type)

OUT OF SCOPE (state explicitly): NL→Cypher (translate_query); CFG/DFG edges; macro
expansion bodies as nodes; cross-TU ODR merging beyond v6; gRPC (M9/M10). The
cpp-mcp query/describe surface unless skipMcpRepo:false.

SUCCESS CRITERIA (PRD acceptance, de-staled): describe output reports schema 7 with
all new node/edge types + totals; ≥1 unit test per new edge type and per new
property; exporter round-trip parity (write→read→compare) on both sinks; live
integration covers template instantiation, virtual override, namespace
using-directive, enum+enumerator, typedef chain via ALIAS_OF; v6 graphs still
queryable; FULL existing test suite green (the PRD's "880" count is stale — assert
"no regression in the existing suite", and the known-unrelated schema_drift case
must itself be updated by C1, not left red).
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
  required: ['deliverable', 'status', 'adrs', 'schemaVersionTarget', 'breakingChange'],
  properties: {
    deliverable: { type: 'string' }, status: { enum: ['clear', 'blocked'] },
    adrs: { type: 'array', items: { type: 'object', additionalProperties: false,
      required: ['file', 'topic', 'status'],
      properties: { file: { type: 'string' }, topic: { type: 'string' },
        status: { enum: ['accepted', 'proposed'] } } } },
    schemaVersionTarget: { type: 'integer', description: 'must be 7 (current 6 + 1) — gate I2b' },
    breakingChange: { type: 'boolean', description: 'must be false — v7 is additive/non-breaking — gate I2c' } } }
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
  `requirements (raw, authoritative — the STALE-PRD RECONCILIATION block is the crux; ` +
  `preserve every HARD CONSTRAINT C1–C7):\n<<<\n${REQUIREMENTS}\n>>>\n` +
  `deliver: ${HD}/requirements.md — scope, out-of-scope, acceptance criteria per story. ` +
  `Frame stories along the PRD rollout S1–S6 but CORRECTED for the live v6 schema (drop the ` +
  `already-done Variable-split; S5 enum work is Enumerator-node + EnumeratorOf/UnderlyingType only). ` +
  `Every HARD CONSTRAINT C1–C7 must map to ≥1 acceptance criterion. Mark the cpp-mcp query/describe ` +
  `surface (C7) as a clearly-labelled deferred-by-default story.`,
  { agentType: 'product-manager', schema: PM_SCHEMA, phase: 'PM', label: 'pm:requirements' },
)
gate('MISSING_ACCEPTANCE_CRITERIA', pm.storiesMissingAC)

// === Role 2 — business-analyst ===========================================
phase('BA')
const ba = await agent(
  `task-slug: ${SLUG}\ncharter: ${CHARTER}\nrole: business-analyst\nread: [${HD}/requirements.md]\n` +
  `deliver: ${HD}/scenarios.md (Gherkin). Drive scenarios from the 8 REFERENCE QUESTIONS plus the ` +
  `edge cases: Type-node dedupe (same spelling → one node), ordered TEMPLATE_ARG/HAS_PARAM by index, ` +
  `typedef chain via ALIAS_OF, enum + Enumerator + UNDERLYING_TYPE, using-directive vs using-declaration, ` +
  `lossy dependent template-arg recovery (graceful degrade, no silent TU failure), and v6-graph ` +
  `read-tolerance (missing v7 fields must not break reads).`,
  { agentType: 'business-analyst', schema: BA_SCHEMA, phase: 'BA', label: 'ba:scenarios' },
)
log(`BA wrote ${ba.scenarioCount} scenarios`)

// === Role 3 — architect ==================================================
phase('Architect')
const arch = await agent(
  `task-slug: ${SLUG}\ncharter: ${CHARTER}\nrole: architect\n` +
  `read: [${HD}/requirements.md, ${HD}/scenarios.md]; inspect src/schema/{nodes.rs,edges.rs,arrow.rs,version.rs} ` +
  `and src/visit/shallow.rs to confirm the three-way delta before designing.\n` +
  `deliver: ${HD}/design.md + one ${HD}/adr-<n>.md per cross-cutting decision. ` +
  `REQUIRED ADR TOPICS (each must be Status: accepted, NOT proposed):\n` +
  `  • ADR "schema-version-7": SCHEMA_VERSION 6→7 + tag + schema_drift/schema.txt/SCHEMA.md sync (C1).\n` +
  `  • ADR "type-node-canonicalization": Type-node dedupe key (by canonical spelling) + exporter dedupe (C4).\n` +
  `  • ADR "edge-naming-reconciliation": REFERENCES-vs-Uses and MEMBER_OF-vs-HasMethod/HasField — pick one, ` +
  `with a back-compat alias if any existing name changes (C2).\n` +
  `  • ADR "v6-read-tolerance": new fields optional on read; arrow.rs/exporter tolerate absence (C2).\n` +
  `  • ADR "indradb-ordered-traversal": ordered-by-index read path for TEMPLATE_ARG/HAS_PARAM, or explicit defer (C6).\n` +
  `  • ADR "cross-repo-boundary": what lands in cpp-indexer vs what is deferred to ${MCP_REPO} (C7).\n` +
  `Return schemaVersionTarget (MUST be 7) and breakingChange (MUST be false).\n${langNote}`,
  { agentType: 'architect', schema: ARCH_SCHEMA, phase: 'Architect', label: 'arch:design' },
)
gate('ADR_UNRESOLVED', arch.adrs.filter(a => a.status === 'proposed').map(a => a.file))
gate('WRONG_SCHEMA_VERSION', arch.schemaVersionTarget !== 7
  ? [`schemaVersionTarget=${arch.schemaVersionTarget}, must be 7 (current 6 + 1)`] : [])
gate('UNEXPECTED_BREAKING_CHANGE', arch.breakingChange
  ? ['design proposes a breaking change; v7 must be additive/non-breaking'] : [])

// === Role 4 — senior-developer ===========================================
phase('Sr Dev')
const plan = await agent(
  `task-slug: ${SLUG}\ncharter: ${CHARTER}\nrole: senior-developer\n` +
  `read: [${HD}/requirements.md, ${HD}/scenarios.md, ${HD}/design.md, ${HD}/adr-*.md]\n` +
  `deliver: ${HD}/plan.md — dependency-ORDERED stories along the corrected rollout. Suggested slices ` +
  `(split/merge, keep small; later stories depend on earlier — order matters):\n` +
  `  (a) Version+props: SCHEMA_VERSION 6→7, tag, schema.txt/SCHEMA.md sync, + new scalar props on ` +
  `Function/Class/Field/GlobalVariable and access/is_virtual on INHERITS (nodes.rs/edges.rs/arrow.rs/shallow.rs).\n` +
  `  (b) Type node + RETURNS/OF_TYPE/HAS_PARAM + Parameter node + function signature compose (the big one; ` +
  `everything below references Type).\n` +
  `  (c) Templates: TEMPLATE_PARAM/TEMPLATE_ARG (ordered) + Concept node + CONSTRAINED_BY.\n` +
  `  (d) Aliasing + pointers: ALIAS_OF, POINTS_TO/REFERS_TO.\n` +
  `  (e) Enums + namespaces: Enumerator node, ENUMERATOR_OF, UNDERLYING_TYPE, USES_NAMESPACE/USES_DECLARATION.\n` +
  `  (f) IndraDB ordered-traversal read path (or documented defer) + exporter round-trip both sinks.\n` +
  `  (g) [DEFERRED unless skipMcpRepo:false] cpp-mcp describe_graph_schema v7 output + 8-question query validation in ${MCP_REPO}.\n` +
  `Each story needs: title, files-to-touch (explicit paths under src/), exit-criteria (cargo fmt --check && ` +
  `cargo clippy -- -D warnings && cargo test <scope> && cargo test --test schema_drift), parallel-safe ` +
  `(set FALSE for the chained schema stories — they share nodes.rs/edges.rs/arrow.rs/shallow.rs).\n${langNote}`,
  { agentType: 'senior-developer', schema: PLAN_SCHEMA, phase: 'Sr Dev', label: 'srdev:plan' },
)
gate('MISSING_EXIT_CRITERIA', plan.stories.filter(s => !s.exitCriteria.length).map(s => s.title))

// Optionally drop the cross-repo cpp-mcp story (default: indexer-scoped only).
const slug = (t) => t.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '')
const isMcpStory = (s) =>
  /cpp-mcp|describe_graph_schema|query_graphdb|8 (reference )?question/i.test(s.title) ||
  s.filesToTouch.some(f => /cpp-mcp/.test(f))
let stories = plan.stories
if (A.skipMcpRepo !== false) {
  const dropped = stories.filter(isMcpStory)
  stories = stories.filter(s => !isMcpStory(s))
  if (dropped.length) log(`skipMcpRepo (default-true): deferring ${dropped.length} cross-repo cpp-mcp story(ies): ` +
    `${dropped.map(s => s.title).join(', ')} — set skipMcpRepo:false to include ${MCP_REPO} work.`)
}

// === Role 5 — developer (SEQUENTIAL, shared main tree) ===================
// These stories are a DEPENDENCY CHAIN: the Type node (b) is referenced by
// templates (c), aliasing (d), enums (e), and the round-trip (f). They also share
// files (nodes.rs, edges.rs, arrow.rs, shallow.rs, version.rs). Therefore: NO
// worktree isolation (would hide each story from the next) and NO parallelism.
// Run strictly in plan order in the main tree so each developer builds on the
// previous one's committed-to-disk code. (Same rationale as graph-symbol-ids.)
phase('Developer')
const devPrompt = (s) =>
  `task-slug: ${SLUG}\ncharter: ${CHARTER}\nstory: "${s.title}"\n` +
  `read: [${HD}/plan.md (this story + any "Cross-story API contract" section), ${HD}/design.md, ${HD}/adr-*.md, ${HD}/scenarios.md]\n` +
  `files-to-touch: ${s.filesToTouch.join(', ')}\n` +
  `IMPORTANT: earlier stories already landed in this working tree — REUSE the real types they added ` +
  `(e.g. the Type node + its dedupe helper, the Parameter struct); do NOT re-create modules that exist. ` +
  `Every schema change MUST keep the schema_drift trio in sync (version.rs + prompt/graph_database/cpp/schema.txt ` +
  `+ docs/schema/SCHEMA.md) or the gate fails.\n` +
  `exit-gate (run internally, max 3 passes): ${LIBCLANG_ENV} sh -c '${s.exitCriteria.join(' && ')}'\n` +
  `log: ${HD}/logs/developer-${slug(s.title)}.md\n${langNote}`
const devOpts = (s) => ({ agentType: 'developer', schema: DEV_SCHEMA, phase: 'Developer',
  label: `dev:${slug(s.title)}` })

const devResults = []
for (const s of stories) devResults.push(await agent(devPrompt(s), devOpts(s)))

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
  `deliver: ${HD}/test-report.md. Must include: ≥1 test per NEW edge type (Returns, HasParam, OfType, ` +
  `PointsTo/RefersTo, AliasOf, TemplateParam, TemplateArg, ConstrainedBy, UsesNamespace, UsesDeclaration, ` +
  `EnumeratorOf, UnderlyingType) and per NEW property; exporter round-trip parity (write→read→compare) on ` +
  `BOTH sinks; ordered TEMPLATE_ARG/HAS_PARAM by index; v6-graph read-tolerance (a v6 fixture still reads ` +
  `with v7 code); the schema_drift suite green; and the FULL existing suite green (no regression — the ` +
  `PRD's "880" is stale, just assert the whole suite passes). Validate the 8 reference questions are ` +
  `answerable from the emitted graph (graph-level assertions; the query-tool layer itself is cpp-mcp/deferred).`,
  { agentType: 'qa-engineer', schema: QA_SCHEMA, phase: 'QA', label: 'qa:test-report' },
)
gate('QA_DEFECT', qa.openDefects)

// === Role 7 — devops (SIDE-EFFECTFUL — deploys to a real cluster) ========
phase('DevOps')
let devops = null
if (A.skipDeploy === false) {
  devops = await agent(
    `task-slug: ${SLUG}\ncharter: ${CHARTER}\nrole: devops\n` +
    `read: [${HD}/design.md, ${HD}/plan.md]\n` +
    `target-context: ${A.targetContext || '<REQUIRED>'}\n` +
    `deliver: ${HD}/deploy-notes.md + ${HD}/runbook.md\n` +
    `MUST verify \`kubectl config current-context\` before applying anything; deploy only to the ` +
    `target-context above; do NOT touch hs-cluster. SCHEMA_VERSION bumped 6→7 — the runbook MUST cover ` +
    `re-indexing existing graphs and confirm v6 graphs remain readable (no destructive reset required ` +
    `since v7 is additive).`,
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
    `deliver: ~/workspace/wiki/pages/code/${SLUG}.md + ${HD}/docs-changes.md. Update docs/schema/SCHEMA.md ` +
    `to v7; mark the PRD ${PRD} as implemented (status), and record what is DEFERRED to ${MCP_REPO}. ` +
    `Update wiki index.md + append log.md; cross-link [[pages/code/cpp-indexer]] and the PRD; run cognee ` +
    `ingest in the closing protocol.`,
    { agentType: 'doc-writer', schema: DOC_SCHEMA, phase: 'Doc-writer', label: 'doc:wiki' },
  )
} else {
  log('Doc-writer SKIPPED (skipDocs default-true). Set skipDocs:false to write ' +
      `the wiki page (~/workspace/wiki/pages/code/${SLUG}.md) + SCHEMA.md and run cognee ingest.`)
}

// Final punch list — structured facts, not prose.
return {
  runId: A.runId || `${SLUG}-?`,
  handoffDir: HD,
  reconciliation: {
    schemaVersion: '6 → 7 (PRD said 2 — stale)',
    alreadyDone: 'Field/GlobalVariable/Enum nodes; Overrides/FriendOf/Instantiates/Specializes/Calls edges',
    realDelta: 'Type/Parameter/Enumerator/Concept nodes; Returns/HasParam/OfType/PointsTo/RefersTo/AliasOf/TemplateParam/TemplateArg/ConstrainedBy/UsesNamespace/UsesDeclaration/EnumeratorOf/UnderlyingType edges; +scalar props',
    crossRepo: `cpp-mcp describe/query surface deferred to ${MCP_REPO} (skipMcpRepo=${A.skipMcpRepo !== false})`,
  },
  gates: { I1: 'MISSING_ACCEPTANCE_CRITERIA ok', I2: 'ADR_UNRESOLVED ok',
           I2b: 'WRONG_SCHEMA_VERSION ok (=7)', I2c: 'UNEXPECTED_BREAKING_CHANGE ok',
           I3: 'MISSING_EXIT_CRITERIA ok', I4: 'QA_DEFECT ok' },
  deliverables: {
    requirements: pm.deliverable, scenarios: ba.deliverable, design: arch.deliverable,
    plan: plan.deliverable, testReport: qa.deliverable,
    deploy: devops ? devops.deliverable : 'skipped',
    wiki: docs ? docs.wikiPage : 'skipped',
  },
  stories: devResults.map(r => ({ slug: r.storySlug, status: r.status })),
}
