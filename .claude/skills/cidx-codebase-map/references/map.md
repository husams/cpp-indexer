# cidx codebase map — where to work

- `python/indexer/storage.py` and `src/storage/`: SQLite schema, migrations,
  persistence records, and storage operations.
- `python/indexer/cli.py` and `src/cli/`: command parsing, command behavior,
  formatting, and JSON output.
- `python/indexer/query.py`, `python/indexer/entity_graph.py`, and `src/graph/`:
  graph queries and graph records.
- `src/ast/` and `src/toolchain/`: the indexing engine (Clang C++ API
  visitors) and driver introspection — C++ only, the sole indexer.
- `python/indexer/compiledb.py` and `src/compiledb/`: compilation DBs.
- `src/astgraph/`: the `cidx-astgraph` per-TU AST graph tool + Souffle.
- `python/indexer/clang/`, `python/indexer/astcache.py`,
  `python/indexer/astcmd.py`: LEGACY libclang indexer/AST cache, pending
  removal — do not extend.
- `python/tests/` and `tests/`: Python and C++ coverage. `manifests/` contains
  small indexing fixtures. Keep tests hermetic unless they are deliberately
  labeled as libclang integration tests.
- `docs/adr/`, `python/docs/`, and `spec/`: design history and contracts. Check
  the relevant ADR before changing an established data model or graph rule.
