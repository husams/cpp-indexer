import {createHash} from 'node:crypto';
import {readFile, stat} from 'node:fs/promises';
import {resolve} from 'node:path';

const root = resolve(new URL('..', import.meta.url).pathname, '..');
const readJson = async (path) => JSON.parse(await readFile(resolve(root, path), 'utf8'));
const budget = await readJson('web/performance-budget.json');
const inventory = await readJson('web/vendor/DEPENDENCY-INVENTORY.json');
const measurement = await readJson(budget.measurement);
if (measurement.format !== 'cidx.browser-performance-measurements.v1' ||
    measurement.environment.browser !== 'Chromium (Playwright)' ||
    measurement.environment.headless !== true ||
    measurement.environment.network !== 'blocked') {
  throw new Error('performance measurement environment is not pinned');
}
if (!Array.isArray(measurement.cases) || measurement.cases.length < 3 ||
    measurement.cases.some((fixture) => fixture.nodes <= 0 || fixture.edges <= 0 ||
      !fixture.shape || Object.keys(budget.runtime).some((metric) =>
        !Number.isFinite(fixture.baseline?.[metric])))) {
  throw new Error('performance measurement inputs/results are incomplete');
}
for (const fixture of measurement.cases) {
  for (const [metric, value] of Object.entries(fixture.baseline)) {
    if (value > budget.runtime[metric]) throw new Error(`${fixture.id}: baseline ${metric} exceeds budget`);
  }
}
const packages = [
  ['web/package.json', 'web/package-lock.json'],
  ['ui/package.json', 'ui/package-lock.json'],
];

for (const [packagePath, lockPath] of packages) {
  const manifest = await readJson(packagePath);
  const lock = await readJson(lockPath);
  const rootPackage = lock.packages?.[''] || {};
  for (const section of ['dependencies', 'devDependencies']) {
    for (const [name, version] of Object.entries(manifest[section] || {})) {
      if (!/^\d/.test(version)) throw new Error(`${packagePath}: ${name} is not pinned: ${version}`);
      if (rootPackage[section]?.[name] !== version) throw new Error(`${lockPath}: root specifier mismatch for ${name}`);
      const locked = lock.packages?.[`node_modules/${name}`]?.version;
      if (locked !== version) throw new Error(`${lockPath}: ${name} resolves to ${locked}, expected ${version}`);
    }
  }
}

for (const [asset, limit] of Object.entries(budget.assets)) {
  const bytes = (await stat(resolve(root, `web/${asset}`))).size;
  if (bytes > limit) throw new Error(`${asset} is ${bytes} bytes, over budget ${limit}`);
}
const cytoscape = await readFile(resolve(root, 'web/vendor/cytoscape.min.js'));
const digest = createHash('sha256').update(cytoscape).digest('hex');
const recorded = inventory.offline_assets.find((asset) => asset.path === 'web/vendor/cytoscape.min.js');
if (!recorded || recorded.sha256 !== digest) throw new Error('offline asset digest is not reproducible');
if (inventory.direct.some((entry) => !entry.package || !entry.version || !entry.license)) throw new Error('dependency inventory has an incomplete entry');
for (const entry of inventory.direct) {
  const lockPath = entry.surface.startsWith('web') ? 'web/package-lock.json' : 'ui/package-lock.json';
  const lock = await readJson(lockPath);
  const packageInfo = lock.packages?.[`node_modules/${entry.package}`];
  if (!packageInfo || packageInfo.version !== entry.version || packageInfo.license !== entry.license) {
    throw new Error(`dependency inventory does not match ${lockPath}: ${entry.package}`);
  }
}
console.log(`browser budgets passed: ${Object.keys(budget.assets).length} assets, ${inventory.direct.length} direct dependencies`);
