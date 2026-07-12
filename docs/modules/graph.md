# `src/graph` — read-only query API

[← docs index](../README.md) · related: [data model](../data-model.md) · [cli](cli.md)

The read side. A view over the resolved `index.db` backing the `cidx graph …`
subcommands. Strictly read-only. ~1.4k LOC.

## Files

| File | Role |
|---|---|
| `query.hpp` / `query.cpp` | `GraphQuery` — the read API |
| `emit.hpp` / `emit.cpp` | `emit_edges` / `emit_syms` — text or `--json` rendering |
| `records.hpp` | result structs (`Sym`, `Edge`, `Site`, `Definition`, `Traversal`) |

## `GraphQuery` (`query.hpp:85`)

A 1:1 read-only port of the Python `GraphQuery`, opened over a `Storage&`. It
raises `NoIndexError` / `NoEdgesError` on an empty graph (`edge_count()` /
`require_edges()`).

| Group | Methods | Backing `cidx graph` command |
|---|---|---|
| Edge-kind catalog | `edge_kinds_map` `:49`, `edge_names_map` `:66` | — |
| Symbol lookup | `get_by_id` `:102`, `get_by_usr` `:103`, `find` (fuzzy) `:107` | — |
| Edge traversal | `edges` `:115`, `edges_in`/`edges_out` `:119/:123`, `references` `:127`, `aliased_by` `:131`, `sites` `:134` | `callers`, `callees`, `refs` |
| Navigation | `peers` `:139`, `kind_ids` `:145`, `walk` (bounded BFS) `:149`, `reaches` (shortest path) `:156` | `neighbors`, `walk`, `path` |
| Hierarchy | `bases` `:163`, `subclasses` `:164`, `members` `:165` | `hierarchy` |
| Dispatch | `overrides_of` `:170`, `overridden_by` `:171`, `is_virtual_method` `:172`, `dispatch_targets` `:174` | `dispatch` |
| Multi-definition (v27) | `redefined` `:178`, `definitions` `:179`, `possible_callees` `:180` | `redefined`, `definitions` |

Dispatch/impact queries rely on the resolve-time `dispatch_calls` (edge kind 18)
and `possible_call` tables — see the [resolve pass](../data-flow.md#the-resolve-pass).

## Emitters (`emit.hpp`)

`emit_edges` (`emit.hpp:24`) and `emit_syms` (`emit.hpp:31`) render results as
human-readable tables or `--json` (through [`cli/json_out`](cli.md#output-formatting));
`emit_syms` takes an optional `{id → depth}` map for `walk`.
