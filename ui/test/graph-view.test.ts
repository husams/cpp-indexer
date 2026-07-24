import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import Ajv2020 from "ajv/dist/2020.js";
import { toCytoscapeElements } from "../src/cytoscape-adapter.ts";
import { boundedFixture, canonicalFixture, oversizedFixture } from "../src/fixtures.ts";
import { applyBudget, canonicalSemanticContent, catalogRelationNames, createSavedView, stablePortableId, toResultEnvelope, validateGraphView, visualSemantics, type Budget } from "../src/graph-view.ts";

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

test("mixed truth cues are deterministic across marker permutations", () => {
  const permutations = [
    ["proved", "stale", "inferred"],
    ["inferred", "proved", "stale"],
    ["stale", "inferred", "proved"],
  ] as const;
  const rendered = permutations.map((markers) => visualSemantics("partial", markers));
  assert.equal(new Set(rendered.map((semantics) => JSON.stringify(semantics))).size, 1);
  assert.match(rendered[0]!.labelSuffix, /partial/);
  assert.match(rendered[0]!.labelSuffix, /proved/);
  assert.match(rendered[0]!.inspectorText, /Partial coverage/);
  assert.match(rendered[0]!.inspectorText, /Proof-backed/);
  assert.equal(rendered[0]!.isCompleteStyle, false);

  const refuted = visualSemantics("complete", ["external", "refuted"]);
  assert.match(refuted.labelSuffix, /refuted/);
  assert.match(refuted.labelSuffix, /external/);
  assert.match(refuted.inspectorText, /Refuted/);
  assert.match(refuted.inspectorText, /External/);
  assert.equal(refuted.isCompleteStyle, false);
});

test("GraphView JSON Schema validates canonical envelopes and rejects invalid payloads", () => {
  const schema = JSON.parse(readFileSync(new URL("../../schemas/graph-view.schema.json", import.meta.url), "utf8"));
  const sharedSchema = JSON.parse(readFileSync(new URL("../../spec/contracts/result-envelope.schema.json", import.meta.url), "utf8"));
  const ajv = new Ajv2020({ allErrors: true, strict: false });
  ajv.addSchema(sharedSchema);
  const validator = ajv.compile(schema);
  for (const slice of ["symbol", "entity", "include", "type"] as const) {
    const result = canonicalFixture(slice);
    const envelope = toResultEnvelope(result);
    assert.deepEqual(Object.keys(envelope), ["envelopeVersion", "operation", "status", "reasonCode", "diagnostics", "producer", "package", "backend", "context", "artifact", "replay", "resources", "payload"]);
    assert.equal(envelope.context.factSet.key, result.factSet.key);
    assert.equal(validator(envelope), true, `${slice}: ${JSON.stringify(validator.errors)}`);

    const withContinuation = {
      ...envelope,
      payload: { ...result, continuation: { token: "continuation:fixture", nextDepth: 1, remaining: { nodes: 4, edges: 5 } } },
    };
    assert.equal(validator(withContinuation), true, `${slice} continuation: ${JSON.stringify(validator.errors)}`);
  }

  const saved = createSavedView(canonicalFixture("symbol"), { layout: "preset", zoom: 1, pan: { x: 0, y: 0 }, hiddenKinds: [], collapsedGroupKeys: [] });
  assert.equal(validator(saved), true, `saved view: ${JSON.stringify(validator.errors)}`);

  const invalid = structuredClone(toResultEnvelope(canonicalFixture("symbol")));
  invalid.payload.nodes[0]!.ref.key = "42";
  assert.equal(validator(invalid), false);
  assert.ok(validator.errors?.some((error) => error.instancePath.includes("/payload/nodes/0/ref/key")));

  const missingEvidenceRefs = structuredClone(toResultEnvelope(canonicalFixture("symbol")));
  delete (missingEvidenceRefs.payload.nodes[0] as { evidenceRefs?: readonly string[] }).evidenceRefs;
  assert.equal(validator(missingEvidenceRefs), false);

  const missingSharedIdentity = structuredClone(toResultEnvelope(canonicalFixture("symbol")));
  delete (missingSharedIdentity as { producer?: unknown }).producer;
  assert.equal(validator(missingSharedIdentity), false);
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
