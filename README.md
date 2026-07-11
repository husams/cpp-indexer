# cpp-indexer

`cpp-indexer` now contains the two parity implementations of `cidx`:

- the C++17 implementation at the repository root, built with CMake;
- the canonical Python implementation and query API in `python/`, packaged as
  the pip-installable `cidx-indexer` project.

The previous Rust, Neo4j, IndraDB, and daemon implementation has been removed.
Both implementations use the same SQLite index contract and must remain
behaviorally compatible.

## Build the C++ tool

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build -L default --output-on-failure
```

The resulting executable is `build/cidx`.

## Install the Python API

```bash
python -m pip install ./python
```

This installs the `indexer` Python package and the `indexer` and
`cidx-python` console commands. The repository launcher is also available as
`python/cidx`.

## Parity rule

Observable behavior, SQLite schema changes, CLI output, and JSON output must
be changed in both implementations in the same change. Run the Python tests
and the C++ tests before merging.
