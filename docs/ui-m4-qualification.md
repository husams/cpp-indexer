# Graph explorer M4 qualification

The supported explorer surface is the loopback live server plus the
self-contained offline export. Both use the versioned `cidx.graph-view.v1`
payload and the same redaction, status, evidence, and budget rules.

## Release evidence

The checked-in [qualification manifest](../web/qualification-manifest.json)
records the pinned CIDX version/schema, main revision, catalog digest, and
`cpp-indexer`/`banking` workspace and corpus hashes. The executable scenario
inputs and expected result projections are in
[qualification-fixtures.json](../web/qualification-fixtures.json); the gate
fails on any result or identity drift. The browser limits are published in
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
sessions, oversized requests/responses, cancellation, and shutdown. Banking
workspace fixture identity is checked by `npm run qualify`, which verifies the
immutable file hashes and pinned CIDX/main revision before executing all 22
scenario/result comparisons. Semantic corpus drift therefore fails the gate
instead of being treated as absence.

Known advisory limitation: `npm audit --offline` requires a locally populated
npm advisory cache. When unavailable, the build remains fully offline and the
skipped audit must be reported with the exact command.
