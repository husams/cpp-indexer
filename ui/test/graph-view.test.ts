import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import Ajv2020 from "ajv/dist/2020.js";
import { toCytoscapeElements } from "../src/cytoscape-adapter.ts";
import { boundedFixture, canonicalFixture, oversizedFixture } from "../src/fixtures.ts";
import { applyBudget, assertPortableReference, canonicalSemanticContent, catalogRelationNames, createSavedView, stablePortableId, toResultEnvelope, usageOf, utf8ByteLength, validateGraphView, visualSemantics, type Budget } from "../src/graph-view.ts";
import { CORE_CATALOG } from "../src/generated/catalog.ts";

const smallBudget: Budget = { maxNodes: 2, maxEdges: 1, maxGroups: 0, maxLabelChars: 400, maxEvidenceRefs: 4, maxSites: 4, maxSiteBytes: 512 };

test("portable GraphView validation rejects database-local identities", () => {
  const result = canonicalFixture("symbol");
  assert.throws(() => stablePortableId({ key: "42", kind: "symbol", semanticUniverse: "fixture" }), /database-local/);
  assert.doesNotThrow(() => validateGraphView(result));
  assert.ok(catalogRelationNames().includes("calls"));
});

test("portable IDs are length-prefixed and duplicate identities are rejected", () => {
  const left = { kind: "symbol" as const, semanticUniverse: "a:b", key: "c" };
  const right = { kind: "symbol" as const, semanticUniverse: "a", key: "b:c" };
  assert.notEqual(stablePortableId(left), stablePortableId(right));
  const result = canonicalFixture("symbol");
  const nodes = [...result.nodes, { ...result.nodes[0]! }];
  const duplicate = { ...result, nodes, usage: usageOf({ nodes, edges: result.edges, groups: result.groups, evidence: result.evidence }) };
  assert.throws(() => validateGraphView(duplicate), /duplicate portable identities/);
});

test("portable-key parity vectors agree between runtime and JSON Schema", () => {
  const vectors = JSON.parse(readFileSync(new URL("../../spec/contracts/golden/portable-key-vectors.json", import.meta.url), "utf8")) as { valid: string[]; invalid: string[] };
  const schema = JSON.parse(readFileSync(new URL("../../schemas/graph-view.schema.json", import.meta.url), "utf8"));
  const sharedSchema = JSON.parse(readFileSync(new URL("../../spec/contracts/result-envelope.schema.json", import.meta.url), "utf8"));
  const ajv = new Ajv2020({ allErrors: true, strict: false });
  ajv.addSchema(sharedSchema, "https://cidx.dev/schemas/result-envelope/v1");
  const validator = ajv.compile(schema);
  for (const key of vectors.valid) {
    assert.doesNotThrow(() => assertPortableReference({ key, kind: "symbol", semanticUniverse: "fixture" }));
    assert.equal(validator({ version: 1, operation: "query", query: { slice: "symbol", root: { key, kind: "symbol", semanticUniverse: "fixture" } }, budget: { maxNodes: 1, maxEdges: 0, maxGroups: 0, maxLabelChars: 1, maxEvidenceRefs: 0, maxSites: 0, maxSiteBytes: 0 } }), true, `schema rejected valid key ${key}: ${JSON.stringify(validator.errors)}`);
  }
  for (const key of vectors.invalid) {
    assert.throws(() => assertPortableReference({ key, kind: "symbol", semanticUniverse: "fixture" }), /database-local/);
    assert.equal(validator({ version: 1, operation: "query", query: { slice: "symbol", root: { key, kind: "symbol", semanticUniverse: "fixture" } }, budget: { maxNodes: 1, maxEdges: 0, maxGroups: 0, maxLabelChars: 1, maxEvidenceRefs: 0, maxSites: 0, maxSiteBytes: 0 } }), false, `schema accepted invalid key ${key}`);
  }
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
  ajv.addSchema(sharedSchema, "https://cidx.dev/schemas/result-envelope/v1");
  const validator = ajv.compile(schema);
  for (const slice of ["symbol", "entity", "include", "type"] as const) {
    const result = canonicalFixture(slice);
    const envelope = toResultEnvelope(result);
    assert.deepEqual(Object.keys(envelope), ["protocol", "operation", "status", "exit_class", "exit_code", "result", "identity", "producer", "completeness", "diagnostics", "evidence", "artifacts", "replay", "resources"]);
    assert.deepEqual(envelope.identity.fact_sets, [result.factSet.key]);
    assert.equal(envelope.identity.freshness, result.freshness === "fresh" ? "current" : result.freshness === "stale" ? "stale" : "unverifiable");
    assert.ok(envelope.identity.source_revision);
    assert.ok(envelope.identity.source_fingerprint);
    assert.equal(envelope.artifacts[0]?.catalog_hash, CORE_CATALOG.catalog_hash);
    assert.equal(validator(envelope), true, `${slice}: ${JSON.stringify(validator.errors)}`);

    const withContinuation = {
      ...envelope,
      result: { ...result, continuation: { token: "continuation:fixture", nextDepth: 1, remaining: { nodes: 4, edges: 5 } } },
    };
    assert.equal(validator(withContinuation), true, `${slice} continuation: ${JSON.stringify(validator.errors)}`);
  }

  const saved = createSavedView(canonicalFixture("symbol"), { layout: "preset", zoom: 1, pan: { x: 0, y: 0 }, hiddenKinds: [], collapsedGroupKeys: [] });
  assert.equal(validator(saved), true, `saved view: ${JSON.stringify(validator.errors)}`);

  const invalid = structuredClone(toResultEnvelope(canonicalFixture("symbol")));
  invalid.result.nodes[0]!.ref.key = "42";
  assert.equal(validator(invalid), false);
  assert.ok(validator.errors?.some((error) => error.instancePath.includes("/result/nodes/0/ref/key")));

  const missingEvidenceRefs = structuredClone(toResultEnvelope(canonicalFixture("symbol")));
  delete (missingEvidenceRefs.result.nodes[0] as { evidenceRefs?: readonly string[] }).evidenceRefs;
  assert.equal(validator(missingEvidenceRefs), false);

  const missingSharedIdentity = structuredClone(toResultEnvelope(canonicalFixture("symbol")));
  delete (missingSharedIdentity as { identity?: unknown }).identity;
  assert.equal(validator(missingSharedIdentity), false);
});

test("shared protocol metadata preserves proof trust and UTF-8 bounds", () => {
  const result = canonicalFixture("symbol");
  const unicodeResult = {
    ...result,
    resultId: "result:unicode",
    nodes: result.nodes.map((node, index) => index === 0 ? { ...node, label: "λ 😀" } : node),
    evidence: [...result.evidence, { id: "ev:proof", class: "proof" as const, role: "derived" as const, summary: "unverified proof claim" }],
  };
  const envelope = toResultEnvelope(unicodeResult);
  assert.equal(utf8ByteLength("λ 😀"), 7);
  assert.equal(envelope.resources?.peak_bytes, null);
  assert.equal(envelope.evidence.find((evidence) => evidence.id === "ev:proof")?.trust, "unverified");
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
  assert.equal(first.diagnostics[0]?.code, "truncated_budget");
  assert.ok(first.continuation);
});

test("edge site budgets trim adversarial sites deterministically and preserve truth metadata", () => {
  const source = canonicalFixture("symbol");
  const edge = source.edges[0]!;
  const adversarial = {
    ...source,
    edges: [{ ...edge, siteRefs: Array.from({ length: 1_000 }, (_, index) => ({ id: `site:${String(index).padStart(4, "0")}`, role: "call-site" as const })) }],
  };
  const budget = { ...source.budget, maxEdges: 1, maxSites: 3, maxSiteBytes: 180 };
  const first = applyBudget(adversarial, budget);
  const second = applyBudget(adversarial, budget);
  assert.deepEqual(first, second);
  assert.equal(first.edges[0]!.siteRefs.length, 3);
  assert.equal(first.usage.sites, 3);
  assert.ok(first.usage.siteBytes <= budget.maxSiteBytes);
  assert.ok(first.markers.includes("truncated"));
  assert.ok(first.edges[0]!.markers.includes("truncated"));
  assert.equal(first.status, "partial");
  assert.equal(first.completeness, "partial");
  assert.ok(first.continuation?.remaining.sites);
  validateGraphView(first);
  const elements = toCytoscapeElements(first);
  const renderedEdge = elements.find((element) => element.data?.kind === "edge");
  assert.equal(renderedEdge?.data?.siteRefs.length, 3);
});

test("saved presentation state is separate from query/result identity", () => {
  const result = boundedFixture("symbol", smallBudget);
  const saved = createSavedView(result, { layout: "preset", zoom: 1.25, pan: { x: 8, y: -4 }, hiddenKinds: ["group"], collapsedGroupKeys: [] });
  assert.equal(saved.resultId, result.resultId);
  assert.equal(saved.queryIdentity, result.queryIdentity);
  assert.notEqual(saved.savedViewId, result.resultId);
  assert.equal(saved.presentation.layout, "preset");
});
