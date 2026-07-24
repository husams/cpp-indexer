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

console.log('graph state merge regression passed');
