# Build & Platforms

[← docs index](README.md) · related: [ast module](modules/ast.md)

## Toolchain

- **CMake, C++23.** Build hosts: macOS AppleClang 15+ / Linux gcc 13+
  (gcc-toolset-13 on RHEL 9). SQLite **≥ 3.35** required (for `RETURNING`).
- **LLVM/Clang dev install** — the engine links the Clang C++ API
  (`clang-cpp` + shared `libLLVM`). CMake auto-discovers it via
  `llvm-config --cmakedir`; pass `-DLLVM_DIR=…/lib/cmake/llvm
  -DClang_DIR=…/lib/cmake/clang` when that probe fails.

## macOS

```bash
brew install llvm
cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix llvm)" -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

The Homebrew keg-only LLVM is auto-detected for `clang-cpp`/`libLLVM`.

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

## The Clang C++ API link model

`cidx_core` links the shared Clang C++ API (`clang-cpp` + `libLLVM`). The
[`src/ast`](modules/ast.md) sources compile in a `cidx_ast` **OBJECT library**
with:

- **`-isystem`** LLVM/Clang headers — a plain `-I` breaks gcc's
  `#include_next <stdlib.h>` in `<cstdlib>`;
- **`-fno-rtti`** — to match a no-RTTI LLVM (e.g. Homebrew's).

On Linux, **`libstdc++` stays dynamic**: the shared `libLLVM` uses the system
libstdc++, so a `-static-libstdc++` binary would put two C++ runtimes in one
process and corrupt the ABI across the LLVM boundary. A gcc-toolset binary runs
on base RHEL 9 via the system `libstdc++.so.6` + nonshared static bits.

## Version portability (LLVM 21 vs 22)

The C++ API is not source-stable across majors. Divergences are localized in
`src/ast/clang_compat.hpp` and a few `#if LLVM_VERSION_MAJOR >= 22` guards
(NestedNameSpecifier, `TypeLoc` qualifier handling, `APSInt` formatting).
Validated on macOS (LLVM 22) and RHEL 9 (LLVM 21).

## clang-tidy

`make tidy` analyzes every project-owned C++ source with the checked-in
`.clang-tidy` policies and CMake's exact C++23 compile commands. Generated and
third-party code is excluded. To run clang-tidy alongside compilation instead:

```bash
cmake -S . -B build -DCIDX_ENABLE_CLANG_TIDY=ON
cmake --build build -j
```

Add `-DCIDX_CLANG_TIDY_WARNINGS_AS_ERRORS=ON` when working against a clean
baseline and findings should fail the target or build.

## Verification

| Gate | Checks |
|---|---|
| `ctest --test-dir build -L default` | hermetic unit/integration suite |
| `ctest --test-dir build -L clang` | real-parse suites incl. the index golden gate and the focused visitor fixtures |
| `make tidy` | modern C++23, bug-prone, analyzer, performance, readability, and enum checks over `src/` |
| `scripts/check_ast_complexity.sh` | the scoped clang-tidy complexity gate for `src/ast` |
| `scripts/dump_layer0.sh <db>` | normalized Layer-0 projection for reviewing semantic deltas |
| `pytest python/tests` | the Python storage/read-query suite |
