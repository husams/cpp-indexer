const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');

function element() {
  const state = {value: '', textContent: '', innerHTML: '', hidden: false, disabled: false};
  return new Proxy(state, {
    get(target, prop) {
      if (prop in target) return target[prop];
      if (prop === 'appendChild' || prop === 'replaceChildren' || prop === 'addEventListener') return () => {};
      if (prop === 'setAttribute') return () => {};
      if (prop === 'querySelectorAll') return () => [];
      if (prop === 'insertAdjacentHTML') return (_position, value) => { target.innerHTML += value; };
      if (prop === 'remove') return () => {};
      if (prop === 'classList') return {add() {}, remove() {}, toggle() {}};
      if (prop === 'style' || prop === 'dataset') return {};
      return undefined;
    },
    set(target, prop, value) {
      target[prop] = value;
      return true;
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
global.navigator = {clipboard: {writeText: () => Promise.resolve()}};
global.localStorage = {getItem: () => null, setItem() {}, removeItem() {}};
global.setInterval = () => 0;
global.window = {
  CIDX_OFFLINE: true,
  location: {search: ''},
  CIDX_GRAPH_VIEW: {
    schema: 'cidx.graph-view.v1', version: 1, status: 'partial',
    markers: ['<script>alert(1)</script>'], query_identity: 'q', result_id: 'r',
    identity: {workspace: 'w', index: 'i', fact_sets: [], freshness: 'current'},
    request: {input_kind: 'symbol', input: '<img src=x>', node_budget: 2, edge_budget: 1, site_budget: 1, byte_budget: 4096},
    metadata: {status: 'partial', markers: [], truncated: false, identity: {freshness: 'current'}},
    nodes: [{id: 'n1', name: '<img src=x onerror=alert(1)>', usr: 'usr', kind: 'function', file: '/tmp/<script>.cpp', line: 1, status: {completeness: 'partial'}}],
    edges: [{id: 'e1', source: 'n1', target: 'n1', kind: 'calls', sites: [{location: 'file://unsafe/<script>.cpp:1'}]}],
  },
};
let cytoscapeCalls = 0;
global.cytoscape = () => {
  cytoscapeCalls += 1;
  return {
    add() {}, nodes() { return {forEach() {}, map() { return []; }, length: 0, reduce: (_fn, init) => init, positions() {}}; },
    $: () => ({length: 0, remove() {}}), $id: () => ({length: 0}), on() {}, fit() {}, animate() {},
    layout: () => ({one() {}, run() {}}), zoom: () => 1, pan: () => ({x: 0, y: 0}), elements: () => ({removeClass() {}}),
  };
};

const html = readFileSync('index.html', 'utf8');
assert.match(html, /frame-ancestors 'none'/);
assert.match(html, /style-src 'nonce-cidx-static'/);
assert.match(html, /id="text-alternative"/);
assert.match(html, /id="selection-title" tabindex="-1"/);
require('./app.js');

const details = registry.get('details').innerHTML;
const alternative = registry.get('text-alternative-content').innerHTML;
assert.doesNotMatch(details, /<(?:img|script)\b/i);
assert.doesNotMatch(alternative, /<(?:img|script)\b/i);
assert.match(alternative, /Visual absence is never proof/);
assert.equal(cytoscapeCalls, 1, 'bounded hostile content should still render safely');

console.log('security and accessibility regression passed');
