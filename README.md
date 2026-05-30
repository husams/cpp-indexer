# cpp-indexer

A Rust + libclang code indexer that builds a queryable graph of a C++ codebase
and writes it to a graph database. Designed for repository-scale C++
(LLVM-/Chromium-class), with first-class support for indexing across multiple
repositories in a single unified graph.

> **Status:** pre-implementation. Planning and product requirements are
> drafted; M1 (foundations) has not started.

## What it does

- Parses C++ translation units with libclang, using each repo's
  `compile_commands.json` for accurate compile flags.
- Emits a typed graph of C++ entities — classes, methods, functions, fields,
  namespaces, templates and specializations, headers, macros, typedefs,
  enums — and the relationships between them (calls, inherits, includes,
  overrides, instantiates, friend-of, ADL candidate, expands-to, ...).
- Uses libclang's USR (Unified Symbol Resolution) as the global primary key,
  so symbols resolve correctly across translation units and runs.
- Supports a **cross-repo unified graph**: index multiple repositories
  independently, then materialize `EXTERNAL_REF` edges that connect a symbol
  use in repo A to its canonical definition in repo B.

## Graph database backends

Both backends are built into the default binary; pick one at runtime via
config or `--sink`:

- **Neo4j** (via `neo4rs`, Bolt protocol).
- **IndraDB** (via `indradb` + `indradb-proto`, gRPC).

## Surfaces

- `cxg-index <path>` — index a file, directory, or repo. The location of
  `compile_commands.json` is auto-detected by walking upward from the input
  path and probing common build directories.
- `cxg-resolve-cross-repo` — second pass that materializes cross-repo
  reference edges after all repos have been indexed.
- `cxg-daemon` — long-lived daemon that serves a REST API for ingestion,
  job status, repo listing, and reset. Accepts both local paths and git
  HTTP(S) URLs (clone-on-ingest into a configured workspace).

## Design constraints

- **`compile_commands.json` required** (no heuristic synthesis of compile
  flags). Generate it with CMake `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, Bear,
  or `intercept-build`.
- **No LLM at index time.** The indexer is pure C++ → graph; downstream
  agents and MCP tools only see the graph.
- **C++ semantics preserved.** When libclang cannot resolve a reference, the
  edge is emitted with `resolved: false` rather than dropped or approximated.

## Pipeline (overview)

1. **Phase 0.5** — auto-detect `compile_commands.json`.
2. **Phase 0** — bootstrap: parse compile DB, deduplicate, build TU work queue.
3. **Phase 1** — parallel per-TU walk with libclang. Output is staged to
   Parquet shards; no graph DB writes.
4. **Phase 2** (optional) — heavier per-TU analyses (control flow, exception
   specs, constexpr, macro-expansion provenance).
5. **Phase 3** — per-repo cross-TU reference resolution in memory via a
   `HashMap<USR, NodeMeta>`. Unresolved edges are flagged as cross-repo
   candidates.
6. **Phase 4** — bulk-write to the chosen graph DB in batched `UNWIND` /
   batched gRPC transactions.
7. **Phase 5** — global cross-repo resolution. Materialize `EXTERNAL_REF`
   edges between repos using a canonical-source-of-truth rule per USR.

## Roadmap

Seven milestones (M1 → M7) targeting v1 GA in approximately 12 weeks (solo).
See `docs/` once it's populated, or the project planning notes referenced in
the development log.

## PCM / C++20 module support

The indexer supports translation units that consume Clang precompiled modules
(`.pcm` files). **libclang 18** or later is required for module-capable parsing.

Supported compile flags detected from `compile_commands.json`:

- `-fmodules` — implicit module map support
- `-fmodule-file=<name>=<path>` or `-fmodule-file=<path>` — named prebuilt `.pcm`
- `-fprebuilt-module-path=<dir>` — directory of prebuilt `.pcm` files

**Best-effort skip posture:** when libclang < 18 is detected at runtime, PCM
TUs are **skipped** with a warning (`failed_tus > 0`) rather than partially
parsed. Non-module TUs in the same run are unaffected.

**Loud failure on missing/invalid `.pcm`:** a missing `.pcm` referenced by
`-fmodule-file=` is detected **before** parse and logged at ERROR level; the TU
is counted as a failure (`failed_tus > 0`, non-zero in the closing summary) and
**no silent partial output** is written. Corrupt or out-of-date `.pcm` files
that produce a `Fatal`-severity libclang diagnostic are caught by a post-parse
gate with the same counted-failure behaviour.

See [`docs/pcm.md`](docs/pcm.md) for the full reference.

## Requirements

- Rust (edition 2021)
- libclang 18 (`libclang.so` / `libclang.dylib` on the dynamic loader path)
- A reachable Neo4j 5.x and/or IndraDB instance
- For each target codebase: a `compile_commands.json` somewhere in or above
  the input path

## License

Apache License 2.0. See [LICENSE](LICENSE).
