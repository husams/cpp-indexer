const assert = require('node:assert/strict');

// HSE-92 fix: `expandDirection()` writes `depth`/`direction` into
// `currentParams` via pushMerge (they are traversal state scoped to
// whatever node Expand was called on). The search-result click handler used
// to build its next request with `freshSemanticParams({root: match.id})`,
// which spreads `currentParams` verbatim -- so a previous Expand's
// depth/direction rode along onto a completely different root. Unlike the
// continuation-token leak this does NOT 400 or wedge the session: a bare
// {root} request defaults server-side to depth=2/direction=out
// (GraphViewRequest in graph_view.hpp), so it silently returns a narrower,
// wrong-direction neighbourhood for the new root with no visible signal.
//
// This is a hand-rolled DOM/fetch stub harness (no jsdom/Playwright
// available in this sandbox) that `require()`s the real web/app.js browser
// IIFE end-to-end in LIVE (non-offline) mode, driving the actual tap-to-
// select, Expand, and search-result-click handlers -- not a
// re-implementation of them -- and inspects the ACTUAL outgoing /api/graph
// requests.

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
  prompt: () => 'depth-leak-test-view',
};
global.setInterval = () => 0;
const storageBacking = new Map();
global.localStorage = {
  getItem: (key) => (storageBacking.has(key) ? storageBacking.get(key) : null),
  setItem: (key, value) => { storageBacking.set(key, String(value)); },
  removeItem: (key) => { storageBacking.delete(key); },
};

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

const makeView = () => ({
  schema: 'cidx.graph-view.v1', version: 1, status: 'complete', markers: [],
  query_identity: 'q', result_id: `r-${Math.random()}`,
  identity: {workspace: 'w', index: 'i', fact_sets: [], freshness: 'current', catalog_version: 1, catalog_hash: 'h'},
  request: {input_kind: 'symbol', input: 'x', node_budget: 10, edge_budget: 10, site_budget: 10, byte_budget: 4096},
  metadata: {
    status: 'complete', markers: [], truncated: false, evidence_truncated: false,
    continuation: {available: false}, identity: {freshness: 'current'},
  },
  nodes: [
    {id: 'node-a', name: 'a', usr: 'usr:a', kind: 'function',
     status: {completeness: 'complete', freshness: 'current'}},
    {id: 'node-b', name: 'nodeB', usr: 'usr:b', kind: 'function',
     status: {completeness: 'complete', freshness: 'current'}},
  ],
  edges: [],
  view_state: {},
});

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
  return {ok: true, json: async () => makeView()};
};

require('./app.js');

(async () => {
  const settle = async () => {
    for (let tick = 0; tick < 5; tick += 1) {
      await new Promise((resolve) => setImmediate(resolve));
    }
  };
  await settle(); // initial load

  // 1. Select node-a and Expand in -- this is the ONLY legitimate write site
  // for depth/direction, and it becomes part of the active `currentParams`.
  assert.ok(tapHandler, 'app.js must have registered a tap handler');
  tapHandler({target: {group: () => 'nodes', data: () => ({}), id: () => 'node-a'}});
  document.getElementById('expand-in').onclick();
  await settle();
  const expanded = graphRequests.at(-1);
  assert.equal(expanded.depth, '1', 'Expand must request depth=1');
  assert.equal(expanded.direction, 'in', 'Expand must request direction=in');

  // 2. Search for nodeB and click its result -- this is a NEW-ROOT
  // navigation, not an Expand. The repro's core assertion: node-a's
  // Expand-in depth/direction must NOT ride along onto node-b's request.
  document.getElementById('index-search').value = 'nodeB';
  await document.getElementById('index-search-run').onclick();
  // Distinguish the search-result button ("nodeB (function) — b.cpp:1") from
  // any accessible-node-list button also rendered for node-b ("nodeB
  // (function)") once it has entered the canvas via Expand.
  const resultButton = createdButtons.find(
    (button) => String(button.textContent).includes('nodeB') && String(button.textContent).includes('—'));
  assert.ok(resultButton, 'search must have rendered a result button for nodeB');
  resultButton.onclick();
  await settle();
  const searched = graphRequests.at(-1);
  assert.equal(searched.root, 'node-b', 'search-result click must request the clicked root');
  assert.equal('depth' in searched, false,
    'search-result click must not carry Expand-in\'s depth onto a different root');
  assert.equal('direction' in searched, false,
    'search-result click must not carry Expand-in\'s direction onto a different root');

  console.log('depth/direction leak regression passed');
})();
