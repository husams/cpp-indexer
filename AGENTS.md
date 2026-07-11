# cpp-indexer contributor instructions

This repository contains two implementations of the same `cidx` tool:

| Implementation | Location | Role |
|---|---|---|
| C++ | `src/` | Performance implementation built by CMake |
| Python | `python/indexer/` | Canonical behavior and public Python API |

Every observable behavioral or SQLite contract change must be implemented in
both languages in the same change. This includes schema versions, indexing,
query behavior, CLI flags and output, and JSON shapes.

Before committing, run:

```bash
python -m pip install -e './python[dev]'
pytest python/tests
cmake -S . -B build
cmake --build build -j
ctest --test-dir build -L default --output-on-failure
```

Do not add Rust/Cargo configuration back to this repository.
