# Python examples — querying the CodexGraph

Runnable examples for querying a `cpp-indexer` graph stored in **IndraDB**. They
talk to the graph through the project's own `cpp_agent_nav.IndraDBAPI` (located
relative to the repo at `.claude/skills/cpp-graph-code-reasoning/scripts`), so
they reuse the schema-aware traversal and the gRPC message-cap fix.

## Files

| File | What it shows |
|---|---|
| `cxg_graph.py` | Shared helper (`Cxg`, `SymbolMap`) + a schema cheat-sheet in its docstring. Import this, don't run it. |
| `graph_query_examples.py` | Basics: node inventory, classes vs structs, symbol lookup, one-hop class relationships, callees, graph-native source body. |
| `advanced_graph_queries.py` | Transitive call trees, class hierarchies (up/down), "who uses this type", namespace contents, orphan-interface detection, `file_id`/`symbol_id` resolution, fan-out ranking. |

## Prerequisites

1. A repo indexed into a running IndraDB server, e.g.:
   ```bash
   indradb-server -a 127.0.0.1:27652 memory &
   cxg-index <repo> --backend indradb --db-uri http://localhost:27652
   ```
2. Python driver: `pip install indradb` (the project's `cpp_agent_nav` is found
   relative to this directory — no install needed).

## Running

```bash
PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python \
CXG_DB_URI=indradb://localhost:27652 \
python3 graph_query_examples.py

# advanced; CXG_SYMBOL_DB is optional (enables id->path/USR resolution)
PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python \
CXG_DB_URI=indradb://localhost:27652 \
CXG_SYMBOL_DB=<stage-dir>/cxg-symbols.db \
python3 advanced_graph_queries.py
```

`PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python` is **required** — the `indradb`
driver's generated protobufs predate protobuf 7.x and otherwise fail to import.

## Environment variables

| Var | Default | Purpose |
|---|---|---|
| `CXG_DB_URI` | `indradb://localhost:27652` | Graph location. |
| `CXG_SYMBOL_DB` | *(unset)* | Path to `cxg-symbols.db` for `file_id`→path and `symbol_id`→USR resolution (compact-ingest stores ids, not paths). |

## Schema gotchas (the ones that trip people up)

- **Look up functions by `name`, not `qualified_name`.** For `METHOD`/`FUNCTION`
  nodes, `qualified_name` embeds the parameter list but no scope
  (`offsets_store(std::vector<TopicPartition *> &)`).
- **`CALLS`/`USES` edges originate from `METHOD`/`FUNCTION` nodes, never `CLASS`.**
  Anchor on methods for delegation/data-access.
- **No `file_path` property** under compact-ingest — only `file_id` (int) into
  `cxg-symbols.db`. Same for `symbol_id`→USR.
- **Function nodes carry the full source** in the `code` property — useful where
  `CALLS` edges are missing.
- **`CALLS` coverage is partial/uneven** — call trees are a lower bound.
- Vertex type label `t` equals the node `kind`. Edge types: `INHERITS`,
  `OVERRIDES`, `CALLS`, `USES`, `HAS_METHOD`, `HAS_PARAM`, `RETURNS`, `OF_TYPE`,
  `CONTAINS`, `POINTS_TO`, `INCLUDES`, `EXTERNAL_REF`.
