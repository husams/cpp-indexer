const assert = require('node:assert/strict');

// HSE-92 round 3 review fix: a continuation ("Load more") request was never
// recorded as its own semantic history entry -- only the base request lived
// in `history`/saved views, so navigating away and back (or saving/loading
// the view) silently lost every page past the first. This is a hand-rolled
// DOM/fetch stub harness (no jsdom/Playwright available in this sandbox)
// that `require()`s the real web/app.js browser IIFE end-to-end in LIVE
// (non-offline) mode, driving the actual "Load more", history back/forward,
// and save/load-view button handlers -- not a re-implementation of them.

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
  CIDX_OFFLINE: false,
  location: {search: '?token=test-token'},
  prompt: () => 'my-view',
};

// The freshness monitor's real 15s interval would otherwise keep the
// process alive forever; this test does not exercise it.
global.setInterval = () => 0;

const storageBacking = new Map();
global.localStorage = {
  getItem: (key) => (storageBacking.has(key) ? storageBacking.get(key) : null),
  setItem: (key, value) => { storageBacking.set(key, String(value)); },
  removeItem: (key) => { storageBacking.delete(key); },
};

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
});

const baseMetadata = (continuation) => ({
  status: 'complete', markers: [], truncated: false, evidence_truncated: false,
  continuation, identity: {freshness: 'current'},
  node_budget: 1, edge_budget: 1, site_budget: 1,
});
const pageOneView = {
  schema: 'cidx.graph-view.v1', version: 1, status: 'complete', markers: [],
  query_identity: 'q1', result_id: 'page-1',
  identity: {workspace: 'w', index: 'i', fact_sets: [], freshness: 'current', catalog_version: 1, catalog_hash: 'h'},
  request: {input_kind: 'symbol', input: 'root', node_budget: 1, edge_budget: 1, site_budget: 1, byte_budget: 4096},
  metadata: baseMetadata({available: true, reason: 'budget', token: 'cont-token-1'}),
  nodes: [{id: 'node-a', name: 'a', usr: 'usr:a', kind: 'function',
           status: {completeness: 'complete', freshness: 'current'}}],
  edges: [],
  view_state: {},
};
const pageTwoView = {
  schema: 'cidx.graph-view.v1', version: 1, status: 'complete', markers: [],
  query_identity: 'q1', result_id: 'page-2',
  identity: {workspace: 'w', index: 'i', fact_sets: [], freshness: 'current', catalog_version: 1, catalog_hash: 'h'},
  request: {input_kind: 'symbol', input: 'root', node_budget: 1, edge_budget: 1, site_budget: 1, byte_budget: 4096},
  metadata: baseMetadata({available: false, reason: 'complete'}),
  nodes: [{id: 'node-b', name: 'b', usr: 'usr:b', kind: 'function',
           status: {completeness: 'complete', freshness: 'current'}}],
  edges: [],
  view_state: {},
};

let fetchCalls = 0;
global.fetch = async (url) => {
  fetchCalls += 1;
  const query = url.split('?')[1] || '';
  const params = new URLSearchParams(query);
  const body = params.has('continuation') ? pageTwoView : pageOneView;
  return {ok: true, json: async () => body};
};

require('./app.js');

(async () => {
  for (let tick = 0; tick < 5; tick += 1) {
    await new Promise((resolve) => setImmediate(resolve));
  }

  const summary = () => document.getElementById('summary').textContent;
  // history-back/-forward's onclick wrappers fire jumpTo() without returning
  // its promise (ordinary DOM click handlers, not awaited by a real
  // browser); settle() lets those pending microtasks resolve before the
  // next assertion, same as the initial-render tick loop above.
  const settle = async () => {
    for (let tick = 0; tick < 5; tick += 1) {
      await new Promise((resolve) => setImmediate(resolve));
    }
  };
  assert.ok(summary().startsWith('1 nodes'), `expected 1 node initially, got: ${summary()}`);

  // "Load more": must merge page 2 onto the canvas AND record itself as a
  // history entry (previously it only did the former).
  await document.getElementById('load-more').onclick();
  assert.ok(summary().startsWith('2 nodes'), `expected 2 nodes after Load more, got: ${summary()}`);
  assert.equal(document.getElementById('history-back').disabled, false,
    'Load more must push a history entry -- Back must now be available');

  // Back then Forward must reconstruct the exact same 2-node canvas: this is
  // the "navigate away, then Back" repro's core assertion -- the
  // continuation page must survive a full history replay, not just the
  // in-place merge that produced it.
  document.getElementById('history-back').onclick();
  await settle();
  assert.ok(summary().startsWith('1 nodes'),
    `expected 1 node after Back, got: ${summary()}`);
  document.getElementById('history-forward').onclick();
  await settle();
  assert.ok(summary().startsWith('2 nodes'),
    `expected 2 nodes after Forward (continuation page restored), got: ${summary()}`);

  // Save this exact (page-1 + page-2) view, then move away from it, then
  // load it back -- the saved view must restore the full 2-node canvas, not
  // just whatever the single active request was at save time.
  const callsBeforeSave = fetchCalls;
  document.getElementById('save-view').onclick();
  assert.equal(fetchCalls, callsBeforeSave, 'saving a view must not itself fetch');

  document.getElementById('history-back').onclick();
  await settle();
  assert.ok(summary().startsWith('1 nodes'), 'moved away from the saved view');

  document.getElementById('saved-views').value = 'my-view';
  await document.getElementById('load-view').onclick();
  assert.ok(summary().startsWith('2 nodes'),
    `expected the saved view to restore both pages, got: ${summary()}`);

  console.log('continuation history regression passed');
})();
