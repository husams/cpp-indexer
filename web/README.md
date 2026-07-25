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
