# Build & Platforms

[← docs index](README.md) · related: [indexing engines](indexing-engines.md)

## Toolchain

- **CMake, C++23.** Build hosts: macOS AppleClang 15+ / Linux gcc 13+
  (gcc-toolset-13 on RHEL 9). SQLite **≥ 3.35** required (for `RETURNING`).
- **libclang headers** come from the *installed* LLVM (a `find_path` for
  `clang-c/Index.h`), from the same prefix as the linked `libclang` — no
  vendored headers.

## macOS

```bash
brew install llvm
cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix llvm)" -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

The Homebrew keg-only LLVM is auto-detected for both the libclang library and
the LibTooling engine's `clang-cpp`/`libLLVM`.

## RHEL 9 / Rocky 9

```bash
./scripts/build-rhel9.sh
```

The script:

- installs deps (`gcc-toolset-13`, `cmake`, `clang-devel`, `llvm-devel`,
  `clang-libs`, `llvm-libs`); `DEPS_ONLY=1` stops after deps;
- builds a **static SQLite** from the amalgamation (RHEL ships 3.34.1, below the
  3.35 floor);
- configures with `LLVM_DIR`/`Clang_DIR` from `llvm-config --cmakedir` so
  `find_package(Clang)` resolves the versioned RHEL LLVM.

Runtime needs `clang-libs` + `llvm-libs`.

## The LibTooling engine's link model

`cidx_core` links the shared Clang C++ API (`clang-cpp` + `libLLVM`). The
[`clangx_lt`](modules/clangx_lt.md) sources compile in a `cidx_lt` **OBJECT
library** with:

- **`-isystem`** LLVM/Clang headers — a plain `-I` breaks gcc's
  `#include_next <stdlib.h>` in `<cstdlib>`;
- **`-fno-rtti`** — to match a no-RTTI LLVM (e.g. Homebrew's).

On Linux, **`libstdc++` stays dynamic**: the shared `libLLVM` uses the system
libstdc++, so a `-static-libstdc++` binary would put two C++ runtimes in one
process and corrupt the ABI across the LLVM boundary. A gcc-toolset binary runs
on base RHEL 9 via the system `libstdc++.so.6` + nonshared static bits.

## Version portability (LLVM 21 vs 22)

The C++ API is not source-stable across majors. Divergences are localized in
`src/clangx_lt/llvm_compat.hpp` and a few `#if LLVM_VERSION_MAJOR >= 22` guards
(NestedNameSpecifier, `VisitTypeLoc` qualifier, `ElaboratedTypeLoc` peeling,
`APSInt` formatting). The libclang engine is unaffected (the C API is stable).

## Verification scripts

| Script | Checks |
|---|---|
| `scripts/lt_symbol_diff.sh` | symbol-pass parity (LT vs libclang) |
| `scripts/lt_index_diff.sh` | full-database dual-engine parity (all 7 Layer-0 tables) |
| `scripts/e2e_lt_gcc_driver.sh` | GCC-driver replication (`-nostdinc++`) + dual-engine parity |
| `ctest` | the unit/integration suite |

Verified byte-identical on macOS (LLVM 22) and RHEL 9.8 / Rocky (LLVM 21),
including base g++ 11.5 and gcc-toolset-13 as compile-command drivers.
