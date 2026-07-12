# `src/compiledb` — compile-command handling

[← docs index](../README.md) · related: [clangx/toolchain](clangx.md) · [data flow](../data-flow.md)

Loads `compile_commands.json`, strips/sanitizes flags for the parser, locates
the real compiler driver, and encodes/decodes portable include-path aliases.
Shared by both indexing engines and by `cidx-astgraph`. ~0.6k LOC.

## Files

`compiledb.hpp` / `compiledb.cpp`.

## Classes

### `CompileCommand` (`compiledb.hpp:25`)

Plain data: `directory`, `filename`, `driver`, `args` (the stripped flags).

### `CompileDb` (`compiledb.hpp:32`)

Loads through libclang's `CXCompilationDatabase` (never its own JSON parsing, so
shell-unquoting matches Python exactly). Key operations:

| Method | Purpose |
|---|---|
| `load(db_arg)` `:37` | read `compile_commands.json` (path or dir), abspath'd |
| `strip_for_libclang(argv, file, dir)` `:51` | parse-time strip: drop `argv[0]`, the frozen drop sets, and the source file; absolutize `-I/-isystem/-iquote` (spaced + glued); preserve `<label>`/`$VAR` values verbatim |
| **`sanitize(stored)`** `:58` | re-apply *only* the drop rules to already-stored options — heals DBs imported by an older cidx with a shorter drop list |
| `driver(...)` `:69` / `command_start` `:64` | locate + absolutize the real compiler, skipping env assignments and wrappers (ccache/sccache/distcc) |
| **`resolve_options(options, lookup, autoderive)`** `:100` | **decode** `<label>`/`$VAR`/`~` include tokens back to absolute dirs (via alias lookup) so the parser sees real paths |
| `build_label_map` `:112`, `match_alias` `:122`, `alias_options` `:137` | the **encode** side — rewrite include paths to `<label>` tokens for portable stored options |
| `split_base_version` `:78`, `version_key` `:88` | portable-path helpers for versioned toolchain dirs |

## Where it fits

At **import**, options are stored (optionally aliased). At **index** time,
`index_one` re-runs `sanitize` + `resolve_options` on the stored options before
handing them to the engine — see the [data flow sequence](../data-flow.md#indexing-one-translation-unit).
The include *search paths* (as opposed to the explicit `-I` flags kept here) are
added separately by [`clangx/toolchain`](clangx.md) via driver introspection.
