# `src/cli` — command dispatch

[← docs index](../README.md) · related: [data flow](../data-flow.md)

Everything the `cidx` binary does at the surface: parse argv, build a `Context`,
dispatch to a `cmd_*` handler, format output. ~8.4k LOC.

## Files

| File | Role |
|---|---|
| `args.hpp` / `args.cpp` | `ParsedArgs` + the hand-rolled argument parser (argparse parity) |
| `commands.hpp` / `commands.cpp` | `Context`, `run_command`, every `cmd_*` handler |
| `format.hpp` / `format.cpp` | symbol-table + Python f-string output helpers |
| `json_out.hpp` / `json_out.cpp` | a `Value` tree serialized as `json.dumps(indent=2)` |
| `kind_names.hpp` / `.cpp` | `CXCursorKind` id ↔ name |
| `analyze.cpp` | `cidx analyze` — Souffle Datalog over the index |

## Dispatch

`main()` (`../main.cpp:49`) → `parse_args()` (`main.cpp:52`) → builds the
`Context` (`main.cpp:62`) → `run_command()` (`commands.cpp:3486`), a flat
`if (args.command == …)` chain that also branches on `args.what` for grouped
commands (`ast`, `graph`, `component`, `repo`, `pch`, …).

### `Context` (`commands.hpp:25`)

Per-invocation state handed to every command: `cache_dir`, `index_path`
(`<cache_dir>/index.db`), a `Logger*`, and `out`/`err` streams (so tests capture
stdout/stderr). `resolve_cache_dir()` implements `$INDEXER_CACHE` else
`~/.cache/cidx`.

### `ParsedArgs` + `parse_args()` (`args.hpp`)

A hand-rolled parser mirroring the Python argparse command tree (per-command
grammar branches; `kVersion = "0.53.0"`). Throws `UsageError` (exit 2) on misuse.

## Command handlers (`commands.cpp`)

| Command(s) | Handler(s) | One line |
|---|---|---|
| `init` | `cmd_init` `:443` | create/init the DB (`--force`) |
| `db migrate` | `cmd_migrate` `:465` | migrate schema forward |
| `component add` / import | `cmd_add_source` `:526`, `cmd_import` `:649` | register a component; load `compile_commands.json` |
| `index` | `cmd_index` `:824` | the per-file index pipeline (**routes to the engine in `index_one`**) |
| `resolve` | `cmd_resolve` `:1412` | `Storage::resolve_pass()` + cross-repo edges |
| `search` | `cmd_search` `:858` | fuzzy qual-name search |
| listings | `cmd_list_components/dirs/files/symbols` `:865/:908/:931/:1002` | tabular listings |
| show / delete | `cmd_show_*`, `cmd_delete_*` `:1031…:1363` | detail dumps / deletions |
| `pch` | `cmd_pch_build/status/clear` `:1436/:1560/:1565` | system PCH lifecycle |
| `ast` | `cmd_ast_dump/locals/conditions` `:2063/:2151/:2226`, `cmd_ast_cache` `:2424` | on-demand AST (retired with the libclang cutover) |
| `graph` | `cmd_graph_callers/callees/refs/neighbors/walk/path/hierarchy/dispatch` `:2865…:3061`, `redefined/definitions` `:3088/:3099` | graph queries (see [graph](graph.md)) |
| `repo` / `component` | `cmd_repo_*` `:3212…:3349`, `cmd_component_show/set_version` `:3148/:3173` | repo/component management |
| `db verify` | `cmd_verify` `:3374` | check component roots + files exist |
| `analyze` | `cmd_analyze` (analyze.cpp) | Souffle Datalog (below) |

`index_one` is the seam where **the engine is selected**
— see [ast](ast.md).

## Output formatting

- `format.hpp` (`cidx::cli::format`) — Python f-string parity: `rjust`/`ljust`,
  `py_str`/`py_repr`, `format_mtime`, and **`print_symbols`** (`format.hpp:42`),
  the shared symbol table used by `search`/`list`.
- `json_out.hpp` (`cidx::json_out`) — a `Value` tree (Null/Bool/Int/Str/Arr/Obj)
  serialized by `dumps_indent2()` as a byte-replica of CPython
  `json.dumps(indent=2)`; backs `--json` on `ast`/`graph`.

## `analyze.cpp`

`cidx analyze` exports facts (`symbol`/`edge`/`edge_site`/`entity_node`/
`entity_edge`/`file`) to TSV and runs built-in Souffle Datalog rules
(`callgraph`, `cycles`, `unused`) over the whole repo-wide index. Contrast with
the per-TU [`cidx-astgraph`](astgraph.md#cidx-astgraph-vs-cidx-analyze) tool.
