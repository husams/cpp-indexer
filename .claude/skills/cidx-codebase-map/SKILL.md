---
name: cidx-codebase-map
description: Locate where a cidx concern lives across the Python and C++ trees. Use when deciding which files to edit for storage/schema, CLI, graph queries, libclang/compilation DBs, AST caching, or tests, and to know which ADRs to check before changing an established data model or graph rule.
---

# cidx codebase map

Use this to find the paired Python and C++ files for a concern before editing.
The full concern-to-path table, test layout, and ADR pointers are in
[references/map.md](references/map.md).

Quick index:

- **Storage / schema / migrations** → `python/indexer/storage.py`, `src/storage/`
- **CLI / formatting / JSON** → `python/indexer/cli.py`, `src/cli/`
- **Graph queries** → `python/indexer/query.py`, `python/indexer/entity_graph.py`, `src/graph/`
- **libclang / compilation DBs** → `python/indexer/clang/`, `python/indexer/compiledb.py`, `src/clangx/`, `src/compiledb/`
- **AST cache** → `python/indexer/astcache.py`, `python/indexer/astcmd.py`, `src/astcache/`, `src/astgraph/`
- **Tests / fixtures** → `python/tests/`, `tests/`, `manifests/`
- **Design history** → `docs/adr/`, `python/docs/`, `spec/`
