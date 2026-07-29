import { EXIT_CODES, PROTOCOL, type DiagnosticCode, type ResultDiagnostic, type ResultEnvelope } from "./generated/result_protocol.ts";
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

export type OverlayKind = "evidence" | "diff" | "effect" | "proof" | "counterexample";
export type OverlayIdentityState = "added" | "removed" | "changed" | "invalidated" | "unchanged";
export type EffectState = "known" | "unknown" | "conditional";
export type ProofState = "proved" | "open" | "refuted" | "conditional" | "assumed" | "inferred";

/** Provenance supplied by the canonical result producer; the UI only displays it. */
export interface OverlayIdentity {
  resultId: string;
  evidenceId?: string;
  source?: SourceLocation;
  limitation?: string;
}

export interface EvidenceOverlayItem {
  id: string;
  label: string;
  strength: "direct" | "bounded" | "inferred" | "assumed";
  identity: OverlayIdentity;
}

export interface EvidencePathStep {
  sequence: number;
  label: string;
  relation?: string;
  identity: OverlayIdentity;
}

export interface EvidenceOverlay {
  kind: "evidence";
  title: string;
  items: readonly EvidenceOverlayItem[];
  path: readonly EvidencePathStep[];
}

export interface DiffOverlayEntry {
  id: string;
  label: string;
  state: OverlayIdentityState;
  oldRef?: PortableReference;
  newRef?: PortableReference;
  identity: OverlayIdentity;
}

export interface DiffOverlay {
  kind: "diff";
  title: string;
  oldWorkspace: IdentityDescriptor;
  newWorkspace: IdentityDescriptor;
  oldFactSet: IdentityDescriptor;
  newFactSet: IdentityDescriptor;
  entries: readonly DiffOverlayEntry[];
}

export interface EffectOverlayElement {
  id: string;
  label: string;
  state: EffectState;
  identity: OverlayIdentity;
}

export interface EffectOverlay {
  kind: "effect";
  title: string;
  summary: string;
  regions: readonly EffectOverlayElement[];
  callDependencies: readonly EffectOverlayElement[];
  assumptions: readonly EffectOverlayElement[];
  targetCoverage: readonly EffectOverlayElement[];
  unknownBoundaries: readonly EffectOverlayElement[];
}

export interface ProofNode {
  id: string;
  parentId?: string;
  label: string;
  state: ProofState;
  identity: OverlayIdentity;
}

export interface ProofOverlay {
  kind: "proof";
  title: string;
  claims: readonly ProofNode[];
  trustedModels: readonly string[];
  assumptions: readonly string[];
}

export interface CounterexampleBinding {
  name: string;
  value: string;
}

export interface CounterexampleStep {
  index: number;
  action?: string;
  state: readonly CounterexampleBinding[];
  identity: OverlayIdentity;
}

export interface CounterexampleOverlay {
  kind: "counterexample";
  title: string;
  specification: string;
  violation: string;
  steps: readonly CounterexampleStep[];
}

export type Overlay = EvidenceOverlay | DiffOverlay | EffectOverlay | ProofOverlay | CounterexampleOverlay;

export interface OverlayBundle {
  evidence?: EvidenceOverlay;
  diff?: DiffOverlay;
  effect?: EffectOverlay;
  proof?: ProofOverlay;
  counterexample?: CounterexampleOverlay;
  exportMaxBytes: number;
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
  maxSites: number;
  maxSiteBytes: number;
}

export interface BudgetUsage {
  nodes: number;
  edges: number;
  groups: number;
  labelChars: number;
  evidenceRefs: number;
  sites: number;
  siteBytes: number;
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
  overlays?: OverlayBundle;
  continuation?: Continuation;
}

export type GraphViewEnvelope = ResultEnvelope<GraphViewResult>;

export function toResultEnvelope(result: GraphViewResult): GraphViewEnvelope {
  const exit = exitForStatus(result.status);
  return {
    protocol: PROTOCOL,
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
      source_revision: result.index.version,
      source_fingerprint: `fixture-source:${result.index.key}:${result.factSet.key}`,
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
    artifacts: [{ kind: "query-result", id: result.resultId, schema_version: result.version, catalog_version: CORE_CATALOG.catalog_version, catalog_hash: CORE_CATALOG.catalog_hash }],
    replay: { command: "graph-view", arguments: [result.queryIdentity] },
    resources: { elapsed_ms: 0, peak_bytes: null },
  };
}

function toProtocolDiagnostic(diagnostic: Diagnostic): ResultDiagnostic {
  const next_action = diagnostic.code === "stale_input" ? "refresh the index before relying on this result" : undefined;
  return { code: diagnostic.code, severity: diagnostic.severity, message: diagnostic.message, ...(next_action === undefined ? {} : { next_action }) };
}

function exitForStatus(status: GraphStatus): Pick<GraphViewEnvelope, "exit_class" | "exit_code"> {
  const exits: Record<GraphStatus, Pick<GraphViewEnvelope, "exit_class" | "exit_code">> = {
    complete: { exit_class: "success", exit_code: EXIT_CODES.success },
    partial: { exit_class: "success", exit_code: EXIT_CODES.success },
    unknown: { exit_class: "unknown", exit_code: EXIT_CODES.unknown },
    error: { exit_class: "infrastructure_failure", exit_code: EXIT_CODES.infrastructure_failure },
  };
  return exits[status];
}

function evidenceTrust(evidenceClass: EvidenceClass): "unverified" | "producer-verified" | "reader-verified" {
  if (evidenceClass === "inferred" || evidenceClass === "assumption" || evidenceClass === "proof") return "unverified";
  return "producer-verified";
}

export function utf8ByteLength(value: string): number {
  return new TextEncoder().encode(value).byteLength;
}

export interface Diagnostic {
  code: DiagnosticCode;
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
  return [ref.kind, ref.semanticUniverse, ref.key].map(encodePortableSegment).join("");
}

function encodePortableSegment(value: string): string {
  return `${value.length}:${value}`;
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
  if (result.usage.labelChars > result.budget.maxLabelChars || result.usage.nodes > result.budget.maxNodes || result.usage.edges > result.budget.maxEdges || result.usage.groups > result.budget.maxGroups || result.usage.evidenceRefs > result.budget.maxEvidenceRefs || result.usage.sites > result.budget.maxSites || result.usage.siteBytes > result.budget.maxSiteBytes) {
    throw new Error("GraphView usage exceeds its declared budget");
  }
  const calculatedUsage = usageOf(result);
  if (JSON.stringify(calculatedUsage) !== JSON.stringify(result.usage)) throw new Error("GraphView usage is not deterministic");
  uniqueElementIds([...result.nodes, ...result.edges, ...result.groups], "elements");
  const nodeKeys = new Set(result.nodes.map((node) => stablePortableId(node.ref)));
  const evidenceIds = new Set(result.evidence.map((reference) => reference.id));
  for (const node of result.nodes) validateElement(node, evidenceIds);
  for (const edge of result.edges) {
    validateElement(edge, evidenceIds);
    validateSites(edge.siteRefs);
    if (!nodeKeys.has(stablePortableId(edge.source)) || !nodeKeys.has(stablePortableId(edge.target))) {
      throw new Error(`edge ${edge.ref.key} references a node outside the result`);
    }
  }
  for (const group of result.groups) {
    validateElement(group, evidenceIds);
    if (group.memberRefs.length > result.budget.maxNodes) throw new Error("group membership exceeds the node budget");
  }
  for (const evidence of result.evidence) {
    if (utf8ByteLength(evidence.summary) > MAX_STRING || utf8ByteLength(evidence.id) > 120) throw new Error("evidence is not bounded");
  }
  if (result.overlays) validateOverlayBundle(result.overlays, result.resultId, evidenceIds);
}

function validateElement(element: GraphElementState & { ref: PortableReference; label?: string }, evidenceIds: ReadonlySet<string>): void {
  assertPortableReference(element.ref);
  if (element.label && utf8ByteLength(element.label) > MAX_STRING) throw new Error("element label is not bounded");
  if (element.evidenceRefs.length > MAX_EVIDENCE_PER_ELEMENT) throw new Error("element evidence references are not bounded");
  if (element.evidenceRefs.some((id) => !evidenceIds.has(id))) throw new Error(`element ${element.ref.key} references unknown evidence`);
}

function uniqueElementIds(elements: readonly { ref: PortableReference }[], label: string): Set<string> {
  const ids = new Set(elements.map((element) => stablePortableId(element.ref)));
  if (ids.size !== elements.length) throw new Error(`${label} contain duplicate portable identities`);
  return ids;
}

function validateSites(sites: readonly SiteReference[]): void {
  const ids = new Set<string>();
  for (const site of sites) {
    if (!site.id || site.id.length > 120 || ids.has(site.id)) throw new Error("edge sites must have unique bounded ids");
    ids.add(site.id);
    if (site.location && (!site.location.path || site.location.path.length > 512 || site.location.line < 1 || (site.location.column !== undefined && site.location.column < 1))) {
      throw new Error("edge site location is invalid");
    }
  }
}

function validateOverlayIdentity(identity: OverlayIdentity, resultId: string, evidenceIds: ReadonlySet<string>): void {
  if (!identity.resultId || identity.resultId.length > 240 || identity.resultId !== resultId) throw new Error("overlay identity must point to the canonical result");
  if (identity.evidenceId !== undefined && (!evidenceIds.has(identity.evidenceId) || identity.evidenceId.length > 120)) throw new Error("overlay identity references unknown evidence");
  if (identity.source === undefined && (!identity.limitation || identity.limitation.length > MAX_STRING)) throw new Error("overlay identity requires source provenance or a named limitation");
  if (identity.source && (!identity.source.path || identity.source.path.length > 512 || identity.source.line < 1 || (identity.source.column !== undefined && identity.source.column < 1))) throw new Error("overlay source provenance is invalid");
}

function validateOverlayElements(elements: readonly { id: string; identity: OverlayIdentity }[], resultId: string, evidenceIds: ReadonlySet<string>): void {
  const ids = new Set<string>();
  for (const element of elements) {
    if (!element.id || element.id.length > 120 || ids.has(element.id)) throw new Error("overlay elements must have unique bounded ids");
    ids.add(element.id);
    validateOverlayIdentity(element.identity, resultId, evidenceIds);
  }
}

export function validateOverlayBundle(bundle: OverlayBundle, resultId: string, evidenceIds: ReadonlySet<string>): void {
  if (!Number.isInteger(bundle.exportMaxBytes) || bundle.exportMaxBytes < 512 || bundle.exportMaxBytes > 1_000_000) throw new Error("overlay export budget is invalid");
  if (bundle.evidence) {
    validateOverlayElements([...bundle.evidence.items, ...bundle.evidence.path.map((step) => ({ id: `path:${step.sequence}`, identity: step.identity }))], resultId, evidenceIds);
    if (bundle.evidence.path.some((step, index) => step.sequence !== index)) throw new Error("evidence path must have deterministic contiguous steps");
  }
  if (bundle.diff) {
    validateOverlayElements(bundle.diff.entries, resultId, evidenceIds);
    if (bundle.diff.entries.some((entry) => entry.state === "added" ? entry.newRef === undefined : entry.state === "removed" ? entry.oldRef === undefined : entry.oldRef === undefined || entry.newRef === undefined)) throw new Error("diff entries must preserve their old/new identity");
  }
  if (bundle.effect) validateOverlayElements([...bundle.effect.regions, ...bundle.effect.callDependencies, ...bundle.effect.assumptions, ...bundle.effect.targetCoverage, ...bundle.effect.unknownBoundaries], resultId, evidenceIds);
  if (bundle.proof) {
    validateOverlayElements(bundle.proof.claims, resultId, evidenceIds);
    const claimIds = new Set(bundle.proof.claims.map((claim) => claim.id));
    if (bundle.proof.claims.some((claim) => claim.parentId !== undefined && (claim.parentId === claim.id || !claimIds.has(claim.parentId)))) throw new Error("proof claims must reference an existing parent");
    const visiting = new Set<string>();
    const visited = new Set<string>();
    const hasCycle = (id: string): boolean => {
      if (visiting.has(id)) return true;
      if (visited.has(id)) return false;
      visiting.add(id);
      const parentId = bundle.proof!.claims.find((claim) => claim.id === id)?.parentId;
      const cycle = parentId !== undefined && hasCycle(parentId);
      visiting.delete(id);
      visited.add(id);
      return cycle;
    };
    if (bundle.proof.claims.some((claim) => hasCycle(claim.id))) throw new Error("proof claims must form an acyclic tree");
  }
  if (bundle.counterexample) {
    if (bundle.counterexample.steps.length === 0) throw new Error("counterexample must contain at least one bounded step");
    validateOverlayElements(bundle.counterexample.steps.map((step) => ({ id: `step:${step.index}`, identity: step.identity })), resultId, evidenceIds);
    if (bundle.counterexample.steps.some((step, index) => step.index !== index || step.state.length > 16 || step.state.some((binding) => !binding.name || binding.name.length > 80 || binding.value.length > MAX_STRING))) throw new Error("counterexample steps must be bounded and ordered");
  }
}

export function exportOverlaySnapshot(overlay: Overlay, maxBytes: number): string {
  if (!Number.isInteger(maxBytes) || maxBytes < 512) throw new Error("overlay export budget is invalid");
  const snapshot: Record<string, unknown> = { version: 1, bounded: true, truncated: false, overlay: structuredClone(overlay) };
  const collectionPaths: string[][] = overlay.kind === "evidence" ? [["overlay", "items"], ["overlay", "path"]] : overlay.kind === "diff" ? [["overlay", "entries"]] : overlay.kind === "effect" ? [["overlay", "regions"], ["overlay", "callDependencies"], ["overlay", "assumptions"], ["overlay", "targetCoverage"], ["overlay", "unknownBoundaries"]] : overlay.kind === "proof" ? [["overlay", "claims"]] : [["overlay", "steps"]];
  const read = (path: readonly string[]): unknown => path.reduce<unknown>((value, key) => (value as Record<string, unknown>)[key], snapshot);
  const byteLength = () => utf8ByteLength(JSON.stringify(snapshot));
  let collectionIndex = 0;
  while (byteLength() > maxBytes && collectionIndex < collectionPaths.length) {
    const collection = read(collectionPaths[collectionIndex]!) as unknown[];
    if (collection.length === 0) {
      collectionIndex += 1;
      continue;
    }
    collection.pop();
    snapshot.truncated = true;
    if (collection.length === 0) collectionIndex += 1;
  }
  if (byteLength() > maxBytes) throw new Error("overlay cannot fit its bounded export snapshot");
  return JSON.stringify(snapshot);
}

function overlayEvidenceIds(bundle: OverlayBundle | undefined): readonly string[] {
  if (!bundle) return [];
  const identities: OverlayIdentity[] = [];
  if (bundle.evidence) identities.push(...bundle.evidence.items.map((item) => item.identity), ...bundle.evidence.path.map((step) => step.identity));
  if (bundle.diff) identities.push(...bundle.diff.entries.map((entry) => entry.identity));
  if (bundle.effect) identities.push(...[...bundle.effect.regions, ...bundle.effect.callDependencies, ...bundle.effect.assumptions, ...bundle.effect.targetCoverage, ...bundle.effect.unknownBoundaries].map((item) => item.identity));
  if (bundle.proof) identities.push(...bundle.proof.claims.map((claim) => claim.identity));
  if (bundle.counterexample) identities.push(...bundle.counterexample.steps.map((step) => step.identity));
  return identities.flatMap((identity) => identity.evidenceId === undefined ? [] : [identity.evidenceId]);
}

function canonicalSite(site: SiteReference): string {
  return JSON.stringify({
    id: site.id,
    role: site.role,
    ...(site.location === undefined ? {} : { location: { path: site.location.path, line: site.location.line, ...(site.location.column === undefined ? {} : { column: site.location.column }) } }),
  });
}

export function siteByteLength(site: SiteReference): number {
  return utf8ByteLength(canonicalSite(site));
}

export function usageOf(result: Pick<GraphViewResult, "nodes" | "edges" | "groups" | "evidence">): BudgetUsage {
  const sites = result.edges.flatMap((edge) => edge.siteRefs);
  return {
    nodes: result.nodes.length,
    edges: result.edges.length,
    groups: result.groups.length,
    labelChars: [...result.nodes, ...result.edges, ...result.groups].reduce((sum, item) => sum + item.label.length, 0),
    evidenceRefs: result.evidence.length,
    sites: sites.length,
    siteBytes: sites.reduce((sum, site) => sum + siteByteLength(site), 0),
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
  const overlayIds = new Set(overlayEvidenceIds(result.overlays));
  if (overlayIds.size > budget.maxEvidenceRefs) throw new Error("overlay evidence exceeds the declared evidence budget");
  const evidenceIds = new Set([...nodes, ...edges, ...groups].flatMap((item) => item.evidenceRefs));
  overlayIds.forEach((id) => evidenceIds.add(id));
  const evidence = [...result.evidence].filter((item) => evidenceIds.has(item.id)).sort((a, b) => Number(overlayIds.has(b.id)) - Number(overlayIds.has(a.id)) || a.id.localeCompare(b.id)).slice(0, budget.maxEvidenceRefs);
  const keptEvidenceIds = new Set(evidence.map((item) => item.id));
  const boundedNodes = nodes.map((item) => ({ ...item, evidenceRefs: item.evidenceRefs.filter((id) => keptEvidenceIds.has(id)) }));
  const boundedEdges = edges.map((item) => ({ ...item, evidenceRefs: item.evidenceRefs.filter((id) => keptEvidenceIds.has(id)) }));
  const boundedGroups = groups.map((item) => ({ ...item, memberRefs: item.memberRefs.filter((member) => nodeKeys.has(stablePortableId(member))), evidenceRefs: item.evidenceRefs.filter((id) => keptEvidenceIds.has(id)) }));
  const labelBound = trimLabels(boundedNodes, boundedEdges, boundedGroups, budget.maxLabelChars);
  const siteBound = trimSites(labelBound.edges, budget);
  const usage = usageOf({ nodes: labelBound.nodes, edges: siteBound.edges, groups: labelBound.groups, evidence });
  const exceeded = nodes.length < result.nodes.length || edges.length < result.edges.length || groups.length < result.groups.length || evidence.length < evidenceIds.size || labelBound.truncated || siteBound.truncated;
  const markers = exceeded ? uniqueMarkers([...result.markers, "truncated"]) : [...result.markers];
  const status = exceeded && result.status === "complete" ? "partial" : result.status;
  const diagnostics = exceeded
    ? [...result.diagnostics, { code: "truncated_budget" as const, message: siteBound.truncated ? "The graph exceeded a deterministic browser site/byte budget; sites were bounded and a continuation is available." : "The fixture exceeded a deterministic browser budget; the slice was bounded.", severity: "warning" as const }].slice(0, MAX_DIAGNOSTICS)
    : [...result.diagnostics];
  const continuation = exceeded ? budgetContinuation(result, usage) : result.continuation;
  return { ...result, budget, nodes: labelBound.nodes, edges: siteBound.edges, groups: labelBound.groups, evidence, usage, status, completeness: exceeded ? "partial" : result.completeness, markers, diagnostics, ...(continuation === undefined ? {} : { continuation }) };
}

function trimSites(edges: readonly GraphEdge[], budget: Budget): { edges: GraphEdge[]; truncated: boolean } {
  let sites = 0;
  let siteBytes = 0;
  let truncated = false;
  const boundedEdges = edges.map((edge) => {
    let edgeTruncated = false;
    const boundedSites = [...edge.siteRefs].sort((left, right) => left.id.localeCompare(right.id)).filter((site) => {
      const bytes = siteByteLength(site);
      if (sites >= budget.maxSites || siteBytes + bytes > budget.maxSiteBytes) {
        truncated = true;
        edgeTruncated = true;
        return false;
      }
      sites += 1;
      siteBytes += bytes;
      return true;
    });
    return edgeTruncated ? { ...edge, siteRefs: boundedSites, markers: uniqueMarkers([...edge.markers, "truncated"]) } : { ...edge, siteRefs: boundedSites };
  });
  return { edges: boundedEdges, truncated };
}

function budgetContinuation(result: GraphViewResult, usage: BudgetUsage): Continuation {
  const remaining = {
    ...result.continuation?.remaining,
    nodes: Math.max(0, result.nodes.length - usage.nodes),
    edges: Math.max(0, result.edges.length - usage.edges),
    groups: Math.max(0, result.groups.length - usage.groups),
    sites: Math.max(0, usageOf(result).sites - usage.sites),
    siteBytes: Math.max(0, usageOf(result).siteBytes - usage.siteBytes),
  };
  return {
    ...result.continuation,
    token: result.continuation?.token ?? `budget:${result.resultId}`.slice(0, 240),
    nextDepth: result.continuation?.nextDepth ?? 0,
    remaining,
  };
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
    edges: [...result.edges].map(({ ref, source, target, relation, label, status, markers: stateMarkers, siteRefs }) => ({ ref, source, target, relation, label, status, markers: [...stateMarkers].sort(), siteRefs: [...siteRefs].sort((left, right) => left.id.localeCompare(right.id)) })).sort((a, b) => stablePortableId(a.ref).localeCompare(stablePortableId(b.ref))),
    overlays: result.overlays,
  });
}

export function createSavedView(result: Pick<GraphViewResult, "resultId" | "queryIdentity">, presentation: PresentationState): SavedView {
  return { savedViewId: `saved:${result.resultId}:${presentation.layout}`, resultId: result.resultId, queryIdentity: result.queryIdentity, presentation: structuredClone(presentation) };
}

export function catalogRelationNames(): readonly string[] {
  return CORE_CATALOG.relations.map((relation) => relation.name);
}
