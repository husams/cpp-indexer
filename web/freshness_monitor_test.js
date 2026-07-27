const assert = require('node:assert/strict');

// HSE-92 round 2 review fix: staleness must be detected promptly while the
// session sits idle, not only reactively on the NEXT user-initiated GraphView
// action. This is a hand-rolled DOM/fetch stub harness (no jsdom/Playwright
// available in this sandbox) that `require()`s the real web/app.js browser
// IIFE end-to-end in LIVE (non-offline) mode, capturing the interval the
// freshness monitor registers so the test can fire it manually (instead of
// waiting on a real 15s timer) and assert the stale banner appears on its
// own -- without any addView()/canvas mutation -- purely from the probe's
// reported identity freshness.

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
};

global.cytoscape = () => ({
  add() {},
  nodes() {
    return {forEach() {}, map() { return []; }, length: 0};
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

// Capture whatever interval(s) app.js registers instead of letting a real
// 15-second timer run; the test fires the captured callback directly.
const intervals = [];
global.setInterval = (fn) => {
  intervals.push(fn);
  return intervals.length;
};

// One node, freshness driven by `currentFreshness` so the test can flip the
// workspace from current to stale between probes.
let currentFreshness = 'current';
let fetchCalls = 0;
global.fetch = async () => {
  fetchCalls += 1;
  return {
    ok: true,
    json: async () => ({
      schema: 'cidx.graph-view.v1', version: 1, status: 'complete', markers: [],
      query_identity: 'q', result_id: 'r',
      identity: {
        workspace: 'w', index: 'i', fact_sets: [], freshness: currentFreshness,
        catalog_version: 1, catalog_hash: 'h',
      },
      request: {input_kind: 'symbol', input: 'x', node_budget: 10, edge_budget: 10,
                site_budget: 10, byte_budget: 4096},
      metadata: {
        status: 'complete', markers: [], truncated: false, evidence_truncated: false,
        continuation: {available: false, reason: 'complete'},
        identity: {freshness: currentFreshness},
      },
      nodes: [{id: 'n1', name: 'a', usr: 'usr:a', kind: 'function',
               status: {completeness: 'complete', freshness: currentFreshness}}],
      edges: [],
      view_state: {},
    }),
  };
};

require('./app.js');

(async () => {
  // start()'s live-mode path awaits a real fetchGraph({}) before registering
  // the freshness monitor; let those pending microtasks settle.
  for (let tick = 0; tick < 5; tick += 1) {
    await new Promise((resolve) => setImmediate(resolve));
  }

  assert.equal(intervals.length, 1,
    'startFreshnessMonitor() must register exactly one interval in live mode');

  const banner = document.getElementById('stale-banner');
  assert.equal(banner.hidden, true, 'no staleness yet: banner starts hidden');
  const callsBeforeProbe = fetchCalls;

  // The indexed workspace identity changes while the session sits idle --
  // no user action, just the next scheduled probe.
  currentFreshness = 'stale';
  await intervals[0]();

  assert.ok(fetchCalls > callsBeforeProbe,
    'the freshness monitor must actually probe the server on its interval');
  assert.equal(banner.hidden, false,
    'staleness must surface from the idle probe alone, with no explicit user action');

  console.log('freshness monitor regression passed');
})();
