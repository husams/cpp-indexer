# CIDX GraphView contract and UX prototype

This is the HSE-90 M0 prototype. It is deliberately offline and fixture-backed:
the browser receives a typed `GraphViewResult`, adapts it to Cytoscape.js, and
never reads SQLite or executes a query.

## Contract boundary

`src/graph-view.ts` is renderer-independent. It defines versioned requests and
results, a shared HSE-70-shaped `ResultEnvelope<GraphViewResult>` boundary,
portable semantic identities, nodes, edges, compound groups, typed bounded
evidence references, capabilities, continuation tokens, budgets, and
status/truth markers. Cytoscape element JSON and layout/position state live only
in `src/cytoscape-adapter.ts` and `src/main.ts`.

Portable identity uses a bounded semantic-universe key. Database-local integers
and row IDs are rejected. `resultId` and `queryIdentity` describe semantic
content; `SavedView` holds only presentation state (layout, pan/zoom, filters,
and collapsed groups).

## Visual truth rules

The base status (`complete`, `partial`, `unknown`, or `error`) is never promoted
by the renderer. Any weaker result, or any marker such as `truncated`, `stale`,
`unresolved`, `external`, `inferred`, `assumed`, `refuted`, or `proved`, gets a
glyph, line/border pattern, and inspector text. The legend repeats those cues
so status does not depend on colour alone.

## Prototype slices

The fixed canonical fixtures demonstrate symbol/call, entity/architecture,
include/file, and type/signature views. The graph canvas supports hierarchy,
neighborhood, circle, and grid layouts; selecting any node or edge exposes its
portable key, semantic kind, status, source location, and bounded evidence.
Compound grouping is represented as a group record in the contract and as a
Cytoscape parent only in the adapter.

## Deterministic budgets

`applyBudget` sorts by portable identity, keeps only bounded nodes/edges/groups,
retains referenced evidence in stable order, and returns `partial` plus a
`truncated` marker and `budget_exhausted` diagnostic when input is oversized.
This is a deterministic refusal/degradation response, not an unbounded browser
operation.

## Commands

```sh
npm ci
npm run check
npm run dev
```

`npm run generate` produces the checked-in TypeScript catalog projection from
the repository's `catalogs/core.json`; semantic relation names are not
hand-copied into the UI.

The focused tests validate `schemas/graph-view.schema.json` against every
canonical envelope, a continuation, a saved view, and invalid identity/evidence
payloads. The schema uses Draft 2020-12 `unevaluatedProperties` composition so
the shared element state and node/edge/group fields validate together.
