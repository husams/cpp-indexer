import {createHash} from 'node:crypto';
import {access, readFile} from 'node:fs/promises';
import {execFile} from 'node:child_process';
import {promisify} from 'node:util';
import {resolve} from 'node:path';

const run = promisify(execFile);
const root = resolve(new URL('..', import.meta.url).pathname, '..');
const readJson = async (path) => JSON.parse(await readFile(resolve(root, path), 'utf8'));
const sha256 = async (path) => createHash('sha256').update(await readFile(resolve(root, path))).digest('hex');
const manifest = await readJson('web/qualification-manifest.json');
const fixtures = await readJson(manifest.qualification_fixture);
const requiredScenarios = new Set([
  'symbol', 'entity', 'include', 'type', 'high-degree', 'multi-repository',
  'stale', 'partial', 'external', 'proof', 'trace',
]);

if (fixtures.format !== 'cidx.explorer-qualification-fixtures.v1') {
  throw new Error('qualification fixture format is not supported');
}
if (manifest.cidx.version !== '0.53.0' || manifest.cidx.schema_version !== 40) {
  throw new Error('CIDX release identity is not pinned');
}
if (!manifest.evidence.versions_are_pinned || manifest.schema.graph_view !== 'cidx.graph-view.v1') {
  throw new Error('qualification evidence is incomplete');
}

const seenScenarioKinds = new Set();
for (const scenario of manifest.scenarios) {
  if (!scenario.id || !scenario.workspace || !scenario.query || !scenario.expected) {
    throw new Error(`qualification scenario is incomplete: ${scenario.id || '<unnamed>'}`);
  }
  const kind = scenario.id.replace(/^(cpp|banking)-/, '');
  seenScenarioKinds.add(kind);
  const workspace = fixtures.workspaces[scenario.workspace];
  if (!workspace) throw new Error(`${scenario.id}: workspace fixture is missing`);
  const degree = new Map(workspace.nodes.map((node) => [node.id, 0]));
  for (const edge of workspace.edges) {
    if (scenario.query.edge_kind && edge.kind !== scenario.query.edge_kind) continue;
    degree.set(edge.source, (degree.get(edge.source) || 0) + 1);
  }
  const hasNodeQuery = ['node_kind', 'name', 'status', 'freshness', 'repositories', 'min_degree']
    .some((field) => scenario.query[field] !== undefined);
  const nodes = hasNodeQuery ? workspace.nodes.filter((node) => {
    const query = scenario.query;
    if (query.node_kind && node.kind !== query.node_kind) return false;
    if (query.name && node.name !== query.name) return false;
    if (query.status && node.status !== query.status) return false;
    if (query.freshness && node.freshness !== query.freshness) return false;
    if (query.repositories && !query.repositories.includes(node.repository)) return false;
    if (query.min_degree && (degree.get(node.id) || 0) < query.min_degree) return false;
    return true;
  }) : [];
  const edges = (scenario.query.edge_kind || scenario.query.min_degree) ? workspace.edges.filter((edge) => {
    if (scenario.query.edge_kind && edge.kind !== scenario.query.edge_kind) return false;
    if (scenario.query.min_degree && (degree.get(edge.source) || 0) < scenario.query.min_degree) return false;
    return true;
  }) : [];
  const overlay = scenario.query.overlay ? workspace.overlays[scenario.query.overlay] : null;
  const actual = {
    nodes: nodes.map((node) => node.id).sort(),
    edges: edges.map((edge) => edge.id).sort(),
    sites: edges.reduce((total, edge) => total + edge.sites.length, 0),
    ...(overlay ? {overlay_items: (overlay.claims || overlay.path || []).length} : {}),
  };
  if (JSON.stringify(actual) !== JSON.stringify(scenario.expected)) {
    throw new Error(`${scenario.id}: result drift\nexpected ${JSON.stringify(scenario.expected)}\nactual ${JSON.stringify(actual)}`);
  }
}
for (const required of requiredScenarios) {
  if (!seenScenarioKinds.has(required)) throw new Error(`qualification scenario missing: ${required}`);
}

for (const workspace of manifest.workspaces) {
  await access(resolve(root, workspace.root));
  for (const path of [workspace.index, workspace.fixture, workspace.lock].filter(Boolean)) {
    await access(resolve(root, workspace.root, path));
  }
  const expectedHashes = [
    ['index', workspace.index, workspace.index_sha256],
    ['corpus', workspace.lock, workspace.corpus_sha256],
    ['workspace', workspace.fixture, workspace.workspace_sha256],
  ];
  if (workspace.corpus_file) expectedHashes[1][1] = workspace.corpus_file;
  for (const [label, path, expected] of expectedHashes) {
    if (!expected) continue;
    const actual = await sha256(resolve(root, workspace.root, path));
    if (actual !== expected) throw new Error(`${workspace.name}: ${label} identity drift (${actual})`);
  }
  const fixture = fixtures.workspaces[workspace.name];
  if (!fixture || fixture.identity.workspace_revision !== manifest.cidx.workspace_revision) {
    throw new Error(`${workspace.name}: fixture identity is not pinned to the release revision`);
  }
}

const {stdout: revision} = await run('git', ['rev-parse', 'origin/main'], {cwd: root});
if (`git:${revision.trim()}` !== manifest.cidx.workspace_revision) {
  throw new Error(`main revision drift (${revision.trim()})`);
}
console.log(`explorer qualification passed: ${manifest.workspaces.length} pinned workspaces, ${manifest.scenarios.length} executable scenarios, no result drift`);
