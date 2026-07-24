import { applyBudget, GRAPH_VIEW_VERSION, usageOf, type Budget, type GraphEdge, type GraphGroup, type GraphKind, type GraphNode, type GraphViewResult, type EvidenceReference, type PortableReference, type SiteReference } from "./graph-view.ts";

const fixtureBudget: Budget = { maxNodes: 32, maxEdges: 48, maxGroups: 8, maxLabelChars: 2_400, maxEvidenceRefs: 64 };
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
];

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
  return baseResult("symbol", nodes, edges, [], "result:oversized:v1", { budget: { maxNodes: count, maxEdges: count, maxGroups: 0, maxLabelChars: count * 20, maxEvidenceRefs: 0 }, usage: usageOf({ nodes, edges, groups: [], evidence: [] }) });
}

export function boundedFixture(slice: Exclude<GraphKind, "group" | "file">, budget: Budget): GraphViewResult {
  return applyBudget(canonicalFixture(slice), budget);
}
