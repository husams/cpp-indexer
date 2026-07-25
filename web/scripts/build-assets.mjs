import { createHash } from 'node:crypto';
import { readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';

const root = resolve(new URL('..', import.meta.url).pathname);
const source = resolve(root, 'node_modules/cytoscape/dist/cytoscape.min.js');
const target = resolve(root, 'vendor/cytoscape.min.js');
const manifestPath = resolve(root, 'vendor/LICENSES.json');

const bytes = await readFile(source);
const digest = createHash('sha256').update(bytes).digest('hex');
const manifest = JSON.parse(await readFile(manifestPath, 'utf8'));
const asset = manifest.assets.find((entry) => entry.name === 'cytoscape.min.js');
if (!asset || asset.version !== '3.31.2' || asset.license !== 'MIT' ||
    asset.license_file !== 'CYTOSCAPE-LICENSE') {
  throw new Error('Cytoscape asset manifest is incomplete or mismatched');
}
if (asset.sha256 !== digest) {
  throw new Error(`Cytoscape digest mismatch: manifest=${asset.sha256} built=${digest}`);
}

if (process.argv.includes('--check')) {
  const checkedIn = await readFile(target);
  if (!checkedIn.equals(bytes)) {
    throw new Error('checked-in Cytoscape bundle differs from the reproducible build');
  }
} else {
  await writeFile(target, bytes);
}
console.log(`verified cytoscape ${asset.version} ${digest}`);
