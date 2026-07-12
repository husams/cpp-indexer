# cidx build and test — full commands

## Prerequisites

- Python 3.12+, SQLite 3, CMake, and a supported C++23 compiler
  (AppleClang 15+, Clang 16+, or GCC 13+).
- An LLVM/Clang dev install (the LibTooling engine links clang-cpp + libLLVM).
  CMake auto-discovers it via `llvm-config --cmakedir`; if discovery fails pass
  `-DLLVM_DIR=.../lib/cmake/llvm -DClang_DIR=.../lib/cmake/clang`.
- Souffle support is optional and falls back to a stub when unavailable.

## Python suite (canonical)

```bash
python -m pip install -e './python[dev]'
pytest python/tests
```

`uv` is also supported from `python/`:

```bash
uv sync --dev
uv run pytest
```

## C++ suite (hermetic)

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build -L default --output-on-failure
```

## Targeted gates

```bash
ctest --test-dir build -L clang --output-on-failure
ctest --test-dir build -L parity --output-on-failure
```

- The `clang` tests perform real parses.
- The `parity` test needs `uv`, SQLite, the Python launcher, and the built C++
  executable.
- `make`, `make test`, and `make static` are convenience wrappers; CMake remains
  the source of truth.

## Which gates to run

- Before committing a behavioral change: both full Python tests and the C++
  `default` tests.
- Add the `clang` and `parity` gates when the change touches parsing, indexing,
  storage interchange, CLI behavior, or graph results.
- When a full suite is impractical during iteration, run the narrow test first,
  then the required gates before handoff.
