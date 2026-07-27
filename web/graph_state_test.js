const assert = require('node:assert/strict');
const {mergeSlices, trackFreshness, computeGroups} = require('./app.js');

const merged = mergeSlices({
  metadata: {
    truncated: false,
    evidence_truncated: false,
    sites_used: 1,
    continuation: {available: false, reason: 'complete'},
  },
  nodes: [{
    id: 'node-a',
    name: 'old label',
    status: {completeness: 'complete', freshness: 'current', truncated: false},
    evidence: {bounded: true, truncated: false},
  }],
  edges: [{
    id: 'edge-a',
    source: 'node-a',
    target: 'node-a',
    sites: [{location: 'main.cpp:1:1'}],
    status: {completeness: 'complete', freshness: 'current', truncated: false, evidence_truncated: false},
  }],
}, {
  metadata: {
    truncated: true,
    evidence_truncated: true,
    continuation: {available: true, reason: 'byte_budget'},
  },
  nodes: [{
    id: 'node-a',
    name: 'updated label',
    status: {completeness: 'partial', freshness: 'stale', truncated: true},
    evidence: {bounded: true, truncated: true},
  }],
  edges: [{
    id: 'edge-a',
    source: 'node-a',
    target: 'node-a',
    sites: [{location: 'main.cpp:2:1'}],
    status: {completeness: 'partial', freshness: 'stale', truncated: true, evidence_truncated: true},
  }],
});

assert.equal(merged.nodes.length, 1);
assert.equal(merged.edges.length, 1);
assert.equal(merged.nodes[0].name, 'updated label');
assert.equal(merged.nodes[0].status.completeness, 'partial');
assert.equal(merged.nodes[0].status.freshness, 'stale');
assert.equal(merged.nodes[0].status.truncated, true);
assert.equal(merged.nodes[0].evidence.truncated, true);
assert.equal(merged.edges[0].sites.length, 2);
assert.equal(merged.edges[0].status.evidence_truncated, true);
assert.equal(merged.metadata.truncated, true);
assert.equal(merged.metadata.evidence_truncated, true);
assert.equal(merged.metadata.sites_used, 2);
assert.deepEqual(merged.metadata.continuation, {available: true, reason: 'byte_budget'});

const bounded = mergeSlices({
  request: {node_budget: 1, edge_budget: 1, site_budget: 1},
  metadata: {node_budget: 1, edge_budget: 1, site_budget: 1, continuation: {available: false, reason: 'complete'}},
  nodes: [{id: 'node-first'}],
  edges: [{id: 'edge-first', source: 'node-first', target: 'node-first', sites: [{location: 'first.cpp:1:1'}]}],
}, {
  request: {node_budget: 1, edge_budget: 1, site_budget: 1},
  metadata: {node_budget: 1, edge_budget: 1, site_budget: 1, continuation: {available: false, reason: 'complete'}},
  nodes: [{id: 'node-second'}],
  edges: [{id: 'edge-second', source: 'node-second', target: 'node-second', sites: [{location: 'second.cpp:1:1'}]}],
});

assert.deepEqual(bounded.nodes.map((node) => node.id), ['node-first']);
assert.deepEqual(bounded.edges.map((edge) => edge.id), ['edge-first']);
assert.equal(bounded.nodes[0].status.truncated, true);
assert.equal(bounded.edges[0].status.truncated, true);
assert.equal(bounded.metadata.sites_used, 1);
assert.equal(bounded.metadata.truncated, true);
assert.equal(bounded.metadata.continuation.available, true);
assert.equal(bounded.metadata.continuation.reason, 'budget');

const siteBounded = mergeSlices({
  request: {node_budget: 2, edge_budget: 2, site_budget: 1},
  metadata: {node_budget: 2, edge_budget: 2, site_budget: 1, continuation: {available: false, reason: 'complete'}},
  nodes: [{id: 'site-node-first'}],
  edges: [{id: 'site-edge-first', source: 'site-node-first', target: 'site-node-first', sites: [{location: 'first.cpp:1:1'}]}],
}, {
  request: {node_budget: 2, edge_budget: 2, site_budget: 1},
  metadata: {node_budget: 2, edge_budget: 2, site_budget: 1, continuation: {available: false, reason: 'complete'}},
  nodes: [{id: 'site-node-second'}],
  edges: [{id: 'site-edge-second', source: 'site-node-second', target: 'site-node-second', sites: [{location: 'second.cpp:1:1'}]}],
});

assert.equal(siteBounded.nodes.length, 2);
assert.equal(siteBounded.edges.length, 2);
assert.equal(siteBounded.metadata.sites_used, 1);
assert.equal(siteBounded.metadata.evidence_truncated, true);
assert.equal(siteBounded.metadata.truncated, true);
assert.equal(siteBounded.edges[1].sites.length, 0);
assert.equal(siteBounded.edges[1].evidence.sites_truncated, true);

// HSE-92 review fix: a continuation ("Load more") merge must GROW the
// canvas across pages, not re-cap it to a single page's own tiny budget.
// Two one-node pages, each with node_budget=1, must merge to 2 retained
// nodes (not 1) when merged with {cumulative: true}.
const continuationPageOne = {
  status: 'partial',
  request: {node_budget: 1, edge_budget: 1, site_budget: 1},
  metadata: {
    node_budget: 1, edge_budget: 1, site_budget: 1, truncated: true,
    continuation: {available: true, reason: 'budget', token: 'page-2'},
  },
  nodes: [{id: 'cont-node-a', status: {truncated: true}}],
  edges: [],
};
const continuationPageTwo = {
  status: 'complete',
  request: {node_budget: 1, edge_budget: 1, site_budget: 1},
  metadata: {
    node_budget: 1, edge_budget: 1, site_budget: 1, truncated: false,
    continuation: {available: false, reason: 'complete'},
  },
  nodes: [{id: 'cont-node-b', status: {truncated: false}}],
  edges: [],
};
const continuationMerged = mergeSlices(continuationPageOne, continuationPageTwo, {cumulative: true});
assert.deepEqual(continuationMerged.nodes.map((node) => node.id).sort(), ['cont-node-a', 'cont-node-b']);
assert.equal(continuationMerged.metadata.truncated, false);
assert.equal(continuationMerged.status, 'complete');
assert.deepEqual(continuationMerged.metadata.continuation, {available: false, reason: 'complete'});
assert.notEqual(continuationMerged.nodes.find((node) => node.id === 'cont-node-a').status.truncated, true);
assert.equal(continuationMerged.metadata.node_budget, 2);

// A third page must keep growing against the ALREADY-grown budget (2),
// not silently reset back to a single page's original budget (1).
const continuationPageThree = {
  request: {node_budget: 1, edge_budget: 1, site_budget: 1},
  metadata: {node_budget: 1, edge_budget: 1, site_budget: 1, continuation: {available: false, reason: 'complete'}},
  nodes: [{id: 'cont-node-c'}],
  edges: [],
};
const continuationMergedAgain = mergeSlices(continuationMerged, continuationPageThree, {cumulative: true});
assert.deepEqual(continuationMergedAgain.nodes.map((node) => node.id).sort(), ['cont-node-a', 'cont-node-b', 'cont-node-c']);
assert.equal(continuationMergedAgain.metadata.truncated, false);
assert.equal(continuationMergedAgain.metadata.node_budget, 3);

// Without {cumulative: true} (the default, used for ordinary "expand"
// merges), the pre-existing bounded-by-the-smaller-budget behavior is
// unchanged -- this must keep matching the `bounded` assertions above.
const nonCumulative = mergeSlices(continuationPageOne, continuationPageTwo);
assert.deepEqual(nonCumulative.nodes.map((node) => node.id), ['cont-node-a']);
assert.equal(nonCumulative.metadata.truncated, true);

// HSE-92: a session must never present a merged view as fresher than its
// weakest contributing slice (stale-index behavior surfaced immediately,
// never silently upgraded by a later merge).
const freshnessMerged = mergeSlices({
  metadata: {
    truncated: false, evidence_truncated: false,
    continuation: {available: false, reason: 'complete'},
    freshness: 'current', identity: {freshness: 'current', workspace: 'w'},
  },
  nodes: [{id: 'fresh-node'}], edges: [],
}, {
  metadata: {
    truncated: false, evidence_truncated: false,
    continuation: {available: false, reason: 'complete'},
    freshness: 'stale', identity: {freshness: 'stale', workspace: 'w'},
  },
  nodes: [{id: 'fresh-node'}], edges: [],
});
assert.equal(freshnessMerged.metadata.freshness, 'stale');
assert.equal(freshnessMerged.metadata.identity.freshness, 'stale');

// Merging in the OTHER order (stale first, current second) must not let a
// later "current" slice erase an earlier "stale" observation.
const freshnessMergedReversed = mergeSlices({
  metadata: {
    truncated: false, evidence_truncated: false,
    continuation: {available: false, reason: 'complete'},
    freshness: 'stale', identity: {freshness: 'stale', workspace: 'w'},
  },
  nodes: [{id: 'fresh-node'}], edges: [],
}, {
  metadata: {
    truncated: false, evidence_truncated: false,
    continuation: {available: false, reason: 'complete'},
    freshness: 'current', identity: {freshness: 'current', workspace: 'w'},
  },
  nodes: [{id: 'fresh-node'}], edges: [],
});
assert.equal(freshnessMergedReversed.metadata.freshness, 'stale');
assert.equal(freshnessMergedReversed.metadata.identity.freshness, 'stale');

assert.equal(trackFreshness('current', 'current'), 'current');
assert.equal(trackFreshness('current', 'stale'), 'stale');
assert.equal(trackFreshness('stale', 'current'), 'stale');
assert.equal(trackFreshness('current', 'unverifiable'), 'unverifiable');
assert.equal(trackFreshness(undefined, 'current'), 'current');

// HSE-92 round 3 review fix: a merged (cumulative) view must never present
// itself as the FIRST contributing slice's own atomic result -- it must
// carry an honest composite identity/provenance chain, and completeness
// must be computed over that composite.
const provenancePageOne = {
  status: 'complete', markers: [], result_id: 'page-1', query_identity: 'q1',
  request: {input: 'root', node_budget: 1},
  metadata: {node_budget: 1, edge_budget: 1, site_budget: 1, continuation: {available: true, reason: 'budget'}},
  nodes: [{id: 'prov-node-a'}], edges: [],
};
const provenancePageTwo = {
  status: 'complete', markers: [], result_id: 'page-2', query_identity: 'q1',
  request: {input: 'root', node_budget: 1, continuation: 'cont-token'},
  metadata: {node_budget: 1, edge_budget: 1, site_budget: 1, continuation: {available: false, reason: 'complete'}},
  nodes: [{id: 'prov-node-b'}], edges: [],
};
const provenanceMerged = mergeSlices(provenancePageOne, provenancePageTwo, {cumulative: true});
assert.notEqual(provenanceMerged.result_id, provenancePageOne.result_id,
  'a merged view must never claim to BE the first page\'s own result');
assert.notEqual(provenanceMerged.result_id, provenancePageTwo.result_id);
assert.deepEqual(provenanceMerged.result_chain.map((entry) => entry.result_id), ['page-1', 'page-2']);
assert.equal(provenanceMerged.result_chain[1].request.continuation, 'cont-token');
// Both pages are individually complete and the merge introduces no
// truncation (node_budget summed to 2, exactly 2 nodes delivered) --
// completeness computed over the composite must stay "complete", not
// silently regress just because two slices were combined.
assert.equal(provenanceMerged.status, 'complete');

// If either contributing slice was only partial, the composite must not
// overstate completeness as "complete".
const provenancePartial = mergeSlices(
  {...provenancePageOne, status: 'partial'}, provenancePageTwo, {cumulative: true});
assert.equal(provenancePartial.status, 'partial');

// A merge that ITSELF truncates (budget too small for the union) must also
// downgrade a composite that would otherwise read as complete.
const provenanceBudgetTruncated = mergeSlices(
  {...provenancePageOne, metadata: {...provenancePageOne.metadata, node_budget: 1}},
  {...provenancePageTwo, metadata: {...provenancePageTwo.metadata, node_budget: 0}});
assert.equal(provenanceBudgetTruncated.status, 'partial');

// Merging a THIRD page onto an already-merged (chained) view must extend
// the existing chain, not collapse it back to a single entry.
const provenancePageThree = {
  status: 'complete', markers: [], result_id: 'page-3', query_identity: 'q1',
  request: {input: 'root', node_budget: 1, continuation: 'cont-token-2'},
  metadata: {node_budget: 1, edge_budget: 1, site_budget: 1, continuation: {available: false, reason: 'complete'}},
  nodes: [{id: 'prov-node-c'}], edges: [],
};
const provenanceMergedAgain = mergeSlices(provenanceMerged, provenancePageThree, {cumulative: true});
assert.deepEqual(provenanceMergedAgain.result_chain.map((entry) => entry.result_id), ['page-1', 'page-2', 'page-3']);

// HSE-92 review fix: exercise the grouping DECISION algorithm directly
// (previously untested -- the C++ "grouping" test only covered the
// node_kind server-side filter, never grouping at all).
const groupingNodes = [
  {id: 'n1', component: 'core', file: 'a.cpp', kind: 'function'},
  {id: 'n2', component: 'core', file: 'b.cpp', kind: 'function'},
  {id: 'n3', component: 'util', file: 'a.cpp', kind: 'variable'},
  {id: 'n4', cidxGroup: true, id2: 'group-node-should-be-ignored'},
];
const byComponent = computeGroups(groupingNodes, 'component');
assert.equal(byComponent.length, 1);
assert.equal(byComponent[0].groupId, 'cidx-group:component:core');
assert.deepEqual(byComponent[0].memberIds.sort(), ['n1', 'n2']);

const byFile = computeGroups(groupingNodes, 'file');
assert.equal(byFile.length, 1);
assert.equal(byFile[0].groupId, 'cidx-group:file:a.cpp');
assert.deepEqual(byFile[0].memberIds.sort(), ['n1', 'n3']);

// A singleton group (only one member) is not grouped at all.
assert.deepEqual(computeGroups(groupingNodes, 'kind').filter((g) => g.groupKey === 'variable'), []);
// No key selected: no grouping.
assert.deepEqual(computeGroups(groupingNodes, ''), []);
// The synthetic compound-group node itself is never re-grouped.
byComponent.concat(byFile).forEach((group) => assert.ok(!group.memberIds.includes('n4')));

console.log('graph state merge regression passed');
