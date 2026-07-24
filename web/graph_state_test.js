const assert = require('node:assert/strict');
const {mergeSlices} = require('./app.js');

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

console.log('graph state merge regression passed');
