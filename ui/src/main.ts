import cytoscape, { type Core } from "cytoscape";
import { toCytoscapeElements } from "./cytoscape-adapter.ts";
import { boundedFixture, canonicalFixture, overlayFixture, type OverlayFixture } from "./fixtures.ts";
import { applyBudget, exportOverlaySnapshot, visualSemantics, type GraphKind, type GraphViewResult, type LayoutName, type Overlay, type OverlayIdentity } from "./graph-view.ts";
import "./styles.css";

const graphElement = document.querySelector<HTMLDivElement>("#graph")!;
const inspectorElement = document.querySelector<HTMLDivElement>("#inspector")!;
const budgetElement = document.querySelector<HTMLDivElement>("#budget-status")!;
const statusElement = document.querySelector<HTMLDivElement>("#canvas-status")!;
const historyElement = document.querySelector<HTMLOListElement>("#history-list")!;
const historyCountElement = document.querySelector<HTMLSpanElement>("#history-count")!;
const fixtureSelect = document.querySelector<HTMLSelectElement>("#fixture-select")!;
const layoutSelect = document.querySelector<HTMLSelectElement>("#layout-select")!;
const queryInput = document.querySelector<HTMLInputElement>("#query-input")!;
const showGroupsInput = document.querySelector<HTMLInputElement>("#show-groups")!;
const showEvidenceInput = document.querySelector<HTMLInputElement>("#show-evidence")!;
const resetButton = document.querySelector<HTMLButtonElement>("#reset-button")!;
const overlayElement = document.querySelector<HTMLDivElement>("#overlay-panel")!;
const exportOverlayButton = document.querySelector<HTMLButtonElement>("#export-overlay")!;

if (!graphElement || !inspectorElement || !budgetElement || !statusElement || !historyElement || !historyCountElement || !fixtureSelect || !layoutSelect || !queryInput || !showGroupsInput || !showEvidenceInput || !resetButton || !overlayElement || !exportOverlayButton) {
  throw new Error("GraphView prototype markup is incomplete");
}

let cy: Core | undefined;
let currentResult: GraphViewResult = boundedFixture("symbol", browserBudget());
let currentLayout: LayoutName = "breadthfirst";
let history: string[] = [];
let counterexampleStep = 0;

const visualTruthLegend = [
  ["complete", "●", "solid", "Complete source or catalog fact"],
  ["partial", "◐", "dashed", "Coverage is incomplete; absence is not proof"],
  ["unknown", "?", "dotted", "The result cannot establish truth"],
  ["stale", "◌", "double", "Fact set is stale against the expected revision"],
  ["external", "↗", "dotted", "External/stub identity with bounded local evidence"],
  ["inferred", "≈", "dashed", "Derived or inferred relation"],
] as const;

function browserBudget() {
  return { maxNodes: 32, maxEdges: 48, maxGroups: 8, maxLabelChars: 2_400, maxEvidenceRefs: 64, maxSites: 256, maxSiteBytes: 32_768 };
}

function render(): void {
  const includeGroups = showGroupsInput.checked;
  const selectedFixture = fixtureSelect.value;
  const overlayFixtures: readonly OverlayFixture[] = ["evidence", "diff", "effect", "proof", "counterexample"];
  currentResult = overlayFixtures.includes(selectedFixture as OverlayFixture)
    ? overlayFixture(selectedFixture as OverlayFixture)
    : applyBudget(canonicalFixture(selectedFixture as Exclude<GraphKind, "group" | "file">), browserBudget());
  counterexampleStep = 0;
  if (cy) cy.destroy();
  cy = cytoscape({
    container: graphElement,
    elements: [...toCytoscapeElements(currentResult, { includeGroups })],
    style: graphStyle(showEvidenceInput.checked),
    layout: { name: currentLayout, fit: true, padding: 42, animate: false },
    minZoom: 0.25,
    maxZoom: 3,
    wheelSensitivity: 0.18,
  });
  cy.on("select", "node, edge", (event) => inspect(event.target.data() as Record<string, unknown>));
  cy.on("unselect", "node, edge", () => {
    if (!cy?.elements(":selected").length) showEmptyInspector();
  });
  updateStatus();
  renderOverlay();
}

function overlayIdentityText(identity: OverlayIdentity): string {
  const provenance = identity.source ? `${identity.source.path}:${identity.source.line}${identity.source.column === undefined ? "" : `:${identity.source.column}`}` : `limitation: ${identity.limitation}`;
  return `result ${identity.resultId} · ${identity.evidenceId ?? "no evidence id"} · ${provenance}`;
}

function overlayState(state: string): string {
  const cues: Record<string, string> = { added: "+", removed: "−", changed: "↔", invalidated: "!", unchanged: "=", known: "●", unknown: "?", conditional: "◇", proved: "✓", open: "○", refuted: "×", assumed: "◇", inferred: "≈" };
  return `${cues[state] ?? "·"} ${state}`;
}

function overlayItem(item: { id: string; label: string; identity: OverlayIdentity; state?: string; strength?: string }): string {
  const state = item.state ?? item.strength;
  return `<li><div class="overlay-item-title"><strong>${escapeHtml(item.label)}</strong>${state ? `<span class="overlay-state">${escapeHtml(overlayState(state))}</span>` : ""}</div><code>${escapeHtml(overlayIdentityText(item.identity))}</code></li>`;
}

function renderOverlay(): void {
  const selected = fixtureSelect.value as OverlayFixture;
  const overlay: Overlay | undefined = currentResult.overlays?.[selected];
  exportOverlayButton.disabled = !overlay;
  if (!overlay) {
    overlayElement.innerHTML = '<p class="muted">Choose an evidence, diff, effect, proof, or TLA+ fixture.</p>';
    return;
  }
  const heading = `<p class="overlay-title"><strong>${escapeHtml(overlay.title)}</strong><span class="overlay-kind">${escapeHtml(overlay.kind)}</span></p>`;
  if (overlay.kind === "evidence") {
    overlayElement.innerHTML = `${heading}<p class="muted">Typed evidence and a bounded explanation path; no frontend inference is added.</p><h3>Evidence</h3><ul class="overlay-list">${overlay.items.map((item) => overlayItem(item)).join("")}</ul><h3>Path</h3><ol class="overlay-list">${overlay.path.map((step) => `<li><div class="overlay-item-title"><strong>${escapeHtml(step.label)}</strong><span class="overlay-state">${escapeHtml(step.relation ?? "step")}</span></div><code>${escapeHtml(overlayIdentityText(step.identity))}</code></li>`).join("")}</ol>`;
  } else if (overlay.kind === "diff") {
    overlayElement.innerHTML = `${heading}<p class="overlay-identities"><code>old ${escapeHtml(overlay.oldWorkspace.version)} / ${escapeHtml(overlay.oldFactSet.version)}</code><br><code>new ${escapeHtml(overlay.newWorkspace.version)} / ${escapeHtml(overlay.newFactSet.version)}</code></p><ul class="overlay-list">${overlay.entries.map((entry) => overlayItem(entry)).join("")}</ul>`;
  } else if (overlay.kind === "effect") {
    const section = (label: string, items: readonly { id: string; label: string; state: string; identity: OverlayIdentity }[]) => `<h3>${escapeHtml(label)}</h3><ul class="overlay-list">${items.map((item) => overlayItem(item)).join("") || '<li class="muted">None declared.</li>'}</ul>`;
    overlayElement.innerHTML = `${heading}<p>${escapeHtml(overlay.summary)}</p>${section("Abstract regions", overlay.regions)}${section("Call dependencies", overlay.callDependencies)}${section("Assumptions", overlay.assumptions)}${section("Target coverage", overlay.targetCoverage)}${section("Unknown boundaries", overlay.unknownBoundaries)}`;
  } else if (overlay.kind === "proof") {
    const depthOf = (id: string): number => { const parent = overlay.claims.find((claim) => claim.id === id)?.parentId; return parent === undefined ? 0 : depthOf(parent) + 1; };
    overlayElement.innerHTML = `${heading}<p class="muted">Trusted models: ${escapeHtml(overlay.trustedModels.join(", ") || "none")}</p><ul class="overlay-list proof-list">${overlay.claims.map((claim) => `<li style="margin-left:${depthOf(claim.id) * 12}px">${overlayItem(claim)}</li>`).join("")}</ul><p class="muted">Assumptions: ${escapeHtml(overlay.assumptions.join("; ") || "none")}</p>`;
  } else {
    const step = overlay.steps[Math.min(counterexampleStep, overlay.steps.length - 1)]!;
    overlayElement.innerHTML = `${heading}<p><strong>${escapeHtml(overlay.specification)}</strong>: ${escapeHtml(overlay.violation)}</p><div class="trace-controls"><button id="trace-previous" type="button" ${counterexampleStep === 0 ? "disabled" : ""}>Previous</button><span>Step ${step.index + 1} / ${overlay.steps.length}</span><button id="trace-next" type="button" ${counterexampleStep >= overlay.steps.length - 1 ? "disabled" : ""}>Next</button></div><dl class="trace-state">${step.state.map((binding) => `<dt>${escapeHtml(binding.name)}</dt><dd><code>${escapeHtml(binding.value)}</code></dd>`).join("")}</dl><p class="muted">Action: ${escapeHtml(step.action ?? "state observation")}</p><p><code>${escapeHtml(overlayIdentityText(step.identity))}</code></p>`;
    overlayElement.querySelector<HTMLButtonElement>("#trace-previous")?.addEventListener("click", () => { counterexampleStep -= 1; renderOverlay(); });
    overlayElement.querySelector<HTMLButtonElement>("#trace-next")?.addEventListener("click", () => { counterexampleStep += 1; renderOverlay(); });
  }
}

function graphStyle(showEvidence: boolean): cytoscape.StylesheetJson {
  return [
    { selector: "node", style: { label: "data(label)", "text-wrap": "wrap", "text-max-width": "150px", "font-size": "12px", color: "#e8edf2", "text-valign": "center", "text-halign": "center", "background-color": "#315b76", "border-width": "2px", "border-color": "#87b9d7", width: "54px", height: "54px", padding: "8px" } },
    { selector: "node[kind = 'group']", style: { label: "data(label)", "background-color": "#182735", "border-color": "#486274", "border-style": "dashed", "text-valign": "top", "text-halign": "center", "font-size": "11px", "padding": "18px", "compound-sizing-wrt-labels": "exclude" } },
    { selector: "edge", style: { label: showEvidence ? "data(label)" : "", width: "2px", "line-color": "#69889b", "target-arrow-color": "#69889b", "target-arrow-shape": "triangle", "curve-style": "bezier", "font-size": "10px", color: "#aebbc4", "text-background-color": "#101820", "text-background-opacity": 0.88, "text-rotation": "autorotate" } },
    { selector: "[status = 'partial']", style: { "border-style": "dashed", "line-style": "dashed", "background-color": "#735e31", "line-color": "#d0a755", "target-arrow-color": "#d0a755" } },
    { selector: "[status = 'unknown']", style: { "border-style": "dotted", "line-style": "dotted", "background-color": "#4c5660", "line-color": "#a8b0b8", "target-arrow-color": "#a8b0b8" } },
    { selector: "[status = 'error']", style: { "border-style": "double", "line-style": "dashed", "background-color": "#6d3540", "line-color": "#e17b8a", "target-arrow-color": "#e17b8a" } },
    { selector: "[markers @= 'stale']", style: { "border-style": "double", "line-style": "dashed" } },
    { selector: "[markers @= 'external'], [markers @= 'stub'], [markers @= 'unresolved']", style: { "border-style": "dotted", "line-style": "dotted" } },
    { selector: "[markers @= 'refuted']", style: { "border-style": "double", "line-style": "dashed", "line-color": "#e17b8a" } },
    { selector: ":selected", style: { "border-width": 4, "border-color": "#f4c95d", "line-color": "#f4c95d", "target-arrow-color": "#f4c95d", "z-index": 20 } },
  ];
}

function inspect(data: Record<string, unknown>): void {
  const status = typeof data.status === "string" ? data.status as GraphViewResult["status"] : "unknown";
  const markers = Array.isArray(data.markers) ? data.markers.filter((item): item is string => typeof item === "string") : [];
  const semantics = visualSemantics(status, markers as never);
  const evidenceIds = Array.isArray(data.evidenceRefs) ? data.evidenceRefs.filter((item): item is string => typeof item === "string") : [];
  const evidence = currentResult.evidence.filter((item) => evidenceIds.includes(item.id));
  const sourcePath = typeof data.sourcePath === "string" ? data.sourcePath : undefined;
  const sourceLine = typeof data.sourceLine === "number" ? data.sourceLine : undefined;
  inspectorElement.innerHTML = `
    <div class="selection-title"><span class="truth-glyph">${semantics.glyph}</span><strong>${escapeHtml(String(data.label ?? "unnamed"))}</strong></div>
    <dl class="inspector-list">
      <dt>Portable identity</dt><dd><code>${escapeHtml(String(data.portableKey ?? "—"))}</code></dd>
      <dt>Kind</dt><dd>${escapeHtml(String(data.kind ?? "—"))} / ${escapeHtml(String(data.semanticKind ?? data.relation ?? "—"))}</dd>
      <dt>Status</dt><dd><span class="status-chip ${status}">${escapeHtml(status)}${markers.length ? ` · ${escapeHtml(markers.join(", "))}` : ""}</span></dd>
      <dt>Truth cue</dt><dd>${escapeHtml(semantics.inspectorText)}</dd>
    </dl>
    ${sourcePath ? `<div class="evidence-action"><button type="button" data-source="${escapeHtml(sourcePath)}:${sourceLine ?? 1}">Copy source location</button><code>${escapeHtml(sourcePath)}:${sourceLine ?? 1}</code></div>` : ""}
    <h3>Evidence (${evidence.length})</h3>
    <ul class="evidence-list">${evidence.length ? evidence.map((item) => `<li><span class="evidence-class">${escapeHtml(item.class)}</span> ${escapeHtml(item.summary)}${item.location ? ` <code>${escapeHtml(item.location.path)}:${item.location.line}</code>` : ""}</li>`).join("") : "<li class=\"muted\">No evidence reference attached.</li>"}</ul>
  `;
  inspectorElement.querySelector<HTMLButtonElement>("[data-source]")?.addEventListener("click", async (event) => {
    const target = event.currentTarget as HTMLButtonElement;
    await navigator.clipboard?.writeText(target.dataset.source ?? "");
    target.textContent = "Copied source location";
  });
}

function showEmptyInspector(): void {
  inspectorElement.innerHTML = '<p class="muted">Select a node or edge to inspect its portable identity, status, and evidence.</p>';
}

function updateStatus(): void {
  const { usage, budget } = currentResult;
  const state = currentResult.markers.length ? currentResult.markers.join(" · ") : currentResult.status;
  statusElement.textContent = `${currentResult.nodes.length} nodes · ${currentResult.edges.length} edges · ${state}`;
  budgetElement.innerHTML = `<div class="budget-meter"><span style="width:${Math.min(100, (usage.nodes / budget.maxNodes) * 100)}%"></span></div><p><strong>${usage.nodes}/${budget.maxNodes}</strong> nodes · <strong>${usage.edges}/${budget.maxEdges}</strong> edges</p><p>${escapeHtml(currentResult.diagnostics[0]?.message ?? "Within browser budget")}</p>`;
}

function updateHistory(label: string): void {
  history = [label, ...history.filter((item) => item !== label)].slice(0, 6);
  historyCountElement.textContent = String(history.length);
  historyElement.innerHTML = history.map((item) => `<li><button type="button" class="history-button">${escapeHtml(item)}</button></li>`).join("");
}

function escapeHtml(value: string): string {
  return value.replace(/[&<>"']/g, (character) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#039;" })[character] ?? character);
}

for (const [name, glyph, pattern, description] of visualTruthLegend) {
  const item = document.createElement("div");
  item.className = "legend-item";
  item.innerHTML = `<span class="legend-glyph ${pattern}">${glyph}</span><div><strong>${name}</strong><span>${description}</span></div>`;
  document.querySelector("#legend")?.append(item);
}

fixtureSelect.addEventListener("change", () => { updateHistory(fixtureSelect.options[fixtureSelect.selectedIndex]?.text ?? "Fixture"); render(); });
layoutSelect.addEventListener("change", () => { currentLayout = layoutSelect.value as LayoutName; render(); });
showGroupsInput.addEventListener("change", render);
showEvidenceInput.addEventListener("change", render);
resetButton.addEventListener("click", () => { currentLayout = "breadthfirst"; layoutSelect.value = currentLayout; showEmptyInspector(); render(); });
queryInput.addEventListener("keydown", (event) => { if (event.key === "Enter") updateHistory(queryInput.value.trim() || "Untitled query"); });
exportOverlayButton.addEventListener("click", () => {
  const overlay = currentResult.overlays?.[fixtureSelect.value as OverlayFixture];
  if (!overlay) return;
  const payload = exportOverlaySnapshot(overlay, currentResult.overlays?.exportMaxBytes ?? 16_384);
  const link = document.createElement("a");
  link.href = URL.createObjectURL(new Blob([payload], { type: "application/json" }));
  link.download = `cidx-${overlay.kind}-overlay.json`;
  link.click();
  URL.revokeObjectURL(link.href);
});

updateHistory("Symbol / calls");
render();
