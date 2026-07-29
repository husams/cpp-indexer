import {createHash} from 'node:crypto';
import {access, copyFile, mkdir, mkdtemp, readFile, rm, writeFile} from 'node:fs/promises';
import {execFile} from 'node:child_process';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {promisify} from 'node:util';

const run = promisify(execFile);
const root = resolve(new URL('..', import.meta.url).pathname, '..');
const manifest = JSON.parse(await readFile(resolve(root, 'web/qualification-manifest.json'), 'utf8'));
const cidx = process.env.CIDX_BIN || resolve(root, 'build/cidx');
const hashFile = async (path) => createHash('sha256').update(await readFile(path)).digest('hex');
const hashText = (text) => createHash('sha256').update(text).digest('hex');
const execute = async (args, options = {}) => run(cidx, args, {cwd: root, ...options});
const sqlite = async (db, sql) => run('sqlite3', [db, sql], {cwd: root});
const sqliteJson = async (db, sql) => {
  const {stdout} = await run('sqlite3', ['-json', db, sql], {cwd: root});
  return JSON.parse(stdout || '[]');
};

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
    .map((scenario) => scenario.class));
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

const nodeRecord = (node) => {
  if (typeof node === 'string') return {name: node};
  return {
    name: node.name,
    kind: node.kind,
    component: node.component,
    file: node.file,
    external: node.external ?? node.status?.external ?? false,
    resolved: node.resolved ?? node.status?.resolved ?? false,
    status: node.status ? {
      completeness: node.status.completeness,
      freshness: node.status.freshness,
      external: node.status.external,
      resolved: node.status.resolved,
      truncated: node.status.truncated,
      pagination_truncated: node.status.pagination_truncated,
    } : undefined,
  };
};

const extractGraphView = (html, scenario) => {
  const match = html.match(/window\.CIDX_GRAPH_VIEW = ([\s\S]*?);\s*<\/script>/);
  if (!match) throw new Error(`${scenario.id}: export did not embed a GraphView`);
  const view = JSON.parse(match[1]);
  if (view.schema !== manifest.schema.graph_view) {
    throw new Error(`${scenario.id}: unexpected GraphView schema`);
  }
  const records = view.nodes.map(nodeRecord);
  const names = records.map((node) => node.name).sort();
  const behavior = scenario.expected.behavior || {};
  if (scenario.expected.node_names &&
      JSON.stringify(names) !== JSON.stringify([...scenario.expected.node_names].sort())) {
    throw new Error(`${scenario.id}: node-name drift (${JSON.stringify(names)})`);
  }
  if (behavior.status !== undefined && view.status !== behavior.status) {
    throw new Error(`${scenario.id}: expected status ${behavior.status}, got ${view.status}`);
  }
  if (behavior.markers !== undefined &&
      JSON.stringify([...view.markers].sort()) !== JSON.stringify([...behavior.markers].sort())) {
    throw new Error(`${scenario.id}: marker drift (${JSON.stringify(view.markers)})`);
  }
  if (behavior.input_kind !== undefined && view.request.input_kind !== behavior.input_kind) {
    throw new Error(`${scenario.id}: expected ${behavior.input_kind} input, got ${view.request.input_kind}`);
  }
  if (behavior.fact_sets !== undefined) {
    for (const factSet of behavior.fact_sets) {
      if (!view.identity.fact_sets.includes(factSet)) {
        throw new Error(`${scenario.id}: missing fact set ${factSet}`);
      }
    }
  }
  if (behavior.min_nodes !== undefined && view.nodes.length < behavior.min_nodes) {
    throw new Error(`${scenario.id}: expected at least ${behavior.min_nodes} nodes`);
  }
  if (behavior.min_edges !== undefined && view.edges.length < behavior.min_edges) {
    throw new Error(`${scenario.id}: expected at least ${behavior.min_edges} edges`);
  }
  if (behavior.min_sites !== undefined &&
      view.edges.reduce((total, edge) => total + (edge.sites?.length || 0), 0) < behavior.min_sites) {
    throw new Error(`${scenario.id}: expected site evidence`);
  }
  if (behavior.edge_kinds !== undefined) {
    const kinds = [...new Set(view.edges.map((edge) => edge.kind))].sort();
    if (JSON.stringify(kinds) !== JSON.stringify([...behavior.edge_kinds].sort())) {
      throw new Error(`${scenario.id}: edge-kind drift (${JSON.stringify(kinds)})`);
    }
  }
  if (behavior.external_nodes !== undefined) {
    const externalNodes = records.filter((node) => node.external).length;
    if (externalNodes !== behavior.external_nodes) {
      throw new Error(`${scenario.id}: expected ${behavior.external_nodes} external nodes, got ${externalNodes}`);
    }
  }
  if (behavior.identity_freshness !== undefined &&
      view.identity.freshness !== behavior.identity_freshness) {
    throw new Error(`${scenario.id}: freshness drift (${view.identity.freshness})`);
  }
  if (behavior.source_fingerprint !== undefined &&
      view.identity.source_fingerprint !== behavior.source_fingerprint) {
    throw new Error(`${scenario.id}: source fingerprint drift`);
  }
  return {view, records, names};
};

const upsertMeta = (db, key, value) =>
  sqlite(db, `INSERT INTO meta(key,value) VALUES('${key}','${value}') ` +
    `ON CONFLICT(key) DO UPDATE SET value=excluded.value;`);

const prepareScenarioState = async (db, state) => {
  if (state === 'stale') {
    await upsertMeta(db, 'source_fingerprint', 'qualification-stale');
    await upsertMeta(db, 'source_revision', 'content-sha1:qualification-stale');
    return;
  }
  if (state === 'entity') {
    await sqlite(db, "INSERT OR IGNORE INTO entity_node(id,kind) " +
      "SELECT id,1 FROM symbol WHERE usr IN " +
      "('c:@N@cidx@N@ui@S@GraphViewRequest','c:@S@AccountState');");
    return;
  }
  if (state === 'external') {
    await sqlite(db, "INSERT OR IGNORE INTO symbol(" +
      "usr,spelling,qual_name,display_name,kind,type_info,file_id,line,col," +
      "end_line,end_col,decl_file_id,decl_line,decl_col,decl_path," +
      "is_definition,is_pure,is_static,is_instantiation,callable_kind," +
      "template_origin,template_form,is_named_instance,linkage,access,parent_usr," +
      "parent_id,resolved,multi_def,const_value,semantic_universe_id,identity_key) " +
      "SELECT 'c:@F@qualification_external_ledger#','qualification_external_ledger'," +
      "'qualification_external_ledger()','qualification_external_ledger()',kind," +
      "type_info,NULL,NULL,NULL,NULL,NULL,NULL,1,1,'/usr/include/qualification/ledger.h'," +
      "0,0,0,0,callable_kind,template_origin,template_form,is_named_instance," +
      "'external',access,NULL,NULL,0,0,NULL,semantic_universe_id," +
      "'qualification^_c:@F@qualification_external_ledger#' FROM symbol LIMIT 1;");
  }
};

const scenarioArgs = (scenario) => scenario.args.map((arg) => {
  if (arg === '{{banking-peer-root}}') return '/tmp/cidx-hse94-banking-peer';
  if (arg === '{{banking-peer-file}}') return '/tmp/cidx-hse94-banking-peer/banking_peer.cpp';
  return arg;
});

const repositorySummary = async (db) => sqliteJson(db,
  'SELECT r.name, r.semantic_universe_id, COUNT(DISTINCT f.id) AS files, ' +
  'COUNT(DISTINCT CASE WHEN f.indexed = 1 THEN f.id END) AS indexed_files, ' +
  'COUNT(DISTINCT s.id) AS symbols, ' +
  'COUNT(DISTINCT CASE WHEN f.indexed = 1 THEN s.id END) AS indexed_symbols ' +
  'FROM repository r ' +
  'JOIN component c ON c.repository_id = r.id ' +
  'JOIN directory d ON d.component_id = c.id ' +
  'JOIN file f ON f.directory_id = d.id ' +
  'LEFT JOIN symbol s ON s.file_id = f.id ' +
  'GROUP BY r.id, r.name, r.semantic_universe_id ORDER BY r.name');

const semanticOutput = (view, records) => JSON.stringify({
  schema: view.schema,
  status: view.status,
  markers: [...view.markers].sort(),
  identity: {
    workspace: view.identity.workspace,
    freshness: view.identity.freshness,
    source_revision: view.identity.source_revision,
    source_fingerprint: view.identity.source_fingerprint,
    fact_sets: [...view.identity.fact_sets].sort(),
  },
  nodes: records.map((node) => ({
    name: node.name,
    kind: node.kind,
    component: node.component,
    file: node.file,
    external: node.external,
    resolved: node.resolved,
    status: node.status,
  })).sort((left, right) => JSON.stringify(left).localeCompare(JSON.stringify(right))),
  edges: view.edges.map((edge) => ({
    source: edge.source,
    target: edge.target,
    kind: edge.kind,
    sites: (edge.sites || []).map((site) => ({file: site.file, line: site.line, col: site.col})),
  })).sort((left, right) => JSON.stringify(left).localeCompare(JSON.stringify(right))),
});

const workspaceDbs = new Map();
const scenarioDbs = new Map();
const seenSemanticOutputs = new Map();
const temporary = await mkdtemp(join(tmpdir(), 'cidx-explorer-qualification-'));
try {
  const cppDb = resolve(root, 'index.db');
  workspaceDbs.set('cpp-indexer', cppDb);
  const banking = manifest.workspaces.find((workspace) => workspace.name === 'banking');
  if (!banking) throw new Error('banking workspace is missing');
  const bankingSource = resolve(root, banking.compile_commands);
  const peerSource = resolve(root, 'examples/packages/banking/qualification-peer/compile_commands.json');
  await access(bankingSource);
  const peerWorkspace = '/tmp/cidx-hse94-banking-peer';
  const peerFile = join(peerWorkspace, 'banking_peer.cpp');
  await rm(peerWorkspace, {recursive: true, force: true});
  await mkdir(peerWorkspace, {recursive: true});
  await copyFile(resolve(root, 'examples/packages/banking/qualification-peer/banking_peer.cpp'), peerFile);
  await writeFile(join(peerWorkspace, 'compile_commands.json'), JSON.stringify([{
    directory: peerWorkspace,
    command: 'clang++ -std=c++23 -c banking_peer.cpp',
    file: peerFile,
  }]));
  await access(peerSource);
  const bankingCache = join(temporary, 'banking-cache');
  await execute(['import', '--db', bankingSource, '--name', 'banking',
    '--repo', 'banking', '--universe', 'qualification:banking'],
    {env: {...process.env, INDEXER_CACHE: bankingCache}});
  await sqlite(join(bankingCache, 'index.db'),
    "UPDATE repository SET remote_url='https://qualification.invalid/banking.git' " +
    "WHERE name='banking';");
  await execute(['index'], {env: {...process.env, INDEXER_CACHE: bankingCache}});
  await execute(['resolve'], {env: {...process.env, INDEXER_CACHE: bankingCache}});
  const bankingDb = join(bankingCache, 'index.db');
  workspaceDbs.set('banking', bankingDb);

  for (const workspace of ['cpp-indexer', 'banking']) {
    const multiCache = join(temporary, `${workspace}-multi-cache`);
    await mkdir(multiCache, {recursive: true});
    await copyFile(workspaceDbs.get(workspace), join(multiCache, 'index.db'));
    await execute(['import', '--db', join(peerWorkspace, 'compile_commands.json'),
      '--name', 'banking-peer', '--repo', 'banking-peer',
      '--universe', 'qualification:banking-peer'],
      {env: {...process.env, INDEXER_CACHE: multiCache}});
    await sqlite(join(multiCache, 'index.db'),
      "UPDATE repository SET remote_url='https://qualification.invalid/banking-peer.git' " +
      "WHERE name='banking-peer';");
    await execute(['index', peerFile], {env: {...process.env, INDEXER_CACHE: multiCache}});
    await execute(['resolve'], {env: {...process.env, INDEXER_CACHE: multiCache}});
    scenarioDbs.set(`${workspace}:multi-repository`, join(multiCache, 'index.db'));
  }

  for (const scenario of manifest.scenarios) {
    const baseDb = workspaceDbs.get(scenario.workspace);
    if (!baseDb) throw new Error(`${scenario.id}: workspace is not prepared`);
    let db = scenarioDbs.get(`${scenario.workspace}:${scenario.class}`) || baseDb;
    if (scenario.state === 'stale') {
      db = join(temporary, `${scenario.id}.db`);
      await copyFile(baseDb, db);
    }
    if (scenario.state === 'entity' || scenario.state === 'external') {
      db = join(temporary, `${scenario.id}.db`);
      await copyFile(baseDb, db);
    }
    await prepareScenarioState(db, scenario.state);
    const output = join(temporary, `${scenario.id}.html`);
    await execute(['ui', 'export', '--db', db, ...scenarioArgs(scenario), '--output', output]);
    const html = await readFile(output, 'utf8');
    const {view, records} = extractGraphView(html, scenario);
    const expectedRepositories = scenario.expected.repository_names;
    if (expectedRepositories) {
      const actualRepositories = await repositorySummary(db);
      for (const repository of expectedRepositories) {
        const actual = actualRepositories.find((entry) => entry.name === repository);
        if (!actual || actual.files === 0 || actual.indexed_files === 0 ||
            actual.symbols === 0 || actual.indexed_symbols === 0) {
          throw new Error(`${scenario.id}: repository ${repository} is not present`);
        }
      }
    }
    if (scenario.expected.behavior?.repository_filter !== undefined &&
        JSON.stringify(view.request.repositories || []) !==
        JSON.stringify([...scenario.expected.behavior.repository_filter].sort())) {
      throw new Error(`${scenario.id}: repository selection drift`);
    }
    if (scenario.expected.behavior?.selected_component !== undefined) {
      const components = [...new Set(records.map((node) => node.component).filter(Boolean))];
      if (components.length !== 1 || components[0] !== scenario.expected.behavior.selected_component) {
        throw new Error(`${scenario.id}: repository selection leaked into ${components.join(',')}`);
      }
    }
    const workspaceSeen = seenSemanticOutputs.get(scenario.workspace) || new Set();
    const semantic = semanticOutput(view, records);
    if (workspaceSeen.has(semantic)) {
      throw new Error(`${scenario.id}: semantic duplicate of an earlier production scenario`);
    }
    workspaceSeen.add(semantic);
    seenSemanticOutputs.set(scenario.workspace, workspaceSeen);
    if (!scenario.expected.export_sha256) {
      throw new Error(`${scenario.id}: missing pinned export hash`);
    }
    const actualExportHash = hashText(html);
    if (actualExportHash !== scenario.expected.export_sha256) {
      const diagnostics = {
        actual_export_sha256: actualExportHash,
        semantic_sha256: hashText(semantic),
        identity: view.identity,
        nodes: records,
        edges: view.edges.map((edge) => ({
          source: edge.source,
          target: edge.target,
          kind: edge.kind,
          sites: edge.sites,
        })),
      };
      throw new Error(`${scenario.id}: exported explorer output drift ` +
        `(expected ${scenario.expected.export_sha256}, got ${actualExportHash})\n` +
        JSON.stringify(diagnostics));
    }
  }
} finally {
  await rm(temporary, {recursive: true, force: true});
}

console.log(`explorer qualification passed: ${manifest.scenarios.length} production exports from pinned inputs`);
