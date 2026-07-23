# Legacy Python extraction inventory and removal gate

The Python/libclang extractor is retained for compatibility only. Production
source extraction is performed by the C++23 LibTooling core (`cidx index`).
Calling `indexer.clang.index_source` emits `LegacyPythonExtractionWarning`.

| Legacy entry point | Current callers | Replacement | Removal evidence |
|---|---|---|---|
| `indexer.clang.index_source` | `python/indexer/cli.py`, `Storage.index()` and legacy tests | `cidx index` / C++ `src/ast/index_engine.cpp` | zero production callers outside the allowlist; C++ default + clang gates green |
| `indexer.clang.index_symbols` / `index_headers` | legacy AST tests and `index_source` | C++ symbol/header passes | normalized Layer-0 fixture coverage is owned by C++ golden tests |
| `python/indexer/astcache.py` | legacy AST cache tests and compatibility imports | C++ index lifecycle/cache | no supported SDK import depends on the module |
| `python/indexer/astcmd.py` | legacy AST CLI compatibility paths | C++ inspect/ast compatibility command | documented CLI migration and usage inventory show no active users |

## Deprecation policy

The warning is a `DeprecationWarning` subclass so applications can promote it
to an error in CI (`-W error::indexer.clang.LegacyPythonExtractionWarning`).
The compatibility adapter remains available during the current product major;
it is not extended with new extraction semantics. Documentation and examples
must point to the C++ command.

## Removal gate

Removal is permitted only when all of the following are recorded in the
platform release checklist:

1. a usage scan finds no production imports beyond the allowlisted compatibility
   tests;
2. C++ default and clang tests cover every retained extraction contract;
3. the Python storage/query SDK test suite passes without importing the legacy
   extractor;
4. a migration guide covers CLI, cache, diagnostics, and failure behavior; and
5. the compatibility manifest and release notes mark the adapter removed.

Until then, deleting or extending `python/indexer/clang/` is out of scope for
feature work.
