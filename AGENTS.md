# Contributor guide for coding agents

## Project shape

This repository contains two implementations of the same `cidx` semantic
indexer. Keep them behaviorally compatible.

| Implementation | Location | Purpose |
|---|---|---|
| Python | `python/indexer/` | Canonical behavior and public Python API |
| C++23 | `src/` | Performance implementation built by CMake |

The implementations share an observable contract: SQLite schema and
migrations, indexing and query semantics, CLI flags and text output, JSON
shapes, path handling, and exit behavior. Any change to that contract must be
implemented and tested in both languages in the same change. The current main
database schema version appears in `python/indexer/storage.py` and
`src/storage/storage.cpp`; update both together.

Do not reintroduce the removed Rust/Cargo, Neo4j, IndraDB, or daemon code.

## Where to work

- `python/indexer/storage.py` and `src/storage/`: SQLite schema, migrations,
  persistence records, and storage operations.
- `python/indexer/cli.py` and `src/cli/`: command parsing, command behavior,
  formatting, and JSON output.
- `python/indexer/query.py`, `python/indexer/entity_graph.py`, and `src/graph/`:
  graph queries and graph records.
- `python/indexer/clang/`, `python/indexer/compiledb.py`, and `src/clangx/` plus
  `src/compiledb/`: libclang parsing, toolchain handling, and compilation DBs.
- `python/indexer/astcache.py`, `python/indexer/astcmd.py`, `src/astcache/`, and
  `src/astgraph/`: on-demand AST analysis and caching.
- `python/tests/` and `tests/`: Python and C++ coverage. `manifests/` contains
  small indexing fixtures. Keep tests hermetic unless they are deliberately
  labeled as libclang integration tests.
- `docs/adr/`, `python/docs/`, and `spec/`: design history and contracts. Check
  the relevant ADR before changing an established data model or graph rule.

## Build and test

Prerequisites are Python 3.12+, SQLite 3, CMake, a supported C++23 compiler
(AppleClang 15+, Clang 16+, or GCC 13+), and an LLVM/Clang development install
(the LibTooling engine links `clang-cpp` + `libLLVM`). CMake discovers LLVM/Clang
automatically via `llvm-config --cmakedir` (with Homebrew hints); pass
`-DLLVM_DIR=/path/to/lib/cmake/llvm -DClang_DIR=/path/to/lib/cmake/clang` if
discovery fails. Souffle support is optional and falls back to a stub when
unavailable.

Set up and run the canonical Python suite:

```bash
python -m pip install -e './python[dev]'
pytest python/tests
```

`uv` is also supported from `python/`:

```bash
uv sync --dev
uv run pytest
```

Build and run the hermetic C++ suite:

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build -L default --output-on-failure
```

Useful targeted gates:

```bash
ctest --test-dir build -L clang --output-on-failure
ctest --test-dir build -L parity --output-on-failure
```

The `clang` tests perform real parses. The `parity` test needs `uv`, SQLite,
the Python launcher, and the built C++ executable. `make`, `make test`, and
`make static` are convenience wrappers; CMake remains the source of truth.

Before committing a behavioral change, run both full Python tests and the C++
`default` tests. Run the `clang` and `parity` gates when the change touches
parsing, indexing, storage interchange, CLI behavior, or graph results.

## Git workflow

- Agents may create Git worktrees and feature branches when starting new work.
- Keep each worktree and branch scoped to a single task, and do not remove a
  worktree or branch that may contain another contributor's work.

## Change discipline

- Treat Python behavior as the reference, but do not leave C++ parity for a
  later change.
- Add focused regression coverage in both suites for shared behavior. Match
  existing nearby test structure and fixtures.
- Preserve database compatibility. Schema changes need matching version bumps,
  migrations, and tests for opening older databases in both implementations.
- Keep text and JSON output deterministic. Do not casually change ordering,
  field names, null handling, or formatting.
- Keep fixtures read-only during tests; copy database fixtures to a temporary
  directory before migration.
- Avoid generated artifacts in commits: build directories, caches,
  `__pycache__`, temporary databases, and local virtual environments.
- Create agent-only temporary files exclusively under `/tmp`; never place
  scratch files in the repository working tree.
- Prefer small changes in the existing module boundaries. Do not add a new
  dependency when the standard library or an existing utility already covers
  the need.

## Validation expectations

Report exactly which checks were run and whether any were skipped because of a
missing optional tool or fixture. Do not claim parity from only one language's
tests. When a full suite is impractical during iteration, run the narrow test
first, then the required gates before handoff.
