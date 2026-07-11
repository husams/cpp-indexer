# cidx codebase map — where to work

- `python/indexer/storage.py` and `src/storage/`: SQLite schema, migrations,
  persistence records, and storage operations.
- `python/indexer/cli.py` and `src/cli/`: command parsing, command behavior,
  formatting, and JSON output.
- `python/indexer/query.py`, `python/indexer/entity_graph.py`, and `src/graph/`:
  graph queries and graph records.
- `python/indexer/clang/`, `python/indexer/compiledb.py`, and `src/clangx/` plus
  `src/compiledb/`: libclang parsing, toolchain handling, and compilation DBs.
- `python/indexer/astcache.py`, `python/indexer/astcmd.py`, `src/astcache/`, and
  `src/astgraph/`: on-demand AST analysis and caching.
- `python/tests/` and `tests/`: Python and C++ coverage. `manifests/` contains
  small indexing fixtures. Keep tests hermetic unless they are deliberately
  labeled as libclang integration tests.
- `docs/adr/`, `python/docs/`, and `spec/`: design history and contracts. Check
  the relevant ADR before changing an established data model or graph rule.
