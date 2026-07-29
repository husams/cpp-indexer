import {access, readFile} from 'node:fs/promises';
import {resolve} from 'node:path';

const root = resolve(new URL('..', import.meta.url).pathname, '..');
const manifest = JSON.parse(await readFile(resolve(root, 'web/qualification-manifest.json'), 'utf8'));
const requiredScenarios = new Set(['symbol', 'entity', 'include', 'type', 'high-degree', 'multi-repository', 'stale', 'partial', 'external', 'proof', 'trace']);
for (const scenario of requiredScenarios) {
  if (!manifest.scenarios.includes(scenario)) throw new Error(`qualification scenario missing: ${scenario}`);
}
for (const workspace of manifest.workspaces) {
  await access(resolve(root, workspace.root));
  for (const path of [workspace.index, workspace.fixture, workspace.lock].filter(Boolean)) await access(resolve(root, workspace.root, path));
}
if (!manifest.evidence.versions_are_pinned || manifest.schema.graph_view !== 'cidx.graph-view.v1') throw new Error('qualification evidence is incomplete');
console.log(`explorer qualification manifest passed: ${manifest.workspaces.length} workspaces, ${manifest.scenarios.length} scenarios`);
