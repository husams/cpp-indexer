# cidx — Implementation Documentation

`cidx` is a semantic indexer for C and C++ codebases. It parses translation
units with Clang, extracts a symbol-level relationship graph, stores it in one
SQLite database (`index.db`), and exposes navigation/query commands (callers,
callees, references, hierarchy, virtual dispatch, impact) plus Datalog analyses.

This documentation describes the **C++23 implementation** under `src/`. It is
organized as one page per component plus a few cross-cutting pages. Start with
the [overview](overview.md), then read the module you care about.

## Cross-cutting

| Page | Contents |
|---|---|
| [Overview](overview.md) | what cidx does, the 4 stages, design invariants, module map + layering diagram |
| [Data model](data-model.md) | schema 28 tables, the ER diagram, the three graph layers |
| [Data flow](data-flow.md) | end-to-end pipeline, the index sequence, the per-file interleave, the resolve pass |
| [Indexing engines](indexing-engines.md) | the two interchangeable engines (libclang vs LibTooling) |
| [Build & platforms](build.md) | CMake, macOS/RHEL, the LT engine link model, parity scripts |
| [Glossary](glossary.md) | USR, Layer-0/1, driver introspection, stub, multi_def |

## Modules (`src/`)

| Module | Page | Role |
|---|---|---|
| `cli/` | [cli](modules/cli.md) | command dispatch, arg parsing, output formatting |
| `storage/` | [storage](modules/storage.md) | the SQLite layer: `Storage`, schema, migrations, resolve pass |
| `compiledb/` | [compiledb](modules/compiledb.md) | `compile_commands.json` load, flag sanitize, include aliasing |
| `clangx/` | [clangx](modules/clangx.md) | the **libclang** indexing engine (default) |
| `clangx_lt/` | [clangx_lt](modules/clangx_lt.md) | the **LibTooling** indexing engine (`CIDX_INDEX_ENGINE=lt`) |
| `graph/` | [graph](modules/graph.md) | read-only `GraphQuery` + emitters |
| `astcache/` | [astcache](modules/astcache.md) | on-disk libclang TU cache for `cidx ast` |
| `astgraph/` | [astgraph](modules/astgraph.md) | `cidx-astgraph`: per-TU raw AST graph + native Souffle |
| `util/` | [util](modules/util.md) | logger, env, subprocess, hashing, pathutil, repo, files |

## Conventions used in these docs

- File anchors are written `file:line` relative to the repo root.
- **Layer-0** = raw AST-extracted rows (symbols + edges); **Layer-1** = the
  derived design graph produced by `cidx resolve`. See the [data model](data-model.md).
- Two invariants recur everywhere: **byte-for-byte parity** with the Python
  reference implementation (`python/indexer/`), and **driver introspection**
  (replicating a specific compiler's include paths). See the [overview](overview.md).
