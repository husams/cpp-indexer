# `src/toolchain` — driver introspection

[← docs index](../README.md) · related: [ast](ast.md) · [compiledb](compiledb.md)

`Toolchain::toolchain_flags(is_cpp, driver)` appends the include search paths
**replicated from the named compiler driver**: `driver_search_dirs(driver,
lang)` runs `<driver> -E -x <lang> - -v` (empty stdin, 30 s timeout via
[`util/subprocess`](util.md)) and parses the `#include <...> search starts
here` block, plus sysroot/resource-dir handling.

Memoized per `(driver, lang)`. This is what lets cidx index GCC-built code
correctly with a Clang parser: the parse uses Clang, the include environment
is whatever `compile_commands.json`'s `driver` would have used (e.g. a
specific cross `g++`).

Consumed by [`ast/index_engine`](ast.md)'s `build_clang_arguments` on every
TU, alongside the sanitized stored options from
[`compiledb`](compiledb.md).
