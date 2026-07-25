import { mkdtemp, readFile, writeFile } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { chromium } from 'playwright';

const root = resolve(new URL('..', import.meta.url).pathname);
const html = await readFile(join(root, 'index.html'), 'utf8');
const styles = await readFile(join(root, 'styles.css'), 'utf8');
const app = await readFile(join(root, 'app.js'), 'utf8');
const cytoscape = await readFile(join(root, 'vendor/cytoscape.min.js'), 'utf8');
const view = {
  schema: 'cidx.graph-view.v1', version: 1, status: 'partial',
  markers: ['truncated'], query_identity: 'query-smoke', result_id: 'result-smoke',
  identity: {workspace: 'smoke', index: 'semantic-index/schema/39', fact_sets: ['symbols', 'edges'], freshness: 'current', catalog_version: 1, catalog_hash: 'smoke'},
  request: {input_kind: 'symbol', input: 'symbol:8:smoke', node_budget: 2, edge_budget: 1, site_budget: 1, byte_budget: 4096},
  metadata: {status: 'partial', markers: ['truncated'], truncated: true, identity: {workspace: 'smoke', index: 'semantic-index/schema/39', fact_sets: ['symbols', 'edges'], freshness: 'current'}},
  nodes: [{id: 'symbol:v1:1:61:1:1:61', usr: 'usr:smoke', semantic_universe: 'test', identity_key: 'smoke', name: 'smoke()', kind: 'function', location: 'main.cpp:1', status: {completeness: 'complete', freshness: 'current', truncated: true}, color: '#65d6c3', border: '#65d6c3'}],
  edges: [], view_state: {}
};
const page = html.replace('__CIDX_STYLES__', () => styles)
  .replace('__CIDX_GRAPH_VIEW__', () => JSON.stringify(view))
  .replace('__CIDX_OFFLINE__', () => 'true')
  .replace('__CIDX_CYTOSCAPE__', () => cytoscape)
  .replace('__CIDX_APP__', () => app);
const temp = await mkdtemp(join(tmpdir(), 'cidx-ui-smoke-'));
const file = join(temp, 'snapshot.html');
await writeFile(file, page);
const candidates = process.platform === 'darwin'
  ? ['/Applications/Google Chrome.app/Contents/MacOS/Google Chrome']
  : process.platform === 'win32'
    ? ['C:/Program Files/Google/Chrome/Application/chrome.exe']
    : ['/usr/bin/google-chrome', '/usr/bin/chromium', '/usr/bin/chromium-browser'];
const executablePath = process.env.CIDX_BROWSER || candidates.find((path) => existsSync(path));
const launchOptions = {headless: true};
if (executablePath) launchOptions.executablePath = executablePath;
const browser = await chromium.launch(launchOptions);
const context = await browser.newContext();
const network = [];
await context.route('**/*', (route) => {
  if (route.request().url().startsWith('file://')) return route.continue();
  network.push(route.request().url());
  return route.abort();
});
const tab = await context.newPage();
await tab.goto(`file://${file}`);
await tab.waitForSelector('#accessible-nodes button', {state: 'attached'});
if (!(await tab.locator('#canvas-status').innerText()).includes('partial')) throw new Error('snapshot status was not rendered');
if (!(await tab.locator('#identity').innerText()).includes('query-smoke')) throw new Error('query identity was not rendered');
await tab.locator('#accessible-nodes button').first().evaluate((button) => button.click());
const selectedDetails = await tab.locator('#details').innerText();
if (!selectedDetails.includes('Canonical id')) throw new Error(`selection inspector was not rendered: ${selectedDetails}`);
if (!(await tab.locator('#expand').isDisabled())) throw new Error('offline expansion was enabled');
if (network.length !== 0) throw new Error(`offline snapshot attempted network access: ${network.join(', ')}`);
await browser.close();
console.log('offline file:// browser smoke passed with zero network requests');
