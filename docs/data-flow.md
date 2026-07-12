# Data Flow

[← docs index](README.md)

## End-to-end pipeline

```mermaid
flowchart LR
    CC[compile_commands.json] -->|cidx import| DB[(index.db)]
    SRC[C/C++ sources] --> IDX
    DB -->|cidx index| IDX{{indexing engine}}
    IDX -->|libclang or LibTooling| L0["Layer-0: symbol, edge,<br/>edge_site, call_arg,<br/>template_*, definition, def_edge"]
    L0 --> DB
    DB -->|cidx resolve| RES["Layer-1: entity_node/edge,<br/>dispatch_calls, possible_call,<br/>multi_def, count roll-ups"]
    RES --> DB
    DB -->|cidx graph / search| Q[navigation & impact]
    DB -->|cidx analyze| DL[Souffle Datalog]
    DB -->|cidx ast| ASTQ[on-demand AST]
```

- **Import** — `cmd_import` ([cli](modules/cli.md)) loads `compile_commands.json`
  through [`CompileDb`](modules/compiledb.md) into `component`/`directory`/`file`
  rows with per-file `compile_options` + `driver`.
- **Index** — `cmd_index` walks pending files and runs the selected
  [indexing engine](indexing-engines.md) per TU (below).
- **Resolve** — `cmd_resolve` → `Storage::resolve_pass()` (below).
- **Query** — [`graph`](modules/graph.md), `search`, [`analyze`](modules/astgraph.md#cidx-astgraph-vs-cidx-analyze),
  [`ast`](modules/astcache.md).

## Indexing one translation unit

`cidx index` iterates pending files (or explicit `FILE…` args). For each source
TU, `index_one` ([cli/commands.cpp](modules/cli.md)) runs:

```mermaid
sequenceDiagram
    autonumber
    participant CMD as cmd_index / index_one
    participant CDB as CompileDb
    participant TC as Toolchain (driver probe)
    participant ENG as Engine (libclang | LibTooling)
    participant DB as Storage (index.db)

    CMD->>CDB: sanitize(stored opts) + resolve_options(labels)
    CMD->>TC: toolchain_flags(is_cpp, driver)
    TC-->>CMD: driver-replicated -isystem paths (+ -resource-dir)
    CMD->>ENG: parse TU (opts + toolchain flags + -ferror-limit=0)
    Note over ENG: diagnostics collected; fatal ≥ abort level ⇒ NO rows, file fails
    ENG->>DB: index_symbols(main file)
    loop each owned, non-system header (pass 1)
        ENG->>DB: add_file_path + index_symbols(header)
    end
    loop each owned header (pass 2)
        ENG->>DB: delete stale edges/defs + index_edges(header)
    end
    ENG->>DB: index_edges(main file)  [decl walk → body descent → ns-uses]
    CMD->>DB: replace_diagnostics + mark_file_indexed(md5, mtime)
    Note over CMD,DB: `cidx resolve` runs afterward (see below)
```

The two engines implement steps 6–10 identically; `CIDX_INDEX_ENGINE=lt` swaps
the implementation, not the sequence.

## The per-file interleave

The ordering in steps 6–10 is **load-bearing**, not incidental:

- Symbols for the **main file** are written first, then symbols for every owned,
  non-system **header** (pass 1), then **edges** for those headers (pass 2), then
  edges for the main file **last**.
- During the main file's edge walk, header-owned symbols already exist, but not
  vice-versa: header edges whose source symbol belongs to the main file are
  *deleted then re-emitted once* by the main-file walk
  (`delete_edges_for_file` excludes `contains`, keyed by the source symbol's
  winning file). This collapses cross-file double emissions and makes
  re-indexing idempotent.
- A header included by many TUs is indexed once (md5-gated) and stamped with the
  including TU's options so it stays standalone-reparseable.

The [`clangx`](modules/clangx.md) and [`clangx_lt`](modules/clangx_lt.md) pages
describe how each engine implements this sequence.

## The resolve pass

`cidx resolve` → `Storage::resolve_pass()` (`storage.cpp:4069`) runs pure SQL
transforms over Layer-0 (no re-parsing) and stamps `meta.graph_resolved_at`:

```mermaid
flowchart TD
    A["rollup_edge_counts()<br/>edge.count = COUNT(edge_site) for calls/uses"] --> B
    B["set_multi_def()<br/>symbol.multi_def = COUNT(definition)"] --> C
    C["materialize_possible_calls()<br/>body→body fan-out for multi-def callees"] --> D
    D["materialize_dispatch_calls()<br/>edge kind 18 via overrides closure"] --> E
    E["materialise_entity_edges()<br/>entity_node + entity_edge (11 relation kinds)"] --> F
    F["count remaining stub symbols → report"]
```

- **`rollup_edge_counts()`** (`storage.cpp:2816`) — `edge.count = COUNT(edge_site)`
  for calls (1) and uses (7).
- **`set_multi_def()`** (`storage.cpp:2957`) — `symbol.multi_def = COUNT(definition)`.
- **`materialize_possible_calls()`** (`storage.cpp:2964`) — for calls into a
  symbol with `multi_def > 1`, a body → body fan-out to each definition.
- **`materialize_dispatch_calls()`** (`storage.cpp:2827`) — rebuilds `edge`
  kind 18 via a recursive CTE over `overrides` edges, so
  `callers(concrete_override)` recovers the virtual caller in one hop.
- **`materialise_entity_edges()`** (`storage.cpp:4041`) — rebuilds
  `entity_node` (type classification) + `entity_edge` (11 design relations).

Details of each product are in the [storage](modules/storage.md#the-resolve-pass)
page.
