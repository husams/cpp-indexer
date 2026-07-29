# CIDX Graph Explorer assets

The HSE-91 static export is a byte-stable HTML snapshot. It embeds the
bounded GraphView data and every runtime asset, and sets `CIDX_OFFLINE` so the
browser cannot request a loopback expansion. `vendor/LICENSES.json` records
the pinned third-party asset and its digest.

The explorer is an offline, self-contained browser surface. Cytoscape.js is
pinned to `3.31.2` in `package.json` and `package-lock.json`; the checked-in
browser bundle is `vendor/cytoscape.min.js` (SHA-256
`a9e75b12b6dab7b7f5b428636070b38c59eba2a4a7d0a3af36b2afdd67b13499`).

The bundle is distributed under the Cytoscape.js MIT license. Static exports do
not load assets or data from a network at runtime. A live `cidx ui open` session
fetches only authenticated, bounded GraphView slices from its loopback `/api/graph`
endpoint when a user expands a selected node; it never contacts an external
origin.

## M4 qualification gates

`npm run check` verifies the pinned offline asset, dependency inventory, browser
budgets, DOM-level regressions, and the Playwright `file://` smoke test. The
published limits are in `performance-budget.json`; an incoming view that
exceeds them is refused before Cytoscape layout starts. The
`qualification-manifest.json` records the frontend, Cytoscape, catalog, schema,
workspace, and corpus identifiers used by the M4 qualification.

The live server binds to IPv4 loopback, requires its ephemeral session token,
rejects a non-matching Origin, caps request/response sizes, times out incomplete
HTTP headers, and emits no-store plus CSP, referrer, MIME, permissions, and
cross-origin policy headers. Source actions copy a bounded, redacted location;
they never open a user-controlled path or URL.

The graph canvas has a keyboard/screen-reader text and table alternative for
nodes, edges, bounded evidence, paths, and result status. Statuses include a
glyph and explanation in addition to colour, and reduced-motion preferences
disable camera animation.
