# cidx — Implementation Documentation

`cidx` is a semantic indexer for C and C++ codebases. It parses translation
units with the Clang C++ API (LibTooling), extracts a symbol-level relationship
graph, stores it in one SQLite database (`index.db`), and exposes
navigation/query commands (callers, callees, references, hierarchy, virtual
dispatch, impact) plus Datalog analyses.

The C++23 LibTooling implementation is the sole production extractor. The
Python tree is the supported storage/read-query SDK; its libclang extractor is
legacy and emits a deprecation warning. Ownership and compatibility promises
are defined in [the platform contracts](platform/README.md).

This documentation describes the **C++23 implementation** under `src/` — the
sole indexer. (The Python tree `python/indexer/` is storage + read-query only;
its libclang indexer is legacy pending removal.) It is organized as one page
per component plus a few cross-cutting pages. Start with the
[overview](overview.md), then read the module you care about.

## Cross-cutting

| Page | Contents |
|---|---|
| [Overview](overview.md) | what cidx does, the 4 stages, design invariants, module map + layering diagram |
| [Data model](data-model.md) | schema 29 tables, the ER diagram, the three graph layers |
| [Data flow](data-flow.md) | end-to-end pipeline, the index sequence, the per-file interleave, the resolve pass |
| [Immutable FactBatch](fact-batch.md) | natural identities, typed partitions, bounded emitters, canonicalization, and transactional replay |
| [FactBatch artifact v1](fact-batch-artifact-v1.md) | canonical wire framing, compatibility, digest integrity, defensive decode, and bounded spill/transfer |
| [Build & platforms](build.md) | CMake, macOS/RHEL, the Clang C++ API link model |
| [Glossary](glossary.md) | USR, Layer-0/1, driver introspection, stub, multi_def |
| [Query DSL guide](query-dsl.md) | using the CXQ QueryPlan pipeline DSL from C++ and Python, with runnable samples |
| [QueryPlan contract](query-plan.md) | the normative CXQ spec: IR grammar, canonical JSON, validation codes, execution semantics |
| [Module architecture and dependency rules](adr/ADR-011-module-architecture-and-dependency-rules.md) | enforceable layer ownership, dependency direction, exceptions, and review policy |
| [Storage M3 accelerator qualification](benchmarks/storage-m3.md) | optional graph projections, lifecycle evidence, and the custom-store gate |
| [Architecture contribution guide](architecture-contributing.md) | how to add a module, target, or dependency without bypassing the contract |
| [CIDX Graph Search and Navigation Skill](cidx-search-language.md) | draft controlled-English graph search skill, semantic navigation IR, traits, inheritance, quantifiers, aggregation, negation, and backend compilation |
| [Improvement roadmap](improvements/README.md) | prioritized follow-on work, with adoption gates |
| [Extension packages](modules/extensions.md) | versioned declarative packages, registries, lockfiles, and conformance |

## Modules (`src/`)

| Module | Page | Role |
|---|---|---|
| `cli/` | [cli](modules/cli.md) | command dispatch, arg parsing, output formatting |
| `storage/` | [storage](modules/storage.md) | the SQLite layer: `Storage`, schema, migrations, resolve pass |
| `compiledb/` | [compiledb](modules/compiledb.md) | `compile_commands.json` load, flag sanitize, include aliasing |
| `ast/` | [ast](modules/ast.md) | the indexing engine: RAV visitors → Layer-0 rows |
| `toolchain/` | [toolchain](modules/toolchain.md) | driver introspection (replicated include search paths) |
| `graph/` | [graph](modules/graph.md) | read-only `GraphQuery` + emitters |
| `astgraph/` | [astgraph](modules/astgraph.md) | `cidx-astgraph`: per-TU raw AST graph + native Souffle |
| `util/` | [util](modules/util.md) | logger, env, subprocess, hashing, pathutil, repo, files |

## Conventions used in these docs

- File anchors are written `file:line` relative to the repo root.
- **Layer-0** = raw AST-extracted rows (symbols + edges); **Layer-1** = the
  derived design graph produced by `cidx resolve`. See the [data model](data-model.md).
- Two invariants recur everywhere: **deterministic semantic output** (text/JSON
  output is stable; database row SETS are the contract, not insertion order —
  see [ast](modules/ast.md#record-ordering)), and **driver introspection**
  (replicating a specific compiler's include paths). See the [overview](overview.md).
