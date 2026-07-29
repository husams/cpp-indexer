import {mkdtemp, readFile, writeFile} from 'node:fs/promises';
import {existsSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {chromium} from 'playwright';

const root = resolve(new URL('..', import.meta.url).pathname, '..');
const budget = JSON.parse(await readFile(resolve(root, 'web/performance-budget.json'), 'utf8'));
const measurement = JSON.parse(await readFile(resolve(root, budget.measurement), 'utf8'));
const html = await readFile(resolve(root, 'web/index.html'), 'utf8');
const styles = await readFile(resolve(root, 'web/styles.css'), 'utf8');
const app = await readFile(resolve(root, 'web/app.js'), 'utf8');
const cytoscape = await readFile(resolve(root, 'web/vendor/cytoscape.min.js'), 'utf8');

const makeView = (fixture) => {
  const nodes = Array.from({length: fixture.nodes}, (_, index) => ({
    id: `node-${index}`, name: `Node ${index}`, usr: `usr-${index}`, kind: 'function',
    location: `fixture.cpp:${index + 1}`, status: {completeness: 'complete'},
    color: '#65d6c3', border: '#65d6c3',
  }));
  const edges = Array.from({length: fixture.edges}, (_, index) => {
    const source = fixture.shape === 'one-high-degree-root' ? 'node-0' : `node-${index % fixture.nodes}`;
    const target = `node-${(index + 1) % fixture.nodes}`;
    return {id: `edge-${index}`, source, target, kind: 'calls', sites: [{location: `fixture.cpp:${index + 1}`}]};
  });
  return {
    schema: 'cidx.graph-view.v1', version: 1, status: 'complete', markers: [],
    query_identity: `performance-${fixture.id}`, result_id: `performance-${fixture.id}`,
    identity: {workspace: 'performance', index: 'pinned-fixture', fact_sets: ['symbols', 'edges'], freshness: 'current'},
    request: {input_kind: 'symbol', input: 'Node 0', node_budget: 2500, edge_budget: 5000, site_budget: 10000, byte_budget: 8388608},
    metadata: {status: 'complete', identity: {workspace: 'performance', freshness: 'current'}},
    nodes, edges, view_state: {},
  };
};

const browserPath = process.platform === 'darwin'
  ? ['/Applications/Google Chrome.app/Contents/MacOS/Google Chrome']
  : process.platform === 'win32'
    ? ['C:/Program Files/Google/Chrome/Application/chrome.exe']
    : ['/usr/bin/google-chrome', '/usr/bin/chromium', '/usr/bin/chromium-browser'];
const executablePath = process.env.CIDX_BROWSER || browserPath.find((path) => existsSync(path));
const browser = await chromium.launch({headless: true, ...(executablePath ? {executablePath} : {}), args: ['--enable-precise-memory-info']});
const results = [];
try {
  for (const fixture of measurement.cases) {
    const view = makeView(fixture);
    const page = await browser.newPage({viewport: {width: 1280, height: 900}});
    await page.route('**/*', (route) => route.request().url().startsWith('file://') ? route.continue() : route.abort());
    const snapshot = html.replace('__CIDX_STYLES__', () => styles)
      .replace('__CIDX_GRAPH_VIEW__', () => JSON.stringify(view))
      .replace('__CIDX_OFFLINE__', () => 'true')
      .replace('__CIDX_CYTOSCAPE__', () => cytoscape)
      .replace('__CIDX_APP__', () => app);
    const temp = await mkdtemp(join(tmpdir(), 'cidx-ui-performance-'));
    const file = join(temp, `${fixture.id}.html`);
    await writeFile(file, snapshot);
    const start = Date.now();
    await page.goto(`file://${file}`);
    await page.waitForSelector('#accessible-nodes button', {state: 'attached'});
    const load_ms = Date.now() - start;
    const layoutStart = Date.now();
    await page.waitForSelector('#cy canvas, #cy svg', {state: 'attached'});
    const layout_ms = Date.now() - layoutStart;
    const panZoomStart = Date.now();
    await page.locator('#fit').evaluate((button) => button.click());
    await page.mouse.move(640, 450);
    await page.mouse.wheel(0, -120);
    await page.mouse.wheel(0, 120);
    const pan_zoom_ms = Date.now() - panZoomStart;
    const selectionStart = Date.now();
    await page.locator('#accessible-nodes button').first().evaluate((button) => button.click());
    await page.waitForFunction(() => document.querySelector('#details')?.textContent?.includes('Canonical id'));
    const selection_ms = Date.now() - selectionStart;
    const updateStart = Date.now();
    await page.locator('#search').fill('Node 0');
    await page.evaluate(() => new Promise((resolve) => requestAnimationFrame(resolve)));
    const progressive_update_ms = Date.now() - updateStart;
    const memory_mb = await page.evaluate(() => (performance.memory?.usedJSHeapSize || 0) / 1024 / 1024);
    const export_bytes = Buffer.byteLength(await page.content());
    if (memory_mb === 0) throw new Error(`${fixture.id}: Chromium memory measurement unavailable`);
    const result = {id: fixture.id, nodes: fixture.nodes, edges: fixture.edges, load_ms, layout_ms, pan_zoom_ms, selection_ms, progressive_update_ms, memory_mb: Number(memory_mb.toFixed(2)), export_bytes};
    for (const [name, limit] of Object.entries(budget.runtime)) {
      if (name === 'memory_mb' && result[name] > limit) throw new Error(`${fixture.id}: ${name} ${result[name]} > ${limit}`);
      if (name === 'export_bytes' && result[name] > limit) throw new Error(`${fixture.id}: ${name} ${result[name]} > ${limit}`);
      if (name.endsWith('_ms') && result[name] > limit) throw new Error(`${fixture.id}: ${name} ${result[name]} > ${limit}`);
    }
    results.push(result);
    await page.close();
  }
} finally {
  await browser.close();
}
console.log(`browser performance measurements passed: ${results.map(({id, load_ms, layout_ms}) => `${id} load=${load_ms}ms layout=${layout_ms}ms`).join(', ')}`);
console.log(JSON.stringify(results));
