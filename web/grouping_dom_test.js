const assert = require('node:assert/strict');

// HSE-92 review fix: the previous C++ "grouping" test never loaded
// web/app.js, never called applyGrouping(), and never verified a
// Cytoscape compound parent assignment -- it only exercised the
// server-side node_kind filter. No jsdom/headless-browser is available in
// this sandbox, so this is a hand-rolled DOM/Cytoscape stub: just enough
// surface for app.js's real browser IIFE to load and run end-to-end
// (through the SAME applyGrouping() the product ships), with a fake
// `cytoscape()` that records every `.move({parent})` call so grouping can
// be asserted directly against the real code path, not a re-implementation
// of it.

function makeElementStub() {
  const state = {value: '', textContent: '', innerHTML: '', hidden: false, disabled: false};
  return new Proxy(state, {
    get(target, prop) {
      if (prop in target) return target[prop];
      if (prop === 'appendChild') return (child) => child;
      if (prop === 'replaceChildren') return () => {};
      if (prop === 'addEventListener') return () => {};
      if (prop === 'setAttribute') return () => {};
      if (prop === 'querySelectorAll') return () => [];
      if (prop === 'remove') return () => {};
      if (prop === 'classList') return {add() {}, remove() {}, toggle() {}};
      if (prop === 'style') return {};
      if (prop === 'dataset') return {};
      return undefined;
    },
    set(target, prop, value) {
      target[prop] = value;
      return true;
    },
  });
}

const elementRegistry = new Map();
global.document = {
  getElementById(id) {
    if (!elementRegistry.has(id)) elementRegistry.set(id, makeElementStub());
    return elementRegistry.get(id);
  },
  createElement() {
    return makeElementStub();
  },
  querySelectorAll() {
    return [];
  },
};
global.navigator = {clipboard: {writeText: () => Promise.resolve()}};
global.window = {
  CIDX_GRAPH_VIEW: {
    schema: 'cidx.graph-view.v1', version: 1, status: 'complete', markers: [],
    query_identity: 'q', result_id: 'r',
    identity: {workspace: 'w', index: 'i', fact_sets: [], freshness: 'current', catalog_version: 1, catalog_hash: 'h'},
    request: {input_kind: 'symbol', input: 'x', node_budget: 10, edge_budget: 10, site_budget: 10, byte_budget: 4096},
    metadata: {
      status: 'complete', markers: [], truncated: false, evidence_truncated: false,
      continuation: {available: false, reason: 'complete'}, identity: {freshness: 'current'},
    },
    nodes: [
      {id: 'n1', name: 'a', usr: 'usr:a', kind: 'function', component: 'core', status: {completeness: 'complete', freshness: 'current'}},
      {id: 'n2', name: 'b', usr: 'usr:b', kind: 'function', component: 'core', status: {completeness: 'complete', freshness: 'current'}},
      {id: 'n3', name: 'c', usr: 'usr:c', kind: 'function', component: 'util', status: {completeness: 'complete', freshness: 'current'}},
    ],
    edges: [],
    view_state: {},
  },
  CIDX_OFFLINE: true,
  location: {search: ''},
};

// Pre-select "component" grouping: applyGrouping() runs once automatically
// as part of the first addView(), so the selection must exist before
// app.js is loaded (there is no live `change` event to fire in this stub).
document.getElementById('group-by').value = 'component';

const moveCalls = [];
function makeNodeHandle(id, data) {
  return {
    id: () => id,
    data: () => data,
    move({parent}) {
      moveCalls.push({id, parent});
    },
    select() {},
    toggleClass() {},
  };
}

global.cytoscape = (config) => {
  const nodeData = new Map(
    (config.elements || [])
      .filter((el) => el.data.source === undefined)
      .map((el) => [String(el.data.id), el.data]));
  const nodeHandles = new Map(
    [...nodeData.entries()].map(([id, data]) => [id, makeNodeHandle(id, data)]));
  const groupIds = new Set();
  const collection = (ids) => {
    const items = ids.map((id) => nodeHandles.get(id)).filter(Boolean);
    return {forEach: (fn) => items.forEach(fn), map: (fn) => items.map(fn), length: items.length};
  };
  return {
    add(spec) {
      (Array.isArray(spec) ? spec : [spec]).forEach((item) => {
        const id = String(item.data.id);
        nodeData.set(id, item.data);
        nodeHandles.set(id, makeNodeHandle(id, item.data));
        if (item.data.cidxGroup) groupIds.add(id);
      });
    },
    nodes(selector) {
      const ids = [...nodeData.keys()];
      if (selector === '[^cidxGroup]') return collection(ids.filter((id) => !nodeData.get(id).cidxGroup));
      return collection(ids);
    },
    $(selector) {
      if (selector === '.cidx-group') {
        return {
          remove() {
            [...groupIds].forEach((id) => { nodeData.delete(id); nodeHandles.delete(id); });
            groupIds.clear();
          },
        };
      }
      return {length: 0, select() {}};
    },
    $id(id) {
      return nodeHandles.get(String(id)) || {length: 0};
    },
    on() {},
    fit() {},
    animate() {},
    layout() {
      return {run() {}};
    },
    zoom() {
      return 1;
    },
    pan() {
      return {x: 0, y: 0};
    },
    elements() {
      return {removeClass() {}};
    },
  };
};

require('./app.js');

// applyGrouping() ran once automatically during the initial render (group-by
// was pre-set to "component"): n1/n2 share component "core" and must be
// moved under one synthetic compound parent; n3 (component "util") is a
// singleton and must stay ungrouped, exactly matching computeGroups()'s own
// singleton rule -- but asserted here through the real Cytoscape-facing
// applyGrouping() code path, not the extracted pure function.
const coreMoves = moveCalls.filter((call) => call.parent && call.parent.includes(':core'));
assert.deepEqual(coreMoves.map((call) => call.id).sort(), ['n1', 'n2']);
assert.ok(coreMoves.every((call) => call.parent === 'cidx-group:component:core'));
assert.ok(!moveCalls.some((call) => call.id === 'n3' && call.parent),
  'n3 (singleton component "util") must not be grouped');

console.log('grouping DOM-level regression passed');
