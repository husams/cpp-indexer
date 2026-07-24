import type cytoscape from "cytoscape";
import { stablePortableId, visualSemantics, type GraphEdge, type GraphNode, type GraphViewResult } from "./graph-view.ts";

export type CytoscapeElement = cytoscape.ElementDefinition;

export function toCytoscapeElements(result: GraphViewResult, options: { includeGroups?: boolean } = {}): readonly CytoscapeElement[] {
  const includeGroups = options.includeGroups ?? true;
  const parentByMember = new Map(result.groups.flatMap((group) => group.memberRefs.map((member) => [stablePortableId(member), stablePortableId(group.ref)] as const)));
  const nodes: CytoscapeElement[] = result.nodes.map((node) => nodeElement(node, parentByMember.get(stablePortableId(node.ref))));
  const edges: CytoscapeElement[] = result.edges.map((edge) => edgeElement(edge));
  const groups: CytoscapeElement[] = includeGroups ? result.groups.map((group) => ({ data: { id: stablePortableId(group.ref), kind: "group", label: group.label, status: group.status, markers: [...group.markers] } })) : [];
  return [...groups, ...nodes, ...edges];
}

function nodeElement(node: GraphNode, parent?: string): CytoscapeElement {
  const semantics = visualSemantics(node.status, node.markers);
  return {
    data: {
      id: stablePortableId(node.ref),
      portableKey: node.ref.key,
      kind: node.ref.kind,
      semanticUniverse: node.ref.semanticUniverse,
      label: `${semantics.glyph} ${node.label}${semantics.labelSuffix}`,
      semanticKind: node.semanticKind,
      status: node.status,
      markers: [...node.markers],
      pattern: semantics.pattern,
      inspectorText: semantics.inspectorText,
      evidenceRefs: [...node.evidenceRefs],
      sourcePath: node.location?.path,
      sourceLine: node.location?.line,
      parent,
    },
  };
}

function edgeElement(edge: GraphEdge): CytoscapeElement {
  const semantics = visualSemantics(edge.status, edge.markers);
  return {
    data: {
      id: stablePortableId(edge.ref),
      source: stablePortableId(edge.source),
      target: stablePortableId(edge.target),
      portableKey: edge.ref.key,
      kind: edge.ref.kind,
      relation: edge.relation,
      label: `${semantics.glyph} ${edge.label}${semantics.labelSuffix}`,
      status: edge.status,
      markers: [...edge.markers],
      pattern: semantics.pattern,
      inspectorText: semantics.inspectorText,
      evidenceRefs: [...edge.evidenceRefs],
      siteRefs: edge.siteRefs.map((site) => ({ ...site, location: site.location ? { ...site.location } : undefined })),
    },
  };
}
