const assert = require('node:assert/strict');
const {overlayState} = require('./app.js');

const identity = {resultId: 'result:overlay:v1', evidenceId: 'ev:proof', limitation: 'producer supplied bounded fixture'};
const view = {
  metadata: {
    overlays: {
      exportMaxBytes: 4096,
      proof: {
        kind: 'proof', title: 'Proof obligations',
        claims: [
          {id: 'claim:root', label: 'Safety invariant', state: 'refuted', identity},
          {id: 'claim:open', label: 'Open obligation', state: 'open', identity},
        ], trustedModels: ['model-v1'], assumptions: ['external model'],
      },
      counterexample: {
        kind: 'counterexample', title: 'Safety violation', specification: 'Spec', violation: 'Invariant false',
        steps: [
          {index: 0, action: 'Init', state: [{name: 'valid', value: 'TRUE'}], identity},
          {index: 1, action: 'Break', state: [{name: 'valid', value: 'FALSE'}], identity},
        ],
      },
    },
  },
};

assert.equal(overlayState.get(view, 'proof').claims[0].state, 'refuted');
assert.equal(overlayState.items(overlayState.get(view, 'counterexample')).length, 2);
const snapshot = overlayState.snapshot(overlayState.get(view, 'counterexample'), 4096);
assert.ok(new TextEncoder().encode(snapshot).byteLength <= 4096);
assert.match(snapshot, /"state":\[\{"name":"valid","value":"FALSE"\}\]/);
assert.match(overlayState.identityText(identity), /result:overlay:v1/);
console.log('overlay state regression passed');
