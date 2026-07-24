import cytoscape, { type Core } from "cytoscape";
import { toCytoscapeElements } from "./cytoscape-adapter.ts";
import { boundedFixture, canonicalFixture } from "./fixtures.ts";
import { applyBudget, visualSemantics, type GraphKind, type GraphViewResult, type LayoutName } from "./graph-view.ts";
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

if (!graphElement || !inspectorElement || !budgetElement || !statusElement || !historyElement || !historyCountElement || !fixtureSelect || !layoutSelect || !queryInput || !showGroupsInput || !showEvidenceInput || !resetButton) {
  throw new Error("GraphView prototype markup is incomplete");
}

let cy: Core | undefined;
let currentResult: GraphViewResult = boundedFixture("symbol", browserBudget());
let currentLayout: LayoutName = "breadthfirst";
let history: string[] = [];

const visualTruthLegend = [
  ["complete", "●", "solid", "Complete source or catalog fact"],
  ["partial", "◐", "dashed", "Coverage is incomplete; absence is not proof"],
  ["unknown", "?", "dotted", "The result cannot establish truth"],
  ["stale", "◌", "double", "Fact set is stale against the expected revision"],
  ["external", "↗", "dotted", "External/stub identity with bounded local evidence"],
  ["inferred", "≈", "dashed", "Derived or inferred relation"],
] as const;

function browserBudget() {
  return { maxNodes: 32, maxEdges: 48, maxGroups: 8, maxLabelChars: 2_400, maxEvidenceRefs: 64 };
}

function render(): void {
  const includeGroups = showGroupsInput.checked;
  currentResult = applyBudget(canonicalFixture(fixtureSelect.value as Exclude<GraphKind, "group" | "file">), browserBudget());
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

updateHistory("Symbol / calls");
render();
