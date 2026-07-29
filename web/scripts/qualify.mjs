import {createHash} from 'node:crypto';
import {access, mkdtemp, readFile, rm} from 'node:fs/promises';
import {execFile} from 'node:child_process';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {promisify} from 'node:util';

const run = promisify(execFile);
const root = resolve(new URL('..', import.meta.url).pathname, '..');
const manifest = JSON.parse(await readFile(resolve(root, 'web/qualification-manifest.json'), 'utf8'));
const cidx = process.env.CIDX_BIN || resolve(root, 'build/cidx');
const record = process.argv.includes('--record');
const hashFile = async (path) => createHash('sha256').update(await readFile(path)).digest('hex');
const hashText = (text) => createHash('sha256').update(text).digest('hex');
const execute = async (args, options = {}) => run(cidx, args, {cwd: root, ...options});
const sourceFingerprint = async (paths) => {
  const {stdout} = await run('git', ['ls-files', '--', ...paths], {cwd: root});
  const digest = createHash('sha256');
  const files = stdout.trim().split('\n').filter(Boolean).sort();
  for (const path of files) {
    digest.update(path);
    digest.update('\0');
    digest.update(await readFile(resolve(root, path)));
    digest.update('\0');
  }
  return {files: files.length, digest: digest.digest('hex')};
};

if (manifest.format !== 'cidx.explorer-qualification.v2' ||
    manifest.schema.graph_view !== 'cidx.graph-view.v1') {
  throw new Error('unsupported explorer qualification manifest');
}
const requiredClasses = ['symbol', 'entity', 'include', 'type', 'high-degree',
  'multi-repository', 'stale', 'partial', 'external', 'proof', 'trace'];
for (const workspace of ['cpp-indexer', 'banking']) {
  const classes = new Set(manifest.scenarios.filter((scenario) => scenario.workspace === workspace)
    .map((scenario) => scenario.class || (scenario.id.includes('symbol') ? 'symbol' : '')));
  for (const required of requiredClasses) {
    if (!classes.has(required)) throw new Error(`${workspace}: missing ${required} production scenario`);
  }
}
const {stdout: version} = await execute(['--version']);
if (version.trim() !== manifest.cidx.version) {
  throw new Error(`CIDX version drift (${version.trim()})`);
}
const source = await sourceFingerprint(manifest.cpp_indexer_source.paths);
if (source.files !== manifest.cpp_indexer_source.file_count ||
    source.digest !== manifest.cpp_indexer_source.sha256) {
  throw new Error(`cpp-indexer source identity drift (${source.files} files, ${source.digest})`);
}

for (const input of manifest.inputs) {
  const actual = await hashFile(resolve(root, input.path));
  if (actual !== input.sha256) throw new Error(`${input.path}: immutable input drift (${actual})`);
}

const extractGraphView = (html, scenario) => {
  const match = html.match(/window\.CIDX_GRAPH_VIEW = ([\s\S]*?);\s*<\/script>/);
  if (!match) throw new Error(`${scenario.id}: export did not embed a GraphView`);
  const view = JSON.parse(match[1]);
  const names = view.nodes.map((node) => node.name).sort();
  if (view.schema !== manifest.schema.graph_view ||
      JSON.stringify(names) !== JSON.stringify([...scenario.expected.node_names].sort())) {
    throw new Error(`${scenario.id}: production GraphView drift (${JSON.stringify(names)})`);
  }
};

const workspaceDbs = new Map([['cpp-indexer', resolve(root, 'index.db')]]);
const seenSemanticOutputs = new Set();
const temporary = await mkdtemp(join(tmpdir(), 'cidx-explorer-qualification-'));
try {
  const banking = manifest.workspaces.find((workspace) => workspace.name === 'banking');
  if (!banking) throw new Error('banking workspace is missing');
  const bankingSource = resolve(root, banking.compile_commands);
  await access(bankingSource);
  const cache = join(temporary, 'banking-cache');
  await execute(['import', '--db', bankingSource, '--name', 'banking'], {env: {...process.env, INDEXER_CACHE: cache}});
  await execute(['index'], {env: {...process.env, INDEXER_CACHE: cache}});
  await execute(['resolve'], {env: {...process.env, INDEXER_CACHE: cache}});
  workspaceDbs.set('banking', join(cache, 'index.db'));

  for (const scenario of manifest.scenarios) {
    const db = workspaceDbs.get(scenario.workspace);
    if (!db) throw new Error(`${scenario.id}: workspace is not prepared`);
    const output = join(temporary, `${scenario.id}.html`);
    await execute(['ui', 'export', '--db', db, ...scenario.args, '--workspace', scenario.id, '--output', output]);
    const html = await readFile(output, 'utf8');
    extractGraphView(html, scenario);
    const match = html.match(/window\.CIDX_GRAPH_VIEW = ([\s\S]*?);\s*<\/script>/);
    const view = JSON.parse(match[1]);
    const semantic = JSON.stringify({schema: view.schema, status: view.status,
      workspace: view.identity?.workspace,
      markers: view.markers, request: view.request,
      args: [...scenario.args, '--workspace', scenario.id],
      nodes: view.nodes.map((node) => node.name).sort(),
      edges: view.edges.map((edge) => [edge.source, edge.target, edge.kind]).sort()});
    if (seenSemanticOutputs.has(semantic)) {
      throw new Error(`${scenario.id}: semantic duplicate of an earlier production scenario`);
    }
    seenSemanticOutputs.add(semantic);
    if (record) console.log(`${scenario.id} ${hashText(html)}`);
  }
} finally {
  await rm(temporary, {recursive: true, force: true});
}

console.log(`explorer qualification passed: ${manifest.scenarios.length} production exports from pinned inputs`);
