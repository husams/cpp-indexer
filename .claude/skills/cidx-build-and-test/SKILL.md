---
name: cidx-build-and-test
description: Build and test the cidx indexer's Python and C++ suites, and pick the right test gate. Use when building the project, running pytest or ctest, setting up the toolchain, or deciding which gates (default, clang, parity) a change requires before commit or handoff.
---

# Building and testing cidx

Prerequisites: Python 3.12+, SQLite 3, CMake, and a C++23 compiler
(AppleClang 15+, Clang 16+, or GCC 13+). If CMake cannot find libclang, pass
`-DCIDX_LIBCLANG=/path/to/libclang`. Souffle is optional (stub fallback).

Canonical Python suite:
```bash
python -m pip install -e './python[dev]' && pytest python/tests
```

Hermetic C++ suite:
```bash
cmake -S . -B build && cmake --build build -j
ctest --test-dir build -L default --output-on-failure
```

Before committing a behavioral change: run **both** full Python tests and the
C++ `default` tests. Run the `clang` and `parity` gates when the change touches
parsing, indexing, storage interchange, CLI behavior, or graph results.

For the `uv` workflow, the targeted gates, and exactly what each gate needs, see
[references/commands.md](references/commands.md).
