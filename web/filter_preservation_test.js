const assert = require('node:assert/strict');

// HSE-92 P1 fix: expandDirection() and the search-result click handler used
// to build their next request from scratch ({root, depth, direction} /
// {root}), discarding whatever filter fields (node_kind/file/component/
// repository/status/applicability/edge kinds) were active in `currentParams`.
// That both (a) merged filter-violating nodes into the canvas, and (b)
// silently dropped the filter from `currentParams` for the rest of the
// session -- every later action (Load more, the freshness poll, history
// replay) then ran unfiltered with no visible signal. This is a hand-rolled
// DOM/fetch stub harness (no jsdom/Playwright available in this sandbox)
// that `require()`s the real web/app.js browser IIFE end-to-end in LIVE
// (non-offline) mode, driving the real "apply filters", tap-to-select,
// Expand, and search-result-click handlers -- not a re-implementation of
// them -- and inspects the ACTUAL outgoing /api/graph requests.

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

const createdButtons = [];
const elementRegistry = new Map();
global.document = {
  getElementById(id) {
    if (!elementRegistry.has(id)) elementRegistry.set(id, makeElementStub());
    return elementRegistry.get(id);
  },
  createElement(tag) {
    const el = makeElementStub();
    if (tag === 'button') createdButtons.push(el);
    return el;
  },
  querySelectorAll() {
    return [];
  },
};
global.navigator = {clipboard: {writeText: () => Promise.resolve()}};
global.window = {
  CIDX_OFFLINE: false,
  location: {search: '?token=test-token'},
};
global.setInterval = () => 0;
global.localStorage = {
  getItem: () => null,
  setItem: () => {},
  removeItem: () => {},
};

// Only the filter field this test exercises is populated; the rest of the
// filter row stays blank, matching what apply-filters reads from the DOM.
document.getElementById('filter-node-kind').value = 'function';

let tapHandler = null;
global.cytoscape = () => ({
  add() {},
  nodes() {
    return {forEach() {}, map() { return []; }, length: 0,
            positions() {}, reduce: (fn, init) => init};
  },
  $() {
    return {length: 0, select() {}, remove() {}};
  },
  $id() {
    return {length: 0};
  },
  on(event, _selector, handler) { if (event === 'tap') tapHandler = handler; },
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
});

const baseMetadata = {
  status: 'complete', markers: [], truncated: false, evidence_truncated: false,
  continuation: {available: false, reason: 'complete'}, identity: {freshness: 'current'},
};
const graphView = {
  schema: 'cidx.graph-view.v1', version: 1, status: 'complete', markers: [],
  query_identity: 'q', result_id: 'r',
  identity: {workspace: 'w', index: 'i', fact_sets: [], freshness: 'current', catalog_version: 1, catalog_hash: 'h'},
  request: {input_kind: 'symbol', input: 'x', node_budget: 10, edge_budget: 10, site_budget: 10, byte_budget: 4096},
  metadata: baseMetadata,
  nodes: [{id: 'node-a', name: 'a', usr: 'usr:a', kind: 'function',
           status: {completeness: 'complete', freshness: 'current'}}],
  edges: [],
  view_state: {},
};

const graphRequests = [];
global.fetch = async (url) => {
  const [path, queryString] = url.split('?');
  const params = Object.fromEntries(new URLSearchParams(queryString || ''));
  if (path.endsWith('/api/search')) {
    return {ok: true, json: async () => ({matches: [
      {id: 'node-b', name: 'nodeB', kind: 'function', location: 'b.cpp:1'},
    ]})};
  }
  graphRequests.push(params);
  return {ok: true, json: async () => graphView};
};

require('./app.js');

(async () => {
  const settle = async () => {
    for (let tick = 0; tick < 5; tick += 1) {
      await new Promise((resolve) => setImmediate(resolve));
    }
  };
  await settle(); // initial load

  // 1. Apply the node_kind filter -- becomes the active `currentParams`.
  document.getElementById('apply-filters').onclick();
  await settle();
  const filtered = graphRequests.at(-1);
  assert.equal(filtered.node_kind, 'function', 'filter apply must request node_kind=function');

  // 2. A continuation token is one-shot paging state, not part of the
  // active semantic base request. Load one page, then re-apply the filter:
  // the new request must not replay the stale token.
  graphView.metadata.continuation = {available: true, reason: 'budget', token: 'page-2'};
  document.getElementById('apply-filters').onclick();
  await settle();
  document.getElementById('load-more').onclick();
  await settle();
  assert.equal(graphRequests.at(-1).continuation, 'page-2');
  document.getElementById('apply-filters').onclick();
  await settle();
  assert.equal(graphRequests.at(-1).continuation, undefined,
    'semantic changes must discard the previous continuation token');
  graphView.metadata.continuation = {available: false, reason: 'complete'};

  // 3. Select node-a (simulating a Cytoscape tap) and Expand.
  assert.ok(tapHandler, 'app.js must have registered a tap handler');
  tapHandler({target: {group: () => 'nodes', data: () => ({}), id: () => 'node-a'}});
  document.getElementById('expand').onclick();
  await settle();
  const expanded = graphRequests.at(-1);
  assert.equal(expanded.node_kind, 'function',
    'Expand must preserve the active node_kind filter in its outgoing request');
  assert.equal(expanded.root, 'node-a');
  assert.equal(expanded.depth, '1');
  assert.equal(expanded.direction, 'out');

  // 4. Search, then click the one result -- must also preserve the filter.
  document.getElementById('index-search').value = 'nodeB';
  await document.getElementById('index-search-run').onclick();
  const resultButton = createdButtons.find((button) => String(button.textContent).includes('nodeB'));
  assert.ok(resultButton, 'search must have rendered a result button for nodeB');
  resultButton.onclick();
  await settle();
  const searched = graphRequests.at(-1);
  assert.equal(searched.node_kind, 'function',
    'clicking a search result must preserve the active node_kind filter in its outgoing request');
  assert.equal(searched.root, 'node-b');

  // 5. Independent check that `currentParams` itself (not just each
  // one-shot outgoing request) retained the filter: stale-refresh re-fetches
  // with whatever `currentParams` currently holds.
  document.getElementById('stale-refresh').onclick();
  await settle();
  const refreshed = graphRequests.at(-1);
  assert.equal(refreshed.node_kind, 'function',
    'currentParams must still carry the filter after Expand and search-navigate (stale-refresh replay)');
  assert.equal(refreshed.root, 'node-b');

  console.log('filter preservation regression passed');
})();
