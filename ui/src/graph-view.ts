import type { ResultDiagnostic, ResultEnvelope } from "../../spec/contracts/generated/result-envelope.ts";
import { CORE_CATALOG } from "./generated/catalog.ts";

export const GRAPH_VIEW_VERSION = 1 as const;
const MAX_STRING = 160;
const MAX_EVIDENCE_PER_ELEMENT = 8;
const MAX_DIAGNOSTICS = 8;

export type GraphKind = "symbol" | "entity" | "include" | "type" | "file" | "group" | "edge";
export type GraphStatus = "complete" | "partial" | "unknown" | "error";
export type TruthMarker =
  | "truncated"
  | "stale"
  | "unresolved"
  | "external"
  | "stub"
  | "inferred"
  | "assumed"
  | "refuted"
  | "proved";
export type EvidenceClass = "source" | "derived" | "inferred" | "runtime" | "assumption" | "proof";
export type EvidenceRole = "declaration" | "definition" | "reference" | "call-site" | "type-use" | "derived";
export type LayoutName = "breadthfirst" | "cose" | "circle" | "grid" | "preset";

export interface PortableReference {
  /** A stable semantic key, never a database-local integer. */
  key: string;
  kind: GraphKind;
  semanticUniverse: string;
}

export interface SourceLocation {
  path: string;
  line: number;
  column?: number;
}

export interface SiteReference {
  id: string;
  role: "reference" | "call-site" | "type-use";
  location?: SourceLocation;
}

export interface EvidenceReference {
  id: string;
  class: EvidenceClass;
  role: EvidenceRole;
  summary: string;
  location?: SourceLocation;
  artifact?: string;
}

export interface GraphElementState {
  status: GraphStatus;
  markers: readonly TruthMarker[];
  evidenceRefs: readonly string[];
}

export interface GraphNode extends GraphElementState {
  ref: PortableReference;
  label: string;
  semanticKind: string;
  groupRef?: PortableReference;
  location?: SourceLocation;
}

export interface GraphEdge extends GraphElementState {
  ref: PortableReference;
  source: PortableReference;
  target: PortableReference;
  relation: string;
  label: string;
  siteRefs: readonly SiteReference[];
}

export interface GraphGroup extends GraphElementState {
  ref: PortableReference;
  label: string;
  memberRefs: readonly PortableReference[];
  parentRef?: PortableReference;
}

export interface IdentityDescriptor {
  key: string;
  version: string;
}

export interface Budget {
  maxNodes: number;
  maxEdges: number;
  maxGroups: number;
  maxLabelChars: number;
  maxEvidenceRefs: number;
}

export interface BudgetUsage {
  nodes: number;
  edges: number;
  groups: number;
  labelChars: number;
  evidenceRefs: number;
}

export interface Continuation {
  token: string;
  relation?: string;
  direction?: "in" | "out";
  nextDepth: number;
  remaining: Partial<BudgetUsage>;
}

export interface GraphCapability {
  operation: "expand" | "path" | "evidence";
  relation?: string;
  direction?: "in" | "out";
  maxDepth?: number;
}

export interface GraphViewRequest {
  version: typeof GRAPH_VIEW_VERSION;
  operation: "query" | "expand" | "path";
  query: { cxq?: string; root?: PortableReference; slice: GraphKind };
  presentation?: PresentationState;
  budget: Budget;
}

export interface GraphViewResult {
  version: typeof GRAPH_VIEW_VERSION;
  resultId: string;
  queryIdentity: string;
  workspace: IdentityDescriptor;
  index: IdentityDescriptor;
  factSet: IdentityDescriptor;
  status: GraphStatus;
  markers: readonly TruthMarker[];
  freshness: "fresh" | "stale" | "unknown";
  completeness: "complete" | "partial" | "unknown";
  budget: Budget;
  usage: BudgetUsage;
  nodes: readonly GraphNode[];
  edges: readonly GraphEdge[];
  groups: readonly GraphGroup[];
  evidence: readonly EvidenceReference[];
  capabilities: readonly GraphCapability[];
  diagnostics: readonly Diagnostic[];
  continuation?: Continuation;
}

export type GraphViewEnvelope = ResultEnvelope<GraphViewResult>;

export function toResultEnvelope(result: GraphViewResult): GraphViewEnvelope {
  const exit = exitForStatus(result.status);
  return {
    protocol: "cidx.result/v1",
    operation: "graph-view",
    status: result.status,
    exit_class: exit.exit_class,
    exit_code: exit.exit_code,
    result,
    identity: {
      workspace: result.workspace.key,
      index: result.index.key,
      fact_sets: [result.factSet.key],
      freshness: ({ fresh: "current", stale: "stale", unknown: "unverifiable" } as const)[result.freshness],
      source_revision: null,
      source_fingerprint: null,
    },
    producer: { package: "cidx-graphview-prototype", version: "0.1.0", backend: "fixture", schema_version: 1 },
    completeness: {
      state: result.completeness,
      truncated: result.markers.includes("truncated"),
      stale: result.freshness === "stale",
      budget: result.budget.maxNodes,
    },
    diagnostics: result.diagnostics.map(toProtocolDiagnostic),
    evidence: result.evidence.map((evidence) => {
      const source = evidence.location ? `${evidence.location.path}:${evidence.location.line}${evidence.location.column === undefined ? "" : `:${evidence.location.column}`}` : evidence.artifact;
      return { id: evidence.id, class: evidence.class, trust: evidenceTrust(evidence.class), summary: evidence.summary, ...(source === undefined ? {} : { source }) };
    }),
    artifacts: [{ kind: "graph-view", id: result.resultId, schema_version: result.version, catalog_version: CORE_CATALOG.catalog_version, catalog_hash: "c5479dfc5757e0a8b23b6d0078b164814a73823a750b41631eb818e3733eef48" }],
    replay: { command: "graph-view", arguments: [result.queryIdentity] },
    resources: { elapsed_ms: 0, peak_bytes: utf8ByteLength(JSON.stringify(result)) },
  };
}

function toProtocolDiagnostic(diagnostic: Diagnostic): ResultDiagnostic {
  const next_action = diagnostic.code === "stale_input" ? "refresh the index before relying on this result" : undefined;
  return { code: diagnostic.code, severity: diagnostic.severity, message: diagnostic.message, ...(next_action === undefined ? {} : { next_action }) };
}

function exitForStatus(status: GraphStatus): Pick<GraphViewEnvelope, "exit_class" | "exit_code"> {
  const exits: Record<GraphStatus, Pick<GraphViewEnvelope, "exit_class" | "exit_code">> = {
    complete: { exit_class: "success", exit_code: 0 },
    partial: { exit_class: "success", exit_code: 0 },
    unknown: { exit_class: "unknown", exit_code: 5 },
    error: { exit_class: "infrastructure_failure", exit_code: 6 },
  };
  return exits[status];
}

function evidenceTrust(evidenceClass: EvidenceClass): "unverified" | "producer-verified" | "reader-verified" {
  if (evidenceClass === "inferred" || evidenceClass === "assumption") return "unverified";
  if (evidenceClass === "proof") return "reader-verified";
  return "producer-verified";
}

export function utf8ByteLength(value: string): number {
  return new TextEncoder().encode(value).byteLength;
}

export interface Diagnostic {
  code: "budget_exhausted" | "stale_input" | "unknown_input" | "invalid_fixture";
  message: string;
  severity: "info" | "warning" | "error";
}

export interface PresentationState {
  layout: LayoutName;
  zoom: number;
  pan: { x: number; y: number };
  hiddenKinds: readonly GraphKind[];
  collapsedGroupKeys: readonly string[];
}

export interface SavedView {
  /** Presentation identity is intentionally separate from resultId/queryIdentity. */
  savedViewId: string;
  resultId: string;
  queryIdentity: string;
  presentation: PresentationState;
}

export interface VisualSemantics {
  marker: string;
  pattern: "solid" | "dashed" | "dotted" | "double" | "crossed" | "none";
  glyph: string;
  labelSuffix: string;
  inspectorText: string;
  isCompleteStyle: boolean;
}

export function stablePortableId(ref: PortableReference): string {
  assertPortableReference(ref);
  return `${ref.kind}:${ref.semanticUniverse}:${ref.key}`;
}

export function assertPortableReference(ref: PortableReference): void {
  if (!ref.key || ref.key.length > 240 || /^\d+$/.test(ref.key) || /(?:^|[-_:])(?:row|db|sqlite)[-_:\d]*$/i.test(ref.key)) {
    throw new Error(`portable identity must not be a database-local id: ${ref.key}`);
  }
  if (!ref.semanticUniverse || ref.semanticUniverse.length > 120) {
    throw new Error("portable identity requires a bounded semantic universe");
  }
}

export function visualSemantics(status: GraphStatus, markers: readonly TruthMarker[]): VisualSemantics {
  const table: Record<string, Omit<VisualSemantics, "isCompleteStyle">> = {
    complete: { marker: "complete", pattern: "solid", glyph: "●", labelSuffix: "", inspectorText: "Complete fact" },
    partial: { marker: "partial", pattern: "dashed", glyph: "◐", labelSuffix: " · partial", inspectorText: "Partial coverage; absence is not proof" },
    unknown: { marker: "unknown", pattern: "dotted", glyph: "?", labelSuffix: " · unknown", inspectorText: "Unknown; the result cannot establish truth" },
    error: { marker: "error", pattern: "crossed", glyph: "!", labelSuffix: " · error", inspectorText: "Error; do not interpret as a semantic fact" },
    truncated: { marker: "truncated", pattern: "dashed", glyph: "↯", labelSuffix: " · truncated", inspectorText: "Truncated by a bound; more results may exist" },
    stale: { marker: "stale", pattern: "double", glyph: "◌", labelSuffix: " · stale", inspectorText: "Stale against the expected source revision" },
    unresolved: { marker: "unresolved", pattern: "dotted", glyph: "?", labelSuffix: " · unresolved", inspectorText: "Identity could not be resolved" },
    external: { marker: "external", pattern: "dotted", glyph: "↗", labelSuffix: " · external", inspectorText: "External or stub identity" },
    stub: { marker: "stub", pattern: "dotted", glyph: "□", labelSuffix: " · stub", inspectorText: "Stub with limited local evidence" },
    inferred: { marker: "inferred", pattern: "dashed", glyph: "≈", labelSuffix: " · inferred", inspectorText: "Inferred relation; not a direct source fact" },
    assumed: { marker: "assumed", pattern: "double", glyph: "◇", labelSuffix: " · assumed", inspectorText: "Assumption; not established by indexed evidence" },
    refuted: { marker: "refuted", pattern: "crossed", glyph: "×", labelSuffix: " · refuted", inspectorText: "Refuted claim or relation" },
    proved: { marker: "proved", pattern: "double", glyph: "✓", labelSuffix: " · proved", inspectorText: "Proof-backed result" },
  };
  const precedence: readonly string[] = ["error", "unknown", "refuted", "partial", "truncated", "stale", "unresolved", "assumed", "external", "stub", "inferred", "proved", "complete"];
  const tokens = [...new Set([...(status === "complete" ? [] : [status]), ...markers])]
    .sort((left, right) => precedence.indexOf(left) - precedence.indexOf(right));
  const safeTokens = tokens.length > 0 ? tokens : ["complete"];
  const primary = table[safeTokens[0]!] ?? table.unknown!;
  const cues = safeTokens.map((token) => table[token] ?? table.unknown!);
  return {
    marker: safeTokens.join("+"),
    pattern: primary.pattern,
    glyph: cues.map((cue) => cue.glyph).join(""),
    labelSuffix: safeTokens.length === 1 && safeTokens[0] === "complete" ? "" : ` · ${safeTokens.join(", ")}`,
    inspectorText: cues.map((cue) => cue.inspectorText).join(" "),
    isCompleteStyle: status === "complete" && markers.length === 0,
  };
}

export function validateGraphView(result: GraphViewResult): void {
  if (result.version !== GRAPH_VIEW_VERSION || result.nodes.length > result.budget.maxNodes || result.edges.length > result.budget.maxEdges) {
    throw new Error("GraphView result violates its version or element budget");
  }
  if (result.groups.length > result.budget.maxGroups || result.evidence.length > result.budget.maxEvidenceRefs) {
    throw new Error("GraphView result exceeds its evidence budget");
  }
  if (result.usage.labelChars > result.budget.maxLabelChars || result.usage.nodes > result.budget.maxNodes || result.usage.edges > result.budget.maxEdges || result.usage.groups > result.budget.maxGroups || result.usage.evidenceRefs > result.budget.maxEvidenceRefs) {
    throw new Error("GraphView usage exceeds its declared budget");
  }
  const calculatedUsage = usageOf(result);
  if (JSON.stringify(calculatedUsage) !== JSON.stringify(result.usage)) throw new Error("GraphView usage is not deterministic");
  const nodeKeys = new Set(result.nodes.map((node) => stablePortableId(node.ref)));
  const evidenceIds = new Set(result.evidence.map((reference) => reference.id));
  for (const node of result.nodes) validateElement(node, evidenceIds);
  for (const edge of result.edges) {
    validateElement(edge, evidenceIds);
    if (!nodeKeys.has(stablePortableId(edge.source)) || !nodeKeys.has(stablePortableId(edge.target))) {
      throw new Error(`edge ${edge.ref.key} references a node outside the result`);
    }
  }
  for (const group of result.groups) {
    validateElement(group, evidenceIds);
    if (group.memberRefs.length > result.budget.maxNodes) throw new Error("group membership exceeds the node budget");
  }
  for (const evidence of result.evidence) {
    if (evidence.summary.length > MAX_STRING || evidence.id.length > 120) throw new Error("evidence is not bounded");
  }
}

function validateElement(element: GraphElementState & { ref: PortableReference; label?: string }, evidenceIds: ReadonlySet<string>): void {
  assertPortableReference(element.ref);
  if (element.label && element.label.length > MAX_STRING) throw new Error("element label is not bounded");
  if (element.evidenceRefs.length > MAX_EVIDENCE_PER_ELEMENT) throw new Error("element evidence references are not bounded");
  if (element.evidenceRefs.some((id) => !evidenceIds.has(id))) throw new Error(`element ${element.ref.key} references unknown evidence`);
}

export function usageOf(result: Pick<GraphViewResult, "nodes" | "edges" | "groups" | "evidence">): BudgetUsage {
  return {
    nodes: result.nodes.length,
    edges: result.edges.length,
    groups: result.groups.length,
    labelChars: [...result.nodes, ...result.edges, ...result.groups].reduce((sum, item) => sum + item.label.length, 0),
    evidenceRefs: result.evidence.length,
  };
}

export function applyBudget(result: GraphViewResult, budget: Budget): GraphViewResult {
  const nodes = [...result.nodes].sort((a, b) => stablePortableId(a.ref).localeCompare(stablePortableId(b.ref))).slice(0, budget.maxNodes);
  const nodeKeys = new Set(nodes.map((node) => stablePortableId(node.ref)));
  const edges = [...result.edges]
    .sort((a, b) => stablePortableId(a.ref).localeCompare(stablePortableId(b.ref)))
    .filter((edge) => nodeKeys.has(stablePortableId(edge.source)) && nodeKeys.has(stablePortableId(edge.target)))
    .slice(0, budget.maxEdges);
  const groups = [...result.groups].sort((a, b) => stablePortableId(a.ref).localeCompare(stablePortableId(b.ref))).slice(0, budget.maxGroups);
  const evidenceIds = new Set([...nodes, ...edges, ...groups].flatMap((item) => item.evidenceRefs));
  const evidence = [...result.evidence].filter((item) => evidenceIds.has(item.id)).sort((a, b) => a.id.localeCompare(b.id)).slice(0, budget.maxEvidenceRefs);
  const keptEvidenceIds = new Set(evidence.map((item) => item.id));
  const boundedNodes = nodes.map((item) => ({ ...item, evidenceRefs: item.evidenceRefs.filter((id) => keptEvidenceIds.has(id)) }));
  const boundedEdges = edges.map((item) => ({ ...item, evidenceRefs: item.evidenceRefs.filter((id) => keptEvidenceIds.has(id)) }));
  const boundedGroups = groups.map((item) => ({ ...item, memberRefs: item.memberRefs.filter((member) => nodeKeys.has(stablePortableId(member))), evidenceRefs: item.evidenceRefs.filter((id) => keptEvidenceIds.has(id)) }));
  const labelBound = trimLabels(boundedNodes, boundedEdges, boundedGroups, budget.maxLabelChars);
  const usage = usageOf({ nodes: labelBound.nodes, edges: labelBound.edges, groups: labelBound.groups, evidence });
  const exceeded = nodes.length < result.nodes.length || edges.length < result.edges.length || groups.length < result.groups.length || evidence.length < evidenceIds.size || labelBound.truncated;
  const markers = exceeded ? uniqueMarkers([...result.markers, "truncated"]) : [...result.markers];
  const status = exceeded && result.status === "complete" ? "partial" : result.status;
  const diagnostics = exceeded
    ? [...result.diagnostics, { code: "budget_exhausted" as const, message: "The fixture exceeded a deterministic browser budget; the slice was bounded.", severity: "warning" as const }].slice(0, MAX_DIAGNOSTICS)
    : [...result.diagnostics];
  return { ...result, budget, nodes: labelBound.nodes, edges: labelBound.edges, groups: labelBound.groups, evidence, usage, status, completeness: exceeded ? "partial" : result.completeness, markers, diagnostics };
}

function trimLabels(nodes: readonly GraphNode[], edges: readonly GraphEdge[], groups: readonly GraphGroup[], maxChars: number): { nodes: GraphNode[]; edges: GraphEdge[]; groups: GraphGroup[]; truncated: boolean } {
  let remaining = Math.max(0, maxChars);
  let truncated = false;
  const trim = <T extends { label: string; markers: readonly TruthMarker[] }>(items: readonly T[]): T[] => items.map((item) => {
    const label = item.label.slice(0, remaining);
    remaining -= label.length;
    if (label.length !== item.label.length) {
      truncated = true;
      return { ...item, label, markers: uniqueMarkers([...item.markers, "truncated"]) };
    }
    return item;
  });
  return { nodes: trim(nodes), edges: trim(edges), groups: trim(groups), truncated };
}

function uniqueMarkers(markers: readonly TruthMarker[]): TruthMarker[] {
  return [...new Set(markers)];
}

export function canonicalSemanticContent(result: GraphViewResult): string {
  return JSON.stringify({
    version: result.version,
    queryIdentity: result.queryIdentity,
    status: result.status,
    markers: [...result.markers].sort(),
    nodes: [...result.nodes].map(({ ref, label, semanticKind, status, markers: stateMarkers }) => ({ ref, label, semanticKind, status, markers: [...stateMarkers].sort() })).sort((a, b) => stablePortableId(a.ref).localeCompare(stablePortableId(b.ref))),
    edges: [...result.edges].map(({ ref, source, target, relation, label, status, markers: stateMarkers }) => ({ ref, source, target, relation, label, status, markers: [...stateMarkers].sort() })).sort((a, b) => stablePortableId(a.ref).localeCompare(stablePortableId(b.ref))),
  });
}

export function createSavedView(result: Pick<GraphViewResult, "resultId" | "queryIdentity">, presentation: PresentationState): SavedView {
  return { savedViewId: `saved:${result.resultId}:${presentation.layout}`, resultId: result.resultId, queryIdentity: result.queryIdentity, presentation: structuredClone(presentation) };
}

export function catalogRelationNames(): readonly string[] {
  return CORE_CATALOG.relations.map((relation) => relation.name);
}
