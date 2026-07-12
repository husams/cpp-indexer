# Glossary

[← docs index](README.md)

- **USR** — Unified Symbol Resolution string; Clang's stable, TU-invariant
  symbol identity and cidx's primary key (e.g. `c:@N@geo@S@Circle@F@area#1`).
  Identical between the two [indexing engines](indexing-engines.md).
- **Layer-0** — raw AST-extracted symbols + edges (the engine's output). Where
  the two engines must agree byte-for-byte. See the [data model](data-model.md#the-three-graph-layers).
- **Layer-1** — the derived *design* graph (`entity_node`/`entity_edge`) plus the
  `dispatch_calls` and `possible_call` roll-ups, produced by
  [`cidx resolve`](data-flow.md#the-resolve-pass).
- **Definition layer** — `definition`/`def_edge`: a symbol's body per backend/TU
  and that body's calls/uses, so multiply-defined symbols keep each body's edges.
- **Driver introspection** — replicating a specific compiler's include search
  paths by running `<driver> -E -x <lang> - -v`, so Clang parses with the right
  headers even for GCC-built code. Lives in [`clangx/toolchain`](modules/clangx.md).
- **Engine** — one of the two Layer-0 extractors: [`clangx`](modules/clangx.md)
  (libclang, default) or [`clangx_lt`](modules/clangx_lt.md) (LibTooling,
  `CIDX_INDEX_ENGINE=lt`).
- **Stub** — a USR-keyed placeholder `symbol` minted (`mint_symbol_id`) for a
  target not yet indexed (e.g. a callee in another TU); backfilled when its
  defining TU is indexed.
- **`multi_def`** — the number of bodies (definitions) a symbol has across
  backends; drives `possible_call` and the `redefined`/`definitions` queries.
- **Parity** — the invariant that the C++ implementation is byte-identical to the
  Python reference under `python/indexer/` (output, USRs, hashes, DB rows).
- **`edge_site` / `call_arg`** — per-occurrence location + provenance of an edge
  and per-argument value-source classification, used by virtual-dispatch and
  impact analysis.
