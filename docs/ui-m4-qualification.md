# Graph explorer M4 qualification

The supported explorer surface is the loopback live server plus the
self-contained offline export. Both use the versioned `cidx.graph-view.v1`
payload and the same redaction, status, evidence, and budget rules.

## Release evidence

The checked-in [qualification manifest](../web/qualification-manifest.json)
records the pinned CIDX version/schema, catalog digest, committed `index.db`,
scoped `src`/`tests`/`manifests` source-tree fingerprint, and banking corpus
hashes. It deliberately does not compare a moving remote
branch. The executable scenarios build the banking corpus and invoke the
production `cidx ui export` command for both workspaces, then verify the
embedded GraphView and complete exported HTML digest. The gate therefore fails
on output or immutable-input drift. The browser limits are published in
[performance-budget.json](../web/performance-budget.json), and the recorded
headless Chromium measurements are in
[performance-measurements.json](../web/performance-measurements.json). The
direct dependency/license inventory is in
[DEPENDENCY-INVENTORY.json](../web/vendor/DEPENDENCY-INVENTORY.json).

## Required gates

From `web/`, run `npm ci --offline` followed by `npm run check`. The check covers
asset reproducibility and licenses, pinned lockfile resolution, deterministic
budget refusal, executable scenario fixtures for both workspaces, security-safe
rendering of hostile labels and locations, DOM keyboard alternatives,
continuation/staleness behavior, the offline `file://` smoke test, and
headless performance measurements with zero network requests.

The repository-native live qualification is `ctest --test-dir build -R
ui_server_test --output-on-failure`. It exercises symbol, entity, include, and
type-shaped graph requests, high-degree bounded evidence, partial/stale and
external identities, witness paths, continuation tokens, invalid origins and
sessions, oversized requests/responses, cancellation, and shutdown. `npm run
qualify` verifies immutable inputs and executes the complete eleven-class
matrix for both named workspaces (including the refreshed repository index and
a rebuilt banking workspace); semantic corpus or explorer-output drift
therefore fails the gate instead of being treated as absence.

Known advisory limitation: `npm audit --offline` requires a locally populated
npm advisory cache. When unavailable, the build remains fully offline and the
skipped audit must be reported with the exact command.
