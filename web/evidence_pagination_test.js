const assert = require('node:assert/strict');

function element() {
  const state = {
    value: '', hidden: false, disabled: false, innerHTML: '', removed: false,
  };
  return new Proxy(state, {
    get(target, prop) {
      if (prop in target) return target[prop];
      if (prop === 'appendChild') return () => {};
      if (prop === 'replaceChildren') return () => {};
      if (prop === 'addEventListener') return () => {};
      if (prop === 'setAttribute') return () => {};
      if (prop === 'insertAdjacentHTML') return () => {};
      if (prop === 'remove') return () => { target.removed = true; };
      if (prop === 'querySelectorAll') return () => [];
      if (prop === 'classList') return {add() {}, remove() {}, toggle() {}};
      if (prop === 'style' || prop === 'dataset') return {};
      return undefined;
    },
  });
}

const registry = new Map();
global.document = {
  getElementById(id) {
    if (!registry.has(id)) registry.set(id, element());
    return registry.get(id);
  },
  createElement: () => element(),
  querySelectorAll: () => [],
};
global.window = {CIDX_OFFLINE: false, location: {search: '?token=test-token'}};
global.navigator = {clipboard: {writeText: () => Promise.resolve()}};
global.localStorage = {getItem: () => null, setItem() {}, removeItem() {}};
global.setInterval = () => 0;

let tapHandler;
global.cytoscape = () => ({
  add() {},
  nodes() {
    return {forEach() {}, map: () => [], reduce: (_fn, init) => init,
            positions() {}, length: 0};
  },
  $: () => ({remove() {}, length: 0}),
  $id: () => ({length: 0}),
  on(event, _selector, handler) { if (event === 'tap') tapHandler = handler; },
  fit() {}, animate() {}, zoom: () => 1, pan: () => ({x: 0, y: 0}),
  elements: () => ({removeClass() {}}),
});

const edge = {
  id: 'edge-1', source: 'node-a', target: 'node-b', kind: 'calls', count: 5002,
  sites: [{file: 'a.cpp', line: 1, col: 1}],
  status: {evidence_truncated: true},
};
const view = {
  schema: 'cidx.graph-view.v1', version: 1, status: 'partial', markers: [],
  query_identity: 'q', result_id: 'r',
  identity: {workspace: 'w', index: 'i', fact_sets: [], freshness: 'current'},
  request: {input_kind: 'symbol', input: 'x', node_budget: 2, edge_budget: 1,
            site_budget: 1, byte_budget: 4096},
  metadata: {status: 'partial', markers: [], truncated: true,
             continuation: {available: false}, identity: {freshness: 'current'}},
  nodes: [
    {id: 'node-a', name: 'a', kind: 'function', status: {}},
    {id: 'node-b', name: 'b', kind: 'function', status: {}},
  ],
  edges: [edge], view_state: {},
};
const offsets = [];
global.fetch = async (url) => {
  if (!url.startsWith('/api/evidence?')) {
    return {ok: true, json: async () => view};
  }
  const params = new URLSearchParams(url.split('?')[1]);
  offsets.push(params.get('site_offset'));
  if (offsets.length === 1) {
    return {
      ok: true,
      json: async () => ({
        truncated: true, next_offset: 5001,
        sites: Array.from({length: 5000}, (_, index) => ({
          file: 'a.cpp', line: index + 2, col: 1,
        })),
      }),
    };
  }
  return {
    ok: true,
    json: async () => ({
      truncated: false, next_offset: 5002,
      sites: [{file: 'a.cpp', line: 5002, col: 1}],
    }),
  };
};

require('./app.js');

(async () => {
  const settle = async () => {
    for (let tick = 0; tick < 5; tick += 1) {
      await new Promise((resolve) => setImmediate(resolve));
    }
  };
  await settle();
  tapHandler({target: {group: () => 'edges', data: () => edge}});
  const button = document.getElementById('load-evidence');
  await button.onclick();
  assert.equal(button.removed, false,
    'a truncated evidence page must retain the continuation action');
  await button.onclick();
  assert.deepEqual(offsets, ['1', '5001']);
  assert.equal(button.removed, true);
  console.log('evidence pagination regression passed');
})();
