# `src/clangx` — the libclang engine (default)

[← docs index](../README.md) · related: [indexing engines](../indexing-engines.md) · [clangx_lt](clangx_lt.md)

The default Layer-0 extraction engine, built on **libclang** — the stable C API
(`clang-c/Index.h`) with a cursor-based AST walk. ~6.4k LOC. Selected unless
`CIDX_INDEX_ENGINE=lt`.

## Files

| File | Role |
|---|---|
| `ast.hpp` / `ast.cpp` | `AstIndexer` — the extraction orchestrator + walk primitives |
| `ast_symbols.cpp` | symbol pass (`to_symbol`, `store`, `index_file`) |
| `ast_edges.cpp` | declaration-level edge pass + body-descent driver |
| `ast_body.cpp` | body descent: calls, uses, construct/destroy forms, provenance |
| `ast_templates.cpp` | template arg/param extraction incl. the `-1` token fallback |
| `ast_cursor.cpp` | the cursor-walk engine, name/location helpers, namespace-uses |
| `ast_query.hpp` / `.cpp` | on-demand walkers for `cidx ast` (independent of indexing) |
| `parse.hpp` / `.cpp` | `Parser` — `clang_parseTranslationUnit` + diagnostics |
| `toolchain.hpp` / `.cpp` | `Toolchain` — **driver introspection** |
| `pch.hpp` / `.cpp` | shared system precompiled header |
| `clang_runtime.hpp` / `.cpp` | libclang discovery + version |
| `clang_raii.hpp` | RAII for libclang-owned handles (`CxString`, indices, TUs) |

## Classes

### `AstIndexer` (`ast.hpp:45`)

The orchestrator. Public passes:

- `index_symbols` — the symbol pass over the target file.
- `index_headers` — the two-pass sequence over owned, non-system headers
  (pass 1 mints their symbols, pass 2 extracts their edges) — see the
  [per-file interleave](../data-flow.md#the-per-file-interleave).
- `index_edges` — declaration-level edges (`ast_edges.cpp`) then body descent
  (`ast_body.cpp`) then namespace-uses.

Private cursor-walk primitives `for_file_cursors` / `for_file_cursors_p`
(parent-aware) prune to the target file and stop at function bodies (descended
explicitly). Emission goes through `Storage` (see [storage](storage.md)).

### `Parser` (`parse.hpp`)

Wraps `clang_parseTranslationUnit`. Assembles `opts + toolchain_flags +
-ferror-limit=0`, captures diagnostics (`collect_diagnostics`), and enforces the
fatal-diagnostic policy: a diagnostic at/above the abort level (`CIDX_STRICT`
picks Error vs Fatal) throws `ClangParseError` **before** any rows are written,
so a failed file leaves the DB untouched.

### `Toolchain` (`toolchain.hpp`) — driver introspection

`toolchain_flags(is_cpp, driver)` appends the include search paths **replicated
from the named compiler driver**: `driver_search_dirs(driver, lang)` runs
`<driver> -E -x <lang> - -v` (empty stdin, 30 s timeout via
[`util/subprocess`](util.md)) and parses the `#include <...> search starts here`
block, plus sysroot/resource-dir. Memoized per `(driver, lang)`. This is what
lets cidx index GCC-built code correctly with a Clang parser, and is **shared by
both engines** — see [indexing engines](../indexing-engines.md).

### `CxString` / `clang_raii.hpp`

RAII wrappers so nothing libclang-owned outlives a visit
(`clang_disposeString`, `clang_disposeTranslationUnit`, …).

## Known fidelity limits (motivating the LibTooling engine)

libclang exposes ~80% of the C++ surface: some symbol properties are always
false, C++20 Concepts are not modeled, and `clang_Cursor_getNumTemplateArguments`
returns `-1` for method-template specializations (recovered here from source
tokens in `ast_templates.cpp`). The [`clangx_lt`](clangx_lt.md) engine closes
these — see [indexing engines](../indexing-engines.md).
