import assert from "node:assert/strict";
import test from "node:test";
import { toCytoscapeElements } from "../src/cytoscape-adapter.ts";
import { boundedFixture, canonicalFixture, oversizedFixture } from "../src/fixtures.ts";
import { applyBudget, canonicalSemanticContent, catalogRelationNames, createSavedView, stablePortableId, validateGraphView, visualSemantics, type Budget } from "../src/graph-view.ts";

const smallBudget: Budget = { maxNodes: 2, maxEdges: 1, maxGroups: 0, maxLabelChars: 400, maxEvidenceRefs: 4 };

test("portable GraphView validation rejects database-local identities", () => {
  const result = canonicalFixture("symbol");
  assert.throws(() => stablePortableId({ key: "42", kind: "symbol", semanticUniverse: "fixture" }), /database-local/);
  assert.doesNotThrow(() => validateGraphView(result));
  assert.ok(catalogRelationNames().includes("calls"));
});

test("canonical semantic content is unchanged by layout choice", () => {
  const result = canonicalFixture("symbol");
  const hierarchy = { ...result, resultId: "layout:hierarchy" };
  const neighborhood = { ...result, resultId: "layout:neighborhood" };
  assert.equal(canonicalSemanticContent(hierarchy), canonicalSemanticContent(neighborhood));
});

test("every weaker truth state has a non-colour cue and cannot use complete styling", () => {
  for (const status of ["partial", "unknown", "error"] as const) {
    const semantics = visualSemantics(status, []);
    assert.notEqual(semantics.glyph, "");
    assert.notEqual(semantics.inspectorText, "");
    assert.equal(semantics.isCompleteStyle, false);
  }
  for (const marker of ["truncated", "stale", "unresolved", "external", "inferred", "assumed", "refuted"] as const) {
    const semantics = visualSemantics("complete", [marker]);
    assert.notEqual(semantics.glyph, "");
    assert.notEqual(semantics.inspectorText, "");
    assert.equal(semantics.isCompleteStyle, false);
  }
});

test("all supported fixture slices adapt to Cytoscape elements with typed evidence", () => {
  for (const slice of ["symbol", "entity", "include", "type"] as const) {
    const result = canonicalFixture(slice);
    validateGraphView(result);
    const elements = toCytoscapeElements(result);
    assert.equal(elements.length, result.nodes.length + result.edges.length + result.groups.length);
    assert.ok(elements.every((element) => typeof element.data?.id === "string"));
  }
});

test("oversized input produces a deterministic bounded response", () => {
  const source = oversizedFixture();
  const first = applyBudget(source, smallBudget);
  const second = applyBudget(source, smallBudget);
  assert.deepEqual(first, second);
  assert.equal(first.nodes.length, 2);
  assert.equal(first.edges.length, 1);
  assert.equal(first.status, "partial");
  assert.ok(first.markers.includes("truncated"));
  assert.equal(first.diagnostics[0]?.code, "budget_exhausted");
});

test("saved presentation state is separate from query/result identity", () => {
  const result = boundedFixture("symbol", smallBudget);
  const saved = createSavedView(result, { layout: "preset", zoom: 1.25, pan: { x: 8, y: -4 }, hiddenKinds: ["group"], collapsedGroupKeys: [] });
  assert.equal(saved.resultId, result.resultId);
  assert.equal(saved.queryIdentity, result.queryIdentity);
  assert.notEqual(saved.savedViewId, result.resultId);
  assert.equal(saved.presentation.layout, "preset");
});
