# cpp-indexer

`cpp-indexer` contains the C++23 production core and a supported Python SDK:

- the C++23 LibTooling implementation under `src/`, built with CMake;
- the Python storage/read-query SDK in `python/`, packaged as the
  pip-installable `cidx-indexer` project.

The previous Rust, Neo4j, IndraDB, daemon, and C++ libclang C API
implementations are retired. Python's libclang extractor remains only as a
deprecation-boundary adapter; new extraction behavior belongs in C++.

## Build the C++ tool

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build -L default --output-on-failure
```

The resulting executable is `build/cidx`.

## Normative TLA+ specification

The executable behavioral contract for CIDX lives in
[`spec/tla/README.md`](spec/tla/README.md). Run its pinned syntax and TLC
smoke gate with `spec/tla/tools/check.sh`; the CI workflow reports that result
separately from the C++ test job.

## Install the Python API

```bash
python -m pip install ./python
```

This installs the `indexer` Python package and the `indexer` and
`cidx-python` console commands. The repository launcher is also available as
`python/cidx`.

## Ownership rule

See the [module architecture and dependency rules](docs/adr/ADR-011-module-architecture-and-dependency-rules.md)
before adding a module or dependency. It is backed by the machine-readable
[module manifest](architecture/cidx-module-manifest.json) and a bootstrap CI gate.

## Compatibility rule

SQLite schema/read behavior and explicitly listed generated contracts are
shared compatibility surfaces. Indexing semantics, CLI behavior, and product
formatting are C++ authority surfaces. See
[docs/platform/ownership.md](docs/platform/ownership.md) and
[docs/platform/versioning-and-compatibility.md](docs/platform/versioning-and-compatibility.md).

Product, database, catalog, artifact, and API versions come from one source:
`spec/platform/version.json`. Run
`uv run --project python python scripts/check_release_contract.py` before
packaging or publishing.
