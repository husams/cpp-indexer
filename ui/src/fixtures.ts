import { applyBudget, GRAPH_VIEW_VERSION, usageOf, type Budget, type CounterexampleOverlay, type DiffOverlay, type EffectOverlay, type EvidenceOverlay, type EvidenceReference, type GraphEdge, type GraphGroup, type GraphKind, type GraphNode, type GraphViewResult, type OverlayIdentity, type PortableReference, type ProofOverlay, type SiteReference } from "./graph-view.ts";

const fixtureBudget: Budget = { maxNodes: 32, maxEdges: 48, maxGroups: 8, maxLabelChars: 2_400, maxEvidenceRefs: 64, maxSites: 256, maxSiteBytes: 32_768 };
const universe = "fixture:canonical-v1";

function ref(kind: PortableReference["kind"], key: string): PortableReference {
  return { kind, key, semanticUniverse: universe };
}

function node(kind: GraphKind, key: string, label: string, semanticKind: string, state: Partial<Pick<GraphNode, "status" | "markers" | "evidenceRefs">> = {}): GraphNode {
  return { ref: ref(kind, key), label, semanticKind, status: state.status ?? "complete", markers: state.markers ?? [], evidenceRefs: state.evidenceRefs ?? [] };
}

function edge(key: string, source: PortableReference, target: PortableReference, relation: string, label: string, state: Partial<Pick<GraphEdge, "status" | "markers" | "evidenceRefs" | "siteRefs">> = {}): GraphEdge {
  return { ref: ref("edge", key), source, target, relation, label, status: state.status ?? "complete", markers: state.markers ?? [], evidenceRefs: state.evidenceRefs ?? [], siteRefs: state.siteRefs ?? [] };
}

function group(key: string, label: string, memberRefs: readonly PortableReference[]): GraphGroup {
  return { ref: ref("group", key), label, memberRefs, status: "complete", markers: [], evidenceRefs: [] };
}

const evidence: readonly EvidenceReference[] = [
  { id: "ev:render-declaration", class: "source", role: "declaration", summary: "render is declared in renderer.hpp", location: { path: "src/renderer.hpp", line: 18, column: 6 } },
  { id: "ev:render-call-site", class: "source", role: "call-site", summary: "render is called by preview", location: { path: "src/preview.cpp", line: 42, column: 9 } },
  { id: "ev:derived-entity", class: "derived", role: "derived", summary: "component relation derived from catalog", artifact: "query-result:entity-fixture" },
  { id: "ev:external-type", class: "inferred", role: "type-use", summary: "external type identity is unresolved locally" },
  { id: "ev:proof-refutation", class: "proof", role: "derived", summary: "proof producer refuted the canvas invariant", artifact: "tlaplus:CanvasSafety:counterexample-1" },
];

function overlayIdentity(resultId: string, evidenceId: string | undefined, source: OverlayIdentity["source"], limitation?: string): OverlayIdentity {
  return { resultId, ...(evidenceId === undefined ? {} : { evidenceId }), ...(source === undefined ? {} : { source }), ...(limitation === undefined ? {} : { limitation }) };
}

function overlayFixtures(resultId: string, render: PortableReference, preview: PortableReference, draw: PortableReference): GraphViewResult["overlays"] {
  const source = { path: "src/renderer.cpp", line: 72 };
  const evidenceOverlay: EvidenceOverlay = {
    kind: "evidence",
    title: "Bounded call explanation",
    items: [
      { id: "finding:render", label: "render call finding", strength: "direct", identity: overlayIdentity(resultId, "ev:render-call-site", { path: "src/preview.cpp", line: 42 }) },
      { id: "finding:draw", label: "draw is inferred beyond the indexed site", strength: "inferred", identity: overlayIdentity(resultId, "ev:derived-entity", undefined, "inference boundary: callee body not indexed") },
    ],
    path: [
      { sequence: 0, label: "preview", relation: "calls", identity: overlayIdentity(resultId, "ev:render-call-site", { path: "src/preview.cpp", line: 42 }) },
      { sequence: 1, label: "render", relation: "calls", identity: overlayIdentity(resultId, "ev:render-call-site", source) },
      { sequence: 2, label: "draw", relation: "inferred calls", identity: overlayIdentity(resultId, "ev:derived-entity", undefined, "bounded explanation path") },
    ],
  };
  const diffOverlay: DiffOverlay = {
    kind: "diff",
    title: "Workspace graph diff",
    oldWorkspace: { key: "workspace:fixture", version: "workspace-v1" },
    newWorkspace: { key: "workspace:fixture", version: "workspace-v2" },
    oldFactSet: { key: "facts:symbol", version: "facts-v1" },
    newFactSet: { key: "facts:symbol", version: "facts-v2" },
    entries: [
      { id: "diff:added", label: "paint dependency", state: "added", newRef: draw, identity: overlayIdentity(resultId, "ev:external-type", undefined, "new fact only") },
      { id: "diff:removed", label: "legacy preview edge", state: "removed", oldRef: preview, identity: overlayIdentity(resultId, "ev:render-call-site", { path: "src/preview.cpp", line: 42 }) },
      { id: "diff:changed", label: "render effect summary", state: "changed", oldRef: render, newRef: render, identity: overlayIdentity(resultId, "ev:render-declaration", { path: "src/renderer.hpp", line: 18 }) },
      { id: "diff:invalidated", label: "stale type fact", state: "invalidated", oldRef: render, newRef: render, identity: overlayIdentity(resultId, undefined, undefined, "fact-set identity changed") },
      { id: "diff:unchanged", label: "source declaration", state: "unchanged", oldRef: render, newRef: render, identity: overlayIdentity(resultId, "ev:render-declaration", { path: "src/renderer.hpp", line: 18 }) },
    ],
  };
  const effectOverlay: EffectOverlay = {
    kind: "effect",
    title: "Effect summary",
    summary: "render may mutate the canvas; the external paint effect is unknown.",
    regions: [
      { id: "effect:canvas", label: "canvas state", state: "known", identity: overlayIdentity(resultId, "ev:render-declaration", source) },
      { id: "effect:vendor", label: "vendor graphics state", state: "unknown", identity: overlayIdentity(resultId, "ev:external-type", undefined, "external implementation not available") },
    ],
    callDependencies: [{ id: "effect:draw", label: "render → draw", state: "conditional", identity: overlayIdentity(resultId, "ev:render-call-site", source) }],
    assumptions: [{ id: "effect:assumption", label: "canvas handle remains valid", state: "conditional", identity: overlayIdentity(resultId, "ev:derived-entity", undefined, "assumption supplied by producer") }],
    targetCoverage: [{ id: "effect:target", label: "renderer.cpp", state: "known", identity: overlayIdentity(resultId, "ev:render-declaration", source) }],
    unknownBoundaries: [{ id: "effect:unknown", label: "unresolved vendor::paint", state: "unknown", identity: overlayIdentity(resultId, "ev:external-type", undefined, "external boundary") }],
  };
  const proofOverlay: ProofOverlay = {
    kind: "proof",
    title: "Proof obligations",
    claims: [
      { id: "proof:root", label: "render preserves canvas invariant", state: "refuted", identity: overlayIdentity(resultId, "ev:proof-refutation", source) },
      { id: "proof:condition", parentId: "proof:root", label: "vendor paint is total", state: "conditional", identity: overlayIdentity(resultId, "ev:external-type", undefined, "depends on external model") },
      { id: "proof:open", parentId: "proof:root", label: "draw releases the canvas handle", state: "open", identity: overlayIdentity(resultId, "ev:render-call-site", undefined, "no proof artifact supplied") },
      { id: "proof:assumption", parentId: "proof:condition", label: "graphics model is trusted", state: "assumed", identity: overlayIdentity(resultId, "ev:derived-entity", undefined, "trusted model declaration") },
      { id: "proof:proved", parentId: "proof:condition", label: "renderer declaration is reachable", state: "proved", identity: overlayIdentity(resultId, "ev:render-declaration", { path: "src/renderer.hpp", line: 18 }) },
      { id: "proof:inferred", parentId: "proof:root", label: "draw is the only observed callee", state: "inferred", identity: overlayIdentity(resultId, "ev:derived-entity", undefined, "bounded call graph") },
    ],
    trustedModels: ["canvas-safety-v1"],
    assumptions: ["vendor::paint is modeled as an external function"],
  };
  const counterexampleOverlay: CounterexampleOverlay = {
    kind: "counterexample",
    title: "TLA+ safety violation",
    specification: "CanvasSafety",
    violation: "Invariant CanvasHandleValid is false at step 2",
    steps: [
      { index: 0, action: "Init", state: [{ name: "handleValid", value: "TRUE" }, { name: "frame", value: "0" }], identity: overlayIdentity(resultId, "ev:proof-refutation", source) },
      { index: 1, action: "CallRender", state: [{ name: "handleValid", value: "TRUE" }, { name: "frame", value: "1" }], identity: overlayIdentity(resultId, "ev:render-call-site", { path: "src/preview.cpp", line: 42 }) },
      { index: 2, action: "VendorPaint", state: [{ name: "handleValid", value: "FALSE" }, { name: "frame", value: "1" }], identity: overlayIdentity(resultId, "ev:proof-refutation", undefined, "TLA+ counterexample state") },
    ],
  };
  return { evidence: evidenceOverlay, diff: diffOverlay, effect: effectOverlay, proof: proofOverlay, counterexample: counterexampleOverlay, exportMaxBytes: 16_384 };
}

function baseResult(slice: GraphKind, nodes: readonly GraphNode[], edges: readonly GraphEdge[], groups: readonly GraphGroup[], resultId: string, extra: Partial<GraphViewResult> = {}): GraphViewResult {
  const result: GraphViewResult = {
    version: GRAPH_VIEW_VERSION,
    resultId,
    queryIdentity: `cxq:fixture:${slice}:render`,
    workspace: { key: "workspace:fixture", version: "fixture-v1" },
    index: { key: "index:fixture", version: "index-v1" },
    factSet: { key: `facts:${slice}`, version: "facts-v1" },
    status: "complete",
    markers: [],
    freshness: "fresh",
    completeness: "complete",
    budget: fixtureBudget,
    usage: usageOf({ nodes, edges, groups, evidence }),
    nodes,
    edges,
    groups,
    evidence,
    capabilities: [
      { operation: "expand", direction: "out", maxDepth: 2 },
      { operation: "path", maxDepth: 4 },
      { operation: "evidence" },
    ],
    diagnostics: [],
    ...extra,
  };
  return applyBudget(result, result.budget);
}

export function canonicalFixture(slice: Exclude<GraphKind, "group" | "file">): GraphViewResult {
  if (slice === "symbol") return symbolFixture();
  if (slice === "entity") return entityFixture();
  if (slice === "include") return includeFixture();
  return typeFixture();
}

export type OverlayFixture = "evidence" | "diff" | "effect" | "proof" | "counterexample";

export function overlayFixture(kind: OverlayFixture): GraphViewResult {
  const resultId = `result:overlay:${kind}:v1`;
  const render = ref("symbol", "symbol:renderer::render");
  const preview = ref("symbol", "symbol:preview::preview");
  const draw = ref("symbol", "symbol:canvas::draw");
  const base = symbolFixture();
  const overlays = overlayFixtures(resultId, render, preview, draw);
  return applyBudget({ ...base, resultId, queryIdentity: `cxq:fixture:overlay:${kind}`, evidence: [...evidence], overlays, ...(kind === "proof" || kind === "counterexample" ? { markers: ["refuted" as const] } : {}) }, fixtureBudget);
}

function symbolFixture(): GraphViewResult {
  const render = ref("symbol", "symbol:renderer::render");
  const preview = ref("symbol", "symbol:preview::preview");
  const draw = ref("symbol", "symbol:canvas::draw");
  const unresolved = ref("symbol", "symbol:vendor::paint");
  const nodes = [
    { ...node("symbol", render.key, "render", "function", { evidenceRefs: ["ev:render-declaration"] }), location: { path: "src/renderer.hpp", line: 18, column: 6 } },
    { ...node("symbol", preview.key, "preview", "function", { evidenceRefs: ["ev:render-call-site"] }), location: { path: "src/preview.cpp", line: 42, column: 9 } },
    { ...node("symbol", draw.key, "draw", "method", { status: "partial", markers: ["inferred"], evidenceRefs: ["ev:render-call-site"] }), location: { path: "src/canvas.cpp", line: 61 } },
    node("symbol", unresolved.key, "paint", "function", { status: "unknown", markers: ["unresolved", "external"], evidenceRefs: ["ev:external-type"] }),
  ];
  const edges = [
    edge("edge:preview-calls-render", preview, render, "calls", "calls", { evidenceRefs: ["ev:render-call-site"], siteRefs: [{ id: "site:preview-render", role: "call-site", location: { path: "src/preview.cpp", line: 42 } }] }),
    edge("edge:render-calls-draw", render, draw, "calls", "calls", { status: "partial", markers: ["truncated"], evidenceRefs: ["ev:render-call-site"], siteRefs: [{ id: "site:render-draw", role: "call-site", location: { path: "src/renderer.cpp", line: 72 } }] }),
    edge("edge:draw-uses-paint", draw, unresolved, "uses", "uses", { status: "unknown", markers: ["unresolved", "external"], evidenceRefs: ["ev:external-type"] }),
  ];
  return baseResult("symbol", nodes, edges, [], "result:symbol:v1");
}

function entityFixture(): GraphViewResult {
  const renderer = ref("entity", "entity:renderer");
  const ui = ref("entity", "entity:ui");
  const platform = ref("entity", "entity:platform");
  const nodes = [node("entity", renderer.key, "Renderer", "component", { evidenceRefs: ["ev:derived-entity"] }), node("entity", ui.key, "UI", "component", { evidenceRefs: ["ev:derived-entity"] }), node("entity", platform.key, "Platform", "component", { status: "partial", markers: ["inferred"], evidenceRefs: ["ev:derived-entity"] })];
  const edges = [edge("edge:ui-uses-renderer", ui, renderer, "uses", "uses", { status: "partial", markers: ["inferred"], evidenceRefs: ["ev:derived-entity"] }), edge("edge:renderer-uses-platform", renderer, platform, "uses", "uses", { evidenceRefs: ["ev:derived-entity"] })];
  return baseResult("entity", nodes, edges, [group("group:architecture", "Architecture", [renderer, ui, platform])], "result:entity:v1");
}

function includeFixture(): GraphViewResult {
  const app = ref("file", "path:src/app.cpp");
  const renderer = ref("file", "path:src/renderer.hpp");
  const external = ref("file", "external:vendor/graphics.hpp");
  const nodes = [node("file", app.key, "app.cpp", "file", { evidenceRefs: ["ev:render-call-site"] }), node("file", renderer.key, "renderer.hpp", "file", { evidenceRefs: ["ev:render-declaration"] }), node("file", external.key, "graphics.hpp", "file", { status: "unknown", markers: ["external", "stub"], evidenceRefs: ["ev:external-type"] })];
  const edges = [edge("edge:app-includes-renderer", app, renderer, "includes", "includes", { evidenceRefs: ["ev:render-call-site"] }), edge("edge:renderer-includes-external", renderer, external, "includes", "includes", { status: "partial", markers: ["external", "stub"], evidenceRefs: ["ev:external-type"] })];
  return baseResult("include", nodes, edges, [group("group:src", "src/", [app, renderer])], "result:include:v1");
}

function typeFixture(): GraphViewResult {
  const render = ref("symbol", "symbol:renderer::render");
  const canvas = ref("type", "type:Canvas");
  const pointer = ref("type", "type:Canvas*");
  const nodes = [node("symbol", render.key, "render", "function", { evidenceRefs: ["ev:render-declaration"] }), node("type", canvas.key, "Canvas", "record", { evidenceRefs: ["ev:render-declaration"] }), node("type", pointer.key, "Canvas*", "pointer", { status: "complete", evidenceRefs: ["ev:render-declaration"] })];
  const edges = [edge("edge:render-of-type", render, canvas, "of_type", "returns", { evidenceRefs: ["ev:render-declaration"] }), edge("edge:pointer-pointee", pointer, canvas, "pointee", "pointee", { status: "partial", markers: ["stale"], evidenceRefs: ["ev:render-declaration"] })];
  return baseResult("type", nodes, edges, [], "result:type:v1", { freshness: "stale", markers: ["stale"], status: "unknown", completeness: "unknown", diagnostics: [{ code: "stale_input", message: "Fixture intentionally models a stale type fact set.", severity: "warning" }] });
}

export function oversizedFixture(count = 200): GraphViewResult {
  const nodes: GraphNode[] = Array.from({ length: count }, (_, index) => node("symbol", `symbol:oversized::node${String(index).padStart(3, "0")}`, `node-${String(index).padStart(3, "0")}`, "function"));
  const edges: GraphEdge[] = nodes.slice(1).map((current, index) => edge(`edge:oversized:${index}`, nodes[index]!.ref, current.ref, "calls", "calls"));
  return baseResult("symbol", nodes, edges, [], "result:oversized:v1", { budget: { maxNodes: count, maxEdges: count, maxGroups: 0, maxLabelChars: count * 20, maxEvidenceRefs: 0, maxSites: count, maxSiteBytes: count * 256 }, usage: usageOf({ nodes, edges, groups: [], evidence: [] }) });
}

export function boundedFixture(slice: Exclude<GraphKind, "group" | "file">, budget: Budget): GraphViewResult {
  return applyBudget(canonicalFixture(slice), budget);
}
