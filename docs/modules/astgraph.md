# `src/astgraph` — per-TU AST graph tool (`cidx-astgraph`)

[← docs index](../README.md) · related: [cli](cli.md)

A **separate binary**, `cidx-astgraph`, that dumps one translation unit's *raw
libclang AST* into a per-TU SQLite graph for Datalog reasoning — far
finer-grained than the repo-wide semantic index. ~1.1k LOC.

## Files

| File | Role |
|---|---|
| `astgraph.hpp` / `astgraph.cpp` | the dump engine (`dump_tu`) |
| `main.cpp` | the `cidx-astgraph` entry point |
| `souffle_runner.hpp` + `rules/` | embedded native Souffle rule(s) (`callgraph`) |

## The artifact (`astgraph.hpp`)

A unified node space: cursors **and** types are rows of `node`; every relation
is a row of `edge(src, dst, rel, ord)`. No NULLs (0/'' sentinels). `RelKind`
(`:43-63`) is grounded in the libclang cursor/type cross-reference API
(kRelChild, kRelReferences, kRelOverrides, kRelResultType, kRelArgType, …);
`kTypeKindBase = 1000` namespaces `CXTypeKind` ids. Entry point
**`dump_tu(tu, out_db_path, opts, source, args, driver)`** (`:91`).

## Shares cidx's config

`main.cpp` reads the source file's compile args + driver from the cidx `index.db`
`file` row (`main.cpp:201`), re-runs [`sanitize` + `resolve_options`](compiledb.md)
(`main.cpp:209`), parses through the same [`Toolchain`/`Parser`](clangx.md)
(`main.cpp:215`), then `dump_tu` (`main.cpp:231`). Its `analyze --rule callgraph`
subcommand runs an embedded native Souffle rule (`souffle_runner.hpp:27`) over
that per-TU artifact and emits JSON.

## `cidx-astgraph` vs `cidx analyze`

| | `cidx-astgraph` | `cidx analyze` |
|---|---|---|
| Scope | one TU | whole repo-wide `index.db` |
| Data | raw libclang AST (cursors + types + libclang relations) | the semantic graph (`symbol`/`edge`/`edge_site`/`entity_*`) exported to TSV |
| Rules | embedded native Souffle `callgraph` | Datalog `callgraph` / `cycles` / `unused` (`cli/analyze.cpp`) |
| Detail | very fine (every cursor/type) | semantic (symbol-level) |

Use `cidx-astgraph` when you need the full AST shape of a single file; use
`cidx analyze` for repo-scale semantic queries.
