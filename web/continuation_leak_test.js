const assert = require('node:assert/strict');

// HSE-92 P1 fix: `pushMerge()` set `currentParams = params` unconditionally
// on success, and `load-more` was the only site that ever put
// `continuation: lastContinuationToken` into that params object -- nothing
// ever stripped it back out. Once "Load more" fired once, `currentParams
// .continuation` stayed set forever, and every later params-construction
// site (`expandDirection`, the search-result click, `apply-filters`) spread
// `currentParams` verbatim, carrying the stale, query-identity-bound token
// into a request for a different root/depth/direction/filters. The server
// (graph_view.cpp) correctly 400s that mismatch, `fetchGraph`/`go`/
// `pushMerge`'s catch only calls `reportError()`, and the session is then
// wedged: every further Expand/search/filter 400s until a page reload.
//
// This is a hand-rolled DOM/fetch stub harness (no jsdom/Playwright
// available in this sandbox) that `require()`s the real web/app.js browser
// IIFE end-to-end in LIVE (non-offline) mode, driving the actual "apply
// filters", tap-to-select, Expand, search-result-click, and "Load more"
// handlers -- not a re-implementation of them -- and inspects the ACTUAL
// outgoing /api/graph requests.

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
  prompt: () => 'leak-test-view',
};
global.setInterval = () => 0;
const storageBacking = new Map();
global.localStorage = {
  getItem: (key) => (storageBacking.has(key) ? storageBacking.get(key) : null),
  setItem: (key, value) => { storageBacking.set(key, String(value)); },
  removeItem: (key) => { storageBacking.delete(key); },
};

// Only the filter field this test exercises is populated, matching what
// apply-filters reads from the DOM (round 4's filter-preservation coverage).
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

const makeView = (continuation) => ({
  schema: 'cidx.graph-view.v1', version: 1, status: 'complete', markers: [],
  query_identity: 'q', result_id: `r-${Math.random()}`,
  identity: {workspace: 'w', index: 'i', fact_sets: [], freshness: 'current', catalog_version: 1, catalog_hash: 'h'},
  request: {input_kind: 'symbol', input: 'x', node_budget: 10, edge_budget: 10, site_budget: 10, byte_budget: 4096},
  metadata: {
    status: 'complete', markers: [], truncated: false, evidence_truncated: false,
    continuation, identity: {freshness: 'current'},
  },
  nodes: [{id: 'node-a', name: 'a', usr: 'usr:a', kind: 'function',
           status: {completeness: 'complete', freshness: 'current'}}],
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
  // Every graph response, including the one minted for a filtered request,
  // exposes an available continuation token bound to ITS OWN query -- this
  // is what leaks forward if a later, different request doesn't strip it.
  return {ok: true, json: async () => makeView({available: true, reason: 'budget', token: `tok:${JSON.stringify(params)}`})};
};

require('./app.js');

(async () => {
  const settle = async () => {
    for (let tick = 0; tick < 5; tick += 1) {
      await new Promise((resolve) => setImmediate(resolve));
    }
  };
  await settle(); // initial load

  // 1. Apply the node_kind filter -- becomes the active `currentParams`. The
  // response mints a continuation token bound to THIS filtered query.
  document.getElementById('apply-filters').onclick();
  await settle();
  const filtered = graphRequests.at(-1);
  assert.equal(filtered.node_kind, 'function', 'filter apply must request node_kind=function');
  assert.equal('continuation' in filtered, false, 'apply-filters itself must not carry a leftover continuation forward');

  // 2. Load more: correctly attaches the token minted for the filtered
  // query -- this is the ONE legitimate site for `continuation`.
  document.getElementById('load-more').onclick();
  await settle();
  const loadMore = graphRequests.at(-1);
  assert.ok(loadMore.continuation, 'load-more must attach the continuation token');
  assert.equal(loadMore.node_kind, 'function', 'load-more must still carry the active filter');

  // 3. Select a node and Expand in a DIFFERENT direction/root -- this is the
  // repro's core assertion: the token minted for the (filtered, out-less)
  // query above must NOT leak into this new semantic request, even though
  // the active filter legitimately should.
  assert.ok(tapHandler, 'app.js must have registered a tap handler');
  tapHandler({target: {group: () => 'nodes', data: () => ({}), id: () => 'node-a'}});
  document.getElementById('expand-in').onclick();
  await settle();
  const expanded = graphRequests.at(-1);
  assert.equal('continuation' in expanded, false,
    'Expand must not carry the stale continuation token into a new semantic request');
  assert.equal(expanded.node_kind, 'function', 'Expand must still preserve the active filter');
  assert.equal(expanded.direction, 'in');

  // 4. Load more again to mint a fresh token bound to the just-expanded
  // query, then exercise the search-result-click path the same way.
  document.getElementById('load-more').onclick();
  await settle();
  assert.ok(graphRequests.at(-1).continuation, 'load-more must attach a token again after the expand');

  document.getElementById('index-search').value = 'nodeB';
  await document.getElementById('index-search-run').onclick();
  const resultButton = createdButtons.find((button) => String(button.textContent).includes('nodeB'));
  assert.ok(resultButton, 'search must have rendered a result button for nodeB');
  resultButton.onclick();
  await settle();
  const searched = graphRequests.at(-1);
  assert.equal('continuation' in searched, false,
    'clicking a search result must not carry the stale continuation token into a new semantic request');
  assert.equal(searched.node_kind, 'function', 'search-result navigation must still preserve the active filter');
  assert.equal(searched.root, 'node-b');

  // 5. Load more once more, then re-apply filters -- apply-filters must also
  // strip a token minted by an intervening load-more, not just its own.
  document.getElementById('load-more').onclick();
  await settle();
  assert.ok(graphRequests.at(-1).continuation, 'load-more must attach a token a third time');
  document.getElementById('apply-filters').onclick();
  await settle();
  const reapplied = graphRequests.at(-1);
  assert.equal('continuation' in reapplied, false,
    'apply-filters must strip a continuation token left behind by an intervening load-more');
  assert.equal(reapplied.node_kind, 'function');

  // 6. The "Load saved view" path is the one site that assigns `currentParams`
  // straight from a recorded history entry's OWN `params` rather than going
  // through go()/pushMerge()'s `withoutContinuation()` sanitization. Save a
  // view whose LAST entry is itself a "Load more" page (so that entry's own
  // recorded params still carry the continuation token it was minted with),
  // navigate away, reload the saved view, then Expand -- this is the one
  // scenario that isolates `freshSemanticParams` from the write-time
  // `withoutContinuation()` guard already covering every other entry point,
  // and is exactly where load-view's own bug lived.
  document.getElementById('load-more').onclick();
  await settle();
  assert.ok(graphRequests.at(-1).continuation, 'load-more must attach a token before the view is saved');
  document.getElementById('save-view').onclick();
  document.getElementById('history-back').onclick();
  await settle();
  document.getElementById('saved-views').value = 'leak-test-view';
  await document.getElementById('load-view').onclick();
  tapHandler({target: {group: () => 'nodes', data: () => ({}), id: () => 'node-a'}});
  document.getElementById('expand-out').onclick();
  await settle();
  const afterLoadView = graphRequests.at(-1);
  assert.equal('continuation' in afterLoadView, false,
    'Expand after loading a saved view whose last page was itself a continuation must not resend that page\'s token');
  assert.equal(afterLoadView.node_kind, 'function', 'the filter must still survive load-view + Expand');

  console.log('continuation leak regression passed');
})();
