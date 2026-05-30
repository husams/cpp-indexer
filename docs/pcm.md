# PCM / C++20 Module Support

## Overview

`cpp-indexer` supports indexing C++ translation units that use Clang precompiled
modules (`.pcm` files) via `-fmodules`, `-fmodule-file=`, and
`-fprebuilt-module-path` compile flags. This page documents requirements,
supported flags, and the failure posture when `.pcm` inputs are missing or invalid.

## Requirements

- **libclang 18** or later is required for module-capable parsing. libclang 17
  and earlier do not expose the module-interface parse API used by the indexer.
- A valid `compile_commands.json` entry for each translation unit, with the
  PCM-related flags present in the `arguments` array (the indexer does not
  synthesize module flags).

## Supported flags

The following compile-command flags cause a translation unit to be recognised
as a module or PCM consumer and routed to the module parse path:

| Flag | Form | Meaning |
|------|------|---------|
| `-fmodules` | bare | Enable Clang implicit module map support |
| `-fmodule-file=<path>` | `name=path` or bare path | Provide a prebuilt `.pcm` for a named module |
| `-fprebuilt-module-path=<dir>` | path to directory | Directory searched for prebuilt `.pcm` files |

Detection is flag-presence-only: the C++ standard (`-std=c++17`, `-std=c++20`,
etc.) is **not** required. Any TU whose compile flags contain at least one of
the above is treated as a PCM-consuming TU regardless of file extension.

Module interface units identified by file extension (`.cppm`, `.ixx`, `.mxx`)
are also routed to the module parse path automatically.

## Best-effort posture (libclang < 18)

When the runtime libclang does not support module parsing (probe returns false),
the indexer adopts a **best-effort skip** posture:

- PCM-consuming TUs are **skipped** with a `WARN`-level log message naming the
  file.
- The skip is counted as a TU error (`failed_tus > 0`), visible in the closing
  summary and in the process exit code when `--fail-on-tu-error` is set to a
  threshold below 1.0.
- Non-module TUs in the same `compile_commands.json` are indexed normally; the
  run is **not** aborted.

Upgrade to libclang 18+ to index PCM-consuming TUs.

## Loud failure on missing or invalid `.pcm`

A **silent partial parse** (Issue 0001 family) is explicitly prevented. When a
`.pcm` file referenced by `-fmodule-file=` does not exist at parse time:

1. The indexer stats the `.pcm` path **before** invoking libclang.
2. If the file is missing, an **ERROR**-level log is emitted naming both the
   `.pcm` path and the TU source file.
3. The TU is counted as a failure (`failed_tus > 0`). No graph nodes or edges
   are written for that TU (no silent partial output).
4. Parsing continues on remaining TUs.

For `-fprebuilt-module-path=<dir>`, the directory is stat-checked at parse
time. Individual `.pcm` files resolved lazily from that directory are caught by
a **post-parse Fatal gate**: if libclang emits a `Fatal`-severity diagnostic
(corrupt, truncated, or out-of-date `.pcm`), the TU is counted as a failure and
no output is written — same non-zero `failed_tus` signal, no silent partial.

In both cases the result is **non-zero** `failed_tus` in the closing summary,
never a silent zero with missing data.

## Environment setup (macOS arm64, libclang 18)

```sh
export LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib
export DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib
```

## See also

- `README.md` — top-level requirements and pipeline overview
- `docs/schema/` — graph schema (PCM support adds no new node or edge kinds;
  module imports reuse `EdgeKind::Includes`)
- Issue 0001 — silent total TU parse failure (the motivating defect)
