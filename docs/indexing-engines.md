# Indexing Engines

[← docs index](README.md)

cidx contains **two implementations** of the AST → graph extraction ("Layer-0"),
selected at runtime by the `CIDX_INDEX_ENGINE` environment variable. They are
mutually exclusive and produce **byte-identical** schema-28 output.

| | [`clangx/`](modules/clangx.md) — default | [`clangx_lt/`](modules/clangx_lt.md) — `CIDX_INDEX_ENGINE=lt` |
|---|---|---|
| API | **libclang** — stable C API (`clang-c/Index.h`), cursor walk | **LibTooling** — Clang C++ API, `RecursiveASTVisitor` |
| Links | `libclang.so` | `clang-cpp` + shared `libLLVM` |
| Portability | one binary across LLVM versions | pinned to the LLVM major it builds against |
| Fidelity | ~80% of C++ surface (some props always-false, no Concepts, `-1` template args) | full AST (real template args, Concepts, resolved dependent calls) |
| Entry | `AstIndexer` | `lt_engine::run_index_one` |

## Why two engines

libclang's C API is stable and cross-version but intentionally lossy — several
symbol properties are always false, C++20 Concepts aren't modeled, and some
template arguments come back as `-1` (recovered from source tokens). The
LibTooling engine was added to unlock that fidelity for future work, but had to
prove itself a **drop-in replacement** first: it reproduces the libclang engine
byte-for-byte before adding anything.

## How they stay identical

Both engines:

- share the **same flag/driver assembly** — [`compiledb`](modules/compiledb.md)
  (`sanitize` + `resolve_options`) and [`clangx/toolchain`](modules/clangx.md)
  (`toolchain_flags` → driver introspection);
- write through the **same `Storage` API** — `add_symbol`, `mint_symbol_id`,
  `add_edge`, `add_edge_site`, `add_call_arg`, `add_template_*`, `definition`/
  `def_edge`;
- follow the **same per-file interleave** (see [data flow](data-flow.md#the-per-file-interleave)):
  symbols(main) → header symbols → header edges → edges(main) last, with the
  same `delete_edges_for_file` collapse.

The selection happens in exactly one place — `index_one` in
[`cli/commands.cpp`](modules/cli.md) — which routes to `lt_engine::run_index_one`
when `CIDX_INDEX_ENGINE=lt`, else runs the libclang `AstIndexer` path.

## Scope

The engines only replace **Layer-0** (symbols + edges). The Layer-1 design graph
(`entity_node`/`entity_edge`), `dispatch_calls`, and `possible_call` are
downstream pure-SQL transforms produced by [`cidx resolve`](data-flow.md#the-resolve-pass)
and are engine-independent.

## Verification

- `scripts/lt_index_diff.sh` — full-database dual-engine diff over all 7 Layer-0
  tables.
- `scripts/lt_symbol_diff.sh` — symbol-pass parity.
- `scripts/e2e_lt_gcc_driver.sh` — proves the GCC-driver path (with `-nostdinc++`,
  `<string>` resolves only via cidx's driver introspection) and dual-engine
  parity.

Verified byte-identical on macOS (LLVM 22) and RHEL 9.8 / Rocky (LLVM 21),
including base g++ 11.5 and gcc-toolset-13 as drivers. See [build](build.md).
