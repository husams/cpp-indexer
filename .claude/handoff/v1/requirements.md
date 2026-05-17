# cpp-indexer — Requirements (M1–M7 → v1 GA)

run_id: cpp-indexer-v1
stage: 1 of 8 — product-manager
version: 1.0
date: 2026-05-17

## Priority convention

- **P0** — milestone-exit-critical; blocks merge gate / demo criterion
- **P1** — in-milestone required; must ship before milestone is "done" but not the demo gate
- **P2** — nice-to-have within milestone; may slip to the next without blocking

## Locked-in decisions (do not re-litigate — see CHARTER.md)

Rust + libclang 18; both Neo4j and IndraDB mandatory in default binary (no Cargo feature gates); shared `GraphSink` trait; USR global primary key; Parquet staging for Phase 1 output; auto-detect `compile_commands.json` by upward walk + build-dir probe; `cxg-daemon` first-class with REST control plane; binaries `cxg-index` / `cxg-resolve-cross-repo` / `cxg-daemon`; public-facing names use **cpp-indexer** only.

---

## Milestone M1 — Foundations

> **Exit criterion:** `cxg-index` produces an isomorphic graph vs the Python CodexGraph reference on a 5-file C++ fixture, verified against **both** Neo4j and IndraDB sinks.

---

### Story M1-S1: Crate skeleton and base schema

As a developer, I want a buildable Rust crate skeleton with the base CodexGraph node and edge types modelled as Rust enums and Arrow schemas, so that all subsequent milestone work has a stable type-safe foundation.

**Acceptance criteria:**

- AC-M1-1: Given a fresh checkout, when `cargo build` is run, then the crate compiles without errors on Linux and macOS with libclang 18 linked.
- AC-M1-2: Given the schema module, when it is compiled, then all base node types (`MODULE`, `CLASS`, `FUNCTION`, `METHOD`, `FIELD`, `GLOBAL_VARIABLE`) and their required attributes are present as typed Rust structs/enums.
- AC-M1-3: Given the schema module, when it is compiled, then all base edge types (`CONTAINS`, `HAS_METHOD`, `HAS_FIELD`, `INHERITS`, `USES`, `CALLS`) are present.
- AC-M1-4: Given the crate, when `cargo test` is run, then unit tests for the schema enum ↔ Arrow schema round-trip pass.

Priority: P0
Dependencies: libclang 18 dev headers in build env
Open questions: none (schema locked)
References: PRD §6.1 FR-S1, FR-S6, FR-S7; plan §Schema; plan §M1

---

### Story M1-S2: Phase 0 — compile_commands.json parser

As a developer, I want a Phase 0 module that reads `compile_commands.json`, deduplicates entries by `(file, args)` hash, and builds a TU work queue, so that the indexer has a reliable, deterministic input list.

**Acceptance criteria:**

- AC-M1-5: Given a valid `compile_commands.json`, when Phase 0 runs, then all entries are parsed and de-duplicated by Blake3 hash of `(file, args)`.
- AC-M1-6: Given a `compile_commands.json` with duplicate `(file, args)` entries, when Phase 0 runs, then each unique pair appears exactly once in the work queue.
- AC-M1-7: Given a malformed or missing `compile_commands.json` path (when auto-detection has already resolved the path to a concrete file), when Phase 0 attempts to parse it, then it exits non-zero with an error naming the file.

Priority: P0
Dependencies: M1-S1
Open questions: Q8 — Bazel `compile_commands.json` equivalents not in scope for v1 (CMake/Bear only; deferred per CHARTER)
References: PRD §6.2 FR-P1; plan §Phase 0; plan §M1

---

### Story M1-S3: Phase 0.5 — auto-detect compile_commands.json

As Atlas, I can point `cxg-index` at a file, directory, or repo root without specifying `--compile-commands`, and the indexer locates the correct `compile_commands.json` automatically, so that common-case use requires no manual configuration.

**Acceptance criteria:**

- AC-M1-8: Given a file path `<repo>/src/foo.cpp` with `compile_commands.json` present at `<repo>/build/`, when `cxg-index <repo>/src/foo.cpp` is run (no `--compile-commands` flag), then the indexer finds the file via upward walk + `build/` probe and logs the resolved path at INFO level.
- AC-M1-9: Given a path with no `compile_commands.json` anywhere in the upward walk, when `cxg-index` runs, then it exits non-zero and prints a message listing every directory it searched; no heuristic fallback is attempted.
- AC-M1-10: Given a path whose upward walk reaches a `.git` directory, when no `compile_commands.json` has been found yet at or below that level, then the walk stops at `.git` and the "not found" error is emitted.
- AC-M1-11: Given multiple `compile_commands.json` candidates at the same directory level (e.g., both `build/` and `out/`), when the input is a specific file, then the candidate whose entry list contains that file is preferred; otherwise the lexicographically first candidate is chosen.
- AC-M1-12: Given a directory input (not a single file), when `compile_commands.json` is found, then only entries whose `file` field is under the input directory (after path normalization) are included in the TU work queue.
- AC-M1-13: Given a single-file input, when `compile_commands.json` is found, then only the entry matching that file is included.

Priority: P0
Dependencies: M1-S2
Open questions: Q8 (Bazel probing) — explicitly out of scope for v1
References: PRD §6.6 FR-AD-1..7, US-01, US-02, US-05b, US-05c; plan §Phase 0.5

---

### Story M1-S4: Phase 1 — base libclang visitor + Parquet staging

As a developer, I want a Phase 1 visitor that walks each TU with libclang and emits base CodexGraph nodes and edges as Parquet shards (no DB writes), so that the pipeline can later scale Phase 1 to parallel workers without contention.

**Acceptance criteria:**

- AC-M1-14: Given a TU in the work queue, when Phase 1 processes it, then nodes for each `MODULE`, `CLASS`, `FUNCTION`, `METHOD`, `FIELD`, and `GLOBAL_VARIABLE` in that TU are written to a per-worker Parquet shard.
- AC-M1-15: Given a TU, when Phase 1 processes it, then no writes are made to Neo4j or IndraDB during Phase 1.
- AC-M1-16: Given a TU that causes a libclang parse error or panic, when Phase 1 processes it, then the error is recorded as a diagnostic and processing continues on remaining TUs; the run does not abort.
- AC-M1-17: Given Phase 1 output, when the Parquet shards are read, then each node row contains a non-empty `usr` field (via `clang_getCursorUSR`).

Priority: P0
Dependencies: M1-S1, M1-S2, M1-S3
Open questions: none
References: PRD §6.2 FR-P2, FR-S6, US-05; plan §Phase 1; plan §M1

---

### Story M1-S5: Phase 3 — trivial in-memory resolution

As a developer, I want a trivial Phase 3 that reads all Parquet shards into a `HashMap<USR, NodeMeta>` and resolves within-repo edges, so that Phase 4 can write a complete single-repo graph.

**Acceptance criteria:**

- AC-M1-18: Given Phase 1 Parquet shards from a 5-file fixture, when Phase 3 runs, then all within-repo edge targets are resolved by USR lookup and written to a final-edges Parquet file.
- AC-M1-19: Given an edge whose target USR is not found in the current repo's shard set, when Phase 3 runs, then that edge is flagged `cross_repo_candidate: true` (not dropped).

Priority: P0
Dependencies: M1-S4
Open questions: none
References: PRD §6.2 FR-P4; plan §Phase 3; plan §M1

---

### Story M1-S6: Both Neo4j and IndraDB sinks behind GraphSink trait

As Atlas, I can choose `--sink neo4j` or `--sink indradb` at runtime without rebuilding the binary, so that the indexer works with either graph DB without separate builds.

**Acceptance criteria:**

- AC-M1-20: Given a compiled `cxg-index` binary, when `--sink neo4j` is passed with valid Neo4j credentials, then Phase 4 writes nodes and edges to Neo4j via a single-transaction write (batching deferred to M3).
- AC-M1-21: Given a compiled `cxg-index` binary, when `--sink indradb` is passed with a valid IndraDB endpoint, then Phase 4 writes nodes and edges to IndraDB.
- AC-M1-22: Given the `GraphSink` trait, when both `Neo4jSink` and `IndraDbSink` implement it, then a `cargo test` run exercises both via the same integration fixture test.
- AC-M1-23: Given `cxg-index.toml`, when `password_env` / `token_env` fields are set, then sink credentials are read from the named env var at runtime; no secret is read from the config file itself or logged.
- AC-M1-24: Given a missing or wrong sink credential env var, when `cxg-index` starts, then it exits non-zero before any indexing with a clear error message identifying the missing variable.

Priority: P0
Dependencies: M1-S1, M1-S5
Open questions: IndraDB gRPC endpoint must be provisioned by devops before M1 integration tests can run (see PRD §10 dependencies)
References: PRD §6.4 FR-B1..B4, US-05a; plan §Module layout sink/; plan §M1

---

### Story M1-S7: M1 exit gate — isomorphic graph fixture test

As the team, we need an automated test that verifies `cxg-index` produces a graph isomorphic to the Python CodexGraph reference on a 5-file fixture against both sinks, so that M1 is demonstrably correct before M2 begins.

**Acceptance criteria:**

- AC-M1-25: Given the 5-file C++ fixture in `tests/fixtures/`, when `cxg-index` runs against it with `--sink neo4j`, then `cargo test` asserts node count, edge count, and a subset of specific USR-keyed nodes match the golden-graph snapshot.
- AC-M1-26: Given the same fixture, when `cxg-index` runs with `--sink indradb`, then the same assertions pass (graph is isomorphic modulo internal node IDs).
- AC-M1-27: Given the golden-graph snapshot, when the test fails, then the output diff identifies which nodes/edges are missing or extra.

Priority: P0
Dependencies: M1-S6
Open questions: none
References: PRD §9 M1 demo gate; plan §M1 exit criterion; PRD §8.3

---

## Milestone M2 — C++ Extensions

> **Exit criterion:** Boost.Optional (single-header library) is fully indexed; all reference edges resolve without `cross_repo_candidate` (within-repo).

---

### Story M2-S1: C++ extension node types

As a developer, I want the Phase 1 visitor extended to emit `NAMESPACE`, `TEMPLATE_DECL`, `SPECIALIZATION`, `TYPEDEF`, `ENUM`, and `HEADER` nodes, so that the graph captures C++ semantics beyond the base CodexGraph schema.

**Acceptance criteria:**

- AC-M2-1: Given a C++ file with named namespaces, when Phase 1 processes it, then a `NAMESPACE` node with the qualified name and USR is emitted for each unique namespace.
- AC-M2-2: Given a C++ file with a class or function template, when Phase 1 processes it, then a `TEMPLATE_DECL` node with `kind`, `name`, and `params` is emitted.
- AC-M2-3: Given a C++ file with a template specialization or instantiation, when Phase 1 processes it, then a `SPECIALIZATION` node with `template_usr` and `template_args` is emitted.
- AC-M2-4: Given a C++ file with `#include` directives, when Phase 1 processes it, then a `HEADER` node is emitted for each included header file.
- AC-M2-5: Given a C++ file with `typedef` and `using` aliases, when Phase 1 processes it, then a `TYPEDEF` node with `underlying_type_usr` is emitted.
- AC-M2-6: Given a C++ file with `enum` and `enum class` declarations, when Phase 1 processes it, then an `ENUM` node with the `scoped` boolean is emitted.

Priority: P0
Dependencies: M1-S4
Open questions: none
References: PRD §6.1 FR-S2; plan §Schema Nodes; plan §M2

---

### Story M2-S2: C++ extension edge types

As a developer, I want the Phase 1 visitor to emit `INCLUDES`, `OVERRIDES`, `INSTANTIATES`, `SPECIALIZES`, `FRIEND_OF`, and `ADL_CANDIDATE` edges, so that C++ structural relationships are first-class in the graph.

**Acceptance criteria:**

- AC-M2-7: Given a module that `#include`s a header, when Phase 1 processes it, then an `INCLUDES` edge from the module to the `HEADER` node is emitted.
- AC-M2-8: Given a virtual method that overrides a base class method, when Phase 1 processes it, then an `OVERRIDES` edge with `vtable_slot` is emitted (derived from `clang_getOverriddenCursors`).
- AC-M2-9: Given a call site that instantiates a template, when Phase 1 processes it, then an `INSTANTIATES` edge from the call site to the `TEMPLATE_DECL` is emitted.
- AC-M2-10: Given a partial or full template specialization, when Phase 1 processes it, then a `SPECIALIZES` edge from the `SPECIALIZATION` to the `TEMPLATE_DECL` is emitted.
- AC-M2-11: Given a class with a `friend` declaration, when Phase 1 processes it, then a `FRIEND_OF` edge is emitted.
- AC-M2-12: Given an unresolved reference that could be resolved by ADL, when Phase 1 processes it, then an `ADL_CANDIDATE` edge is emitted.

Priority: P0
Dependencies: M2-S1
Open questions: none
References: PRD §6.1 FR-S4, FR-S7, FR-S8; plan §Schema Edges; plan §M2

---

### Story M2-S3: Unresolved references and system header filtering

As Atlas, I want unresolved references emitted as partial nodes (not silently dropped) and system headers excluded from the core graph by default, so that the graph is complete without being polluted by stdlib internals.

**Acceptance criteria:**

- AC-M2-13: Given an edge whose target USR cannot be resolved within the repo, when Phase 3 runs, then the edge is emitted with `resolved: false` (not dropped).
- AC-M2-14: Given a TU that includes a system header under `/usr/include/` or a compiler-internal path, when `skip_system_headers: true` is set in config, then nodes and edges from those headers are excluded from Parquet output.
- AC-M2-15: Given `skip_system_headers: false`, when Phase 1 runs, then system header nodes are included.

Priority: P1
Dependencies: M2-S2
Open questions: none
References: PRD §6.1 FR-S8; plan §Phase 1; plan §M2

---

### Story M2-S4: M2 exit gate — Boost.Optional fixture

As the team, we need an integration test verifying Boost.Optional indexes completely (all within-repo references resolved), so that M2 is demonstrably correct before M3 begins.

**Acceptance criteria:**

- AC-M2-16: Given the Boost.Optional single-header source as a fixture, when `cxg-index` runs, then `cargo test` asserts zero `cross_repo_candidate: true` edges in the output (all within-repo references resolve).
- AC-M2-17: Given the same fixture, when the graph is queried, then template specialization nodes and `SPECIALIZES` edges for `boost::optional<T>` are present.

Priority: P0
Dependencies: M2-S1, M2-S2, M2-S3
Open questions: Boost checkout must be acquired as a fixture by the team before M2 integration tests
References: PRD §9 M2 demo gate; plan §M2 exit criterion

---

## Milestone M3 — Performance & Scale

> **Exit criterion:** Full LLVM index in ≤20 min on a 32-core box; incremental re-index after one-file change in ≤1 min; both measured against Neo4j.

---

### Story M3-S1: Parallel Phase 1 with rayon + thread-local clang Index

As Atlas, I want Phase 1 to process TUs in parallel across all available CPU cores with a thread-local `clang::Index` per worker, so that wall-clock indexing time scales with hardware.

**Acceptance criteria:**

- AC-M3-1: Given a work queue of N TUs and W workers, when Phase 1 runs on a W-core machine, then all N TUs are processed and the wall time is measurably less than sequential processing (verified on the LLVM fixture with 32 cores).
- AC-M3-2: Given rayon parallelism, when a worker-local `clang::Index` panics or segfaults on one TU, then other workers continue; the failed TU is recorded as a diagnostic.
- AC-M3-3: Given the LLVM source checkout (~25k TUs), when Phase 1 runs with 32 workers, then wall time is ≤15 min for Phase 1 alone.

Priority: P0
Dependencies: M2-S2
Open questions: Q2 — one graph per build config vs merged graph; default is separate graphs per config tuple (no change to M3 scope)
References: PRD §7 NFR performance; plan §Phase 1; plan §M3

---

### Story M3-S2: Batched Phase 4 writes — Neo4j and IndraDB

As a developer, I want Phase 4 to write nodes and edges in batched `UNWIND CREATE` transactions with configurable batch size and concurrent sessions, so that Neo4j throughput meets the ≥50k rows/s target and IndraDB meets ≥100k rows/s.

**Acceptance criteria:**

- AC-M3-4: Given default config (`batch_size=10000, sessions=16`), when Phase 4 writes to Neo4j, then the measured throughput is ≥50k rows/s on the LLVM fixture.
- AC-M3-5: Given default config, when Phase 4 writes to IndraDB, then the measured throughput is ≥100k rows/s on the LLVM fixture.
- AC-M3-6: Given a write failure mid-batch to either sink, when Phase 4 retries, then no duplicate nodes are written (idempotent write via USR-keyed MERGE or equivalent).

Priority: P0
Dependencies: M1-S6, M3-S1
Open questions: none
References: PRD §7 NFR throughput; plan §Phase 4; plan §M3

---

### Story M3-S3: Content-hash cache for incremental re-indexing

As Atlas, I can run `cxg-index` again after editing one file and have only the changed TUs re-indexed, so that CI re-index after a commit takes ≤1 min.

**Acceptance criteria:**

- AC-M3-7: Given a completed index run, when `cxg-index` is run again with no file changes, then zero TUs are processed by Phase 1 (all are cache hits) and the run exits in under 30 seconds.
- AC-M3-8: Given a completed index run, when one source file is modified and `cxg-index` re-runs, then only the TU(s) whose `(source_hash, args_hash)` changed are re-processed by Phase 1.
- AC-M3-9: Given a cache from a previous run with a different libclang version or schema version, when `cxg-index` starts, then the cache is invalidated and all TUs are re-processed.
- AC-M3-10: Given an incremental run on the LLVM fixture (one file changed), when `cxg-index` completes, then wall time is ≤1 min end-to-end.

Priority: P0
Dependencies: M3-S1
Open questions: none
References: PRD §6.3 FR-C1..C4, US-03, G5; plan §M3

---

### Story M3-S4: Memory budget enforcement

As a developer, I want Phase 1 peak memory capped at ≤16 GB and Phase 3 USR map spillable to RocksDB when it exceeds 8 GB, so that the indexer does not OOM on LLVM-class repos.

**Acceptance criteria:**

- AC-M3-11: Given the LLVM fixture on a 32-core machine, when Phase 1 completes, then peak RSS is ≤16 GB.
- AC-M3-12: Given a Phase 3 USR map that exceeds 8 GB in memory, when Phase 3 runs, then the map spills excess entries to a RocksDB store under `.cxg-cache/` and the run completes without OOM.

Priority: P1
Dependencies: M3-S1, M3-S3
Open questions: none
References: PRD §7 NFR memory; plan §Performance budget

---

### Story M3-S5: Progress reporting

As Atlas, I can see real-time progress (nodes/sec, edges/sec, TUs done/total) on stderr while indexing, so that I can estimate time-to-completion and detect hangs.

**Acceptance criteria:**

- AC-M3-13: Given an indexing run, when Phase 1 is active, then stderr shows TUs done / total, nodes/sec, and edges/sec updated at least once per 5 seconds.
- AC-M3-14: Given an indexing run with a cache hit, when Phase 1 skips a TU, then the progress counter counts it as done immediately.

Priority: P1
Dependencies: M3-S1
Open questions: none
References: PRD US-04; plan §M3

---

## Milestone M4 — Cross-Repo

> **Exit criterion:** Two-repo `EXTERNAL_REF` demo: index two fixtures, run `cxg-resolve-cross-repo`, and a Cypher query traverses an `EXTERNAL_REF` edge from repo A to repo B.

---

### Story M4-S1: REPO nodes and BELONGS_TO_REPO edges

As Atlas, I can filter any graph query by repo using `BELONGS_TO_REPO` edges, so that I know the provenance of every node.

**Acceptance criteria:**

- AC-M4-1: Given a repo indexed with `cxg-index`, when the graph is written, then a `REPO` node exists with `name`, `root_path`, `commit_sha`, and `commit_date` attributes.
- AC-M4-2: Given any node emitted by the indexer, when the graph is written, then that node has a `BELONGS_TO_REPO` edge to its repo's `REPO` node.
- AC-M4-3: Given a `REPO` node, when it records which sink backend was used, then `cxg-resolve-cross-repo` can detect heterogeneous setups (Neo4j + IndraDB) and refuse with a clear error.

Priority: P0
Dependencies: M1-S6
Open questions: none
References: PRD §6.1 FR-S3, FR-S5, US-07; plan §Schema REPO; plan §M4

---

### Story M4-S2: cross_repo_candidate flag and Phase 5 binary

As Atlas, I can run `cxg-resolve-cross-repo` after indexing two or more repos to materialize `EXTERNAL_REF` edges between them, so that cross-repo call graphs are queryable in one graph DB.

**Acceptance criteria:**

- AC-M4-4: Given two repos indexed into the same Neo4j database, when `cxg-resolve-cross-repo --config <toml>` is run, then all `cross_repo_candidate: true` edges whose target USR exists in another repo are materialized as `EXTERNAL_REF` edges with `via:<original_edge_type>`.
- AC-M4-5: Given a `cross_repo_candidate` edge whose target USR is not found in any indexed repo, when Phase 5 runs, then the edge remains with `resolved: false` and is logged as a diagnostic.
- AC-M4-6: Given two repos running Phase 5 simultaneously, when an advisory lock is in place, then Phase 5 waits for the lock before writing `EXTERNAL_REF` edges (no race condition).

Priority: P0
Dependencies: M4-S1, M1-S5
Open questions: Q4 — cross-repo schema versioning: if repos were indexed with different schema versions, Phase 5 MUST refuse with an error (default resolution per CHARTER; architect resolves in ADR)
References: PRD §6.2 FR-P4, FR-P6, US-06; plan §Phase 5; plan §M4

---

### Story M4-S3: System-header canonicalization

As Atlas, I want system-header USRs resolved to canonical `system:libstdc++` or `system:libc` repo nodes (not user repos), so that STL symbols do not pollute cross-repo attribution.

**Acceptance criteria:**

- AC-M4-7: Given a USR that resolves to a path under `/usr/include/**` or a known compiler-internal path, when Phase 5 canonicalises it, then the target is pinned to the `system:libstdc++` or `system:libc` `REPO` node, regardless of which user repo was indexed first.
- AC-M4-8: Given a vendored copy of a third-party header under `third_party/<pkg>/`, when Phase 5 runs, then it is assigned a `repo:vendored:<pkg>` `REPO` node, distinct from both the user repo and the upstream `REPO` node.

Priority: P1
Dependencies: M4-S2
Open questions: none
References: PRD §6.2 FR-P4 canonicalisation, US-08, US-09; plan §Cross-repo design; plan §M4

---

### Story M4-S4: M4 exit gate — two-repo EXTERNAL_REF demo

As the team, we need an integration test confirming `EXTERNAL_REF` edges materialize correctly on a two-repo fixture, so that M4 is demonstrably correct.

**Acceptance criteria:**

- AC-M4-9: Given two fixture repos (`lib-a` calls a function in `lib-b`), when both are indexed and `cxg-resolve-cross-repo` runs, then a Cypher query `MATCH p=()-[:EXTERNAL_REF]->() RETURN p LIMIT 1` returns at least one path.
- AC-M4-10: Given the two-repo fixture, when the `EXTERNAL_REF` edge is queried, then its `via` attribute equals `CALLS`.

Priority: P0
Dependencies: M4-S2, M4-S3
Open questions: none
References: PRD §9 M4 demo gate; plan §M4 exit criterion

---

## Milestone M5 — Macros & Modules

> **Exit criterion:** Chromium subset (`base/`, `net/`) indexes without segfault; macro-heavy code produces sensible `EXPANDS_TO` edges.

---

### Story M5-S1: MACRO nodes and EXPANDS_TO edges

As a developer, I want `MACRO` nodes and `EXPANDS_TO` edges emitted for top-level macro expansions, so that macro-heavy C++ codebases (LLVM `.def` files, Chromium) are represented in the graph.

**Acceptance criteria:**

- AC-M5-1: Given a C++ file with object-like and function-like macros, when Phase 1 processes it, then a `MACRO` node with `kind`, `name`, `file_path`, and `params` is emitted for each macro definition.
- AC-M5-2: Given a macro invocation at call-site level, when Phase 1 processes it, then an `EXPANDS_TO` edge from the call site to the `MACRO` node is emitted.
- AC-M5-3: Given a deeply nested macro expansion, when Phase 1 processes it, then only top-level macro expansions produce `EXPANDS_TO` edges; nested expansions are not individually emitted (to prevent edge explosion).
- AC-M5-4: Given the LLVM `.def` file fixtures, when Phase 1 processes them, then `EXPANDS_TO` edge count is bounded (no more than 10× the number of source lines in the file).

Priority: P0
Dependencies: M2-S2
Open questions: none
References: PRD §6.1 FR-S2, FR-S4 (EXPANDS_TO); plan §Phase 1 macros.rs; plan §M5

---

### Story M5-S2: Phase 2 — optional deep decoration

As a developer, I want Phase 2 (control flow, exception specs, constexpr, macro-expansion provenance) available as an opt-in behind `--skip-phase2` flag, so that fast runs omit expensive analyses.

**Acceptance criteria:**

- AC-M5-5: Given `--skip-phase2` is NOT passed, when the indexer runs, then Phase 2 decorates TU ASTs with control-flow and exception-spec annotations in Parquet.
- AC-M5-6: Given `--skip-phase2` is passed, when the indexer runs, then Phase 2 is entirely skipped and wall time is no greater than M3 baseline.

Priority: P1
Dependencies: M3-S1
Open questions: none
References: PRD §6.2 FR-P3; plan §Phase 2; plan §M5

---

### Story M5-S3: C++20 modules support (conditional)

As Atlas, I want C++20 module imports (`.cppm`, `.pcm`) indexed when libclang 18 supports them, with a documented fallback when they are unavailable, so that the graph handles modern C++ without breaking on older toolchains.

**Acceptance criteria:**

- AC-M5-7: Given a build environment where libclang 18 supports C++20 module interfaces, when a `.cppm` file is in the TU work queue, then Phase 1 processes it and emits nodes/edges for exported declarations.
- AC-M5-8: Given a build environment where libclang 18 does NOT support C++20 modules, when a `.cppm` file is encountered, then the indexer logs a warning, skips that TU, and continues without aborting.
- AC-M5-9: Given `cxg-index --version` output, when C++20 module support is unavailable at runtime, then the output includes a note indicating the limitation.

Priority: P1
Dependencies: M5-S1
Open questions: Q3 — C++20 modules: implement now with caveats (if libclang 18 supports it) or defer to libclang 19+. Architect MUST resolve in ADR before M5 developer dispatch.
References: PRD §7 compatibility C++20; plan §M5; PRD Q3

---

### Story M5-S4: Chromium fixture exit gate

As the team, we need the Chromium `base/` + `net/` subtree to index without segfault and macro-heavy code to produce valid `EXPANDS_TO` edges, so that M5 is demonstrably correct.

**Acceptance criteria:**

- AC-M5-10: Given the Chromium `base/` and `net/` subtree as a fixture, when `cxg-index` runs, then it completes without segfault and exit code is zero.
- AC-M5-11: Given the same run, when the Parquet output is inspected, then at least one `MACRO` node and one `EXPANDS_TO` edge are present.

Priority: P0
Dependencies: M5-S1, M5-S2
Open questions: Chromium checkout must be acquired as a fixture by the team before M5 integration tests
References: PRD §9 M5 demo gate; plan §M5 exit criterion

---

## Milestone M6 — Agent Integration

> **Exit criterion:** The CodexGraph Streamlit agent correctly answers "what classes inherit from `Foo`?" against an LLVM-indexed graph.

---

### Story M6-S1: Build-time schema prompt regeneration

As Nina (the agent), I want `prompt/graph_database/cpp/schema.txt` regenerated from the Rust schema enum at build time, so that the agent's translator prompt never drifts from the actual graph schema.

**Acceptance criteria:**

- AC-M6-1: Given the schema enums in `src/schema/`, when `cargo build` (or a dedicated build script) runs, then `prompt/graph_database/cpp/schema.txt` is updated to reflect all current node and edge types.
- AC-M6-2: Given a schema change that adds or removes a node or edge type, when `cargo build` runs, then the generated `schema.txt` reflects the change without manual intervention.
- AC-M6-3: Given `schema.txt` in the repository, when it differs from what `cargo build` would generate, then CI fails with a message instructing the developer to regenerate.

Priority: P0
Dependencies: M2-S1, M2-S2
Open questions: Q5 — cpp-mcp boundary: does the schema prompt live in this repo or in the cpp-mcp repo? **Architect MUST resolve in adr-1.md before M6 developer dispatch (blocker per CHARTER).** All M6 stories are blocked on Q5 ADR.
References: PRD §6.6 FR-A1; plan §prompt/codegen.rs; plan §M6; PRD Q5

---

### Story M6-S2: C++ idiom examples file

As Nina, I want `prompt/graph_database/cpp/example.txt` with representative C++ query idioms (template, override, namespace, include), so that the agent's translator produces syntactically correct Cypher for C++ patterns.

**Acceptance criteria:**

- AC-M6-4: Given `prompt/graph_database/cpp/example.txt`, when it is read, then it contains at least one example each for: template instantiation query, method override traversal, namespace-filtered lookup, and `#include` edge traversal.
- AC-M6-5: Given the example file, when the CodexGraph agent uses it as a translator prompt supplement, then the agent produces valid Cypher for at least 3 of the 4 idiom categories without syntax errors.

Priority: P1
Dependencies: M6-S1
Open questions: Q5 (same blocker as M6-S1)
References: PRD §6.6 FR-A2; plan §M6

---

### Story M6-S3: Schema version field for drift detection

As Devon, I want a `schema_version` field in the graph DB so that cpp-mcp and the CodexGraph agent can detect version mismatches and refuse stale queries, so that schema-prompt drift is caught early.

**Acceptance criteria:**

- AC-M6-6: Given a graph written by `cxg-index`, when the Neo4j or IndraDB database is queried, then a `SchemaVersion` node (or equivalent metadata entry) with the current schema version string is present.
- AC-M6-7: Given a `schema_version` mismatch between the indexed graph and the running cpp-mcp, when cpp-mcp attempts a query, then it returns an error identifying the version conflict rather than returning potentially wrong results.

Priority: P1
Dependencies: M6-S1
Open questions: Q5 (same blocker); Q4 — cross-repo schema versioning also applies here (architect resolves in ADR)
References: PRD §6.6 FR-A3; PRD Q4, Q5; plan §M6

---

### Story M6-S4: M6 exit gate — Streamlit agent inheritance query

As the team, we need the CodexGraph Streamlit agent to correctly answer an inheritance query against an LLVM-indexed graph, so that M6 is demonstrably correct.

**Acceptance criteria:**

- AC-M6-8: Given an LLVM-indexed graph and the CodexGraph Streamlit agent configured with the cpp schema prompt, when the query "what classes inherit from `Foo`?" is issued, then the agent returns a correct, non-empty answer referencing at least one real LLVM class hierarchy.
- AC-M6-9: Given 10 hand-written NL questions about the LLVM graph, when the agent answers them, then at least 8/10 are graded correct by manual review.

Priority: P0
Dependencies: M6-S1, M6-S2, M6-S3
Open questions: Q5 (blocker — must be resolved before this story can be dispatched); CodexGraph agent (Python) must be available and reachable
References: PRD §8.1 launch criterion; plan §M6 exit criterion; PRD US-10, US-11, US-12

---

## Milestone M7 — Ops + REST API

> **Exit criterion:** `cxg-daemon` runs unattended for one week on hermes-agent; git-URL ingest of a public repo round-trips through `POST /v1/ingest`.

---

### Story M7-S1: cxg-daemon binary and REST control plane

As Devon, I can start `cxg-daemon --config <toml>` and use its REST API to trigger, monitor, and reset indexing jobs from CI or a shell script, so that indexing is automatable without direct shell access to the indexer host.

**Acceptance criteria:**

- AC-M7-1: Given a running `cxg-daemon`, when `POST /v1/ingest` is called with a local path body, then it returns `202 Accepted` with a `job_id` within 50 ms p99.
- AC-M7-2: Given a `job_id` from M7-AC-1, when `GET /v1/jobs/{id}` is called, then it returns the job state (`queued|running|done|failed`), current phase (0–5), and progress fields (`tus_done`, `tus_total`, `nodes`, `edges`) within 20 ms p99.
- AC-M7-3: Given a `GET /v1/jobs?state=done&limit=10` call, then it returns the 10 most recent completed jobs, most-recent first.
- AC-M7-4: Given a running `cxg-daemon`, when `GET /v1/status` is called, then it returns daemon uptime, queue depth, active worker count, and connectivity status for both Neo4j and IndraDB.
- AC-M7-5: Given a running `cxg-daemon`, when `GET /v1/repos` is called, then it returns a list of all tracked repos with `name`, `root_path`, `commit_sha`, `last_indexed_at`, `node_count`, `edge_count`, and `sink`.
- AC-M7-6: Given all REST write endpoints (`POST /v1/ingest`, `POST /v1/reset`), when called without a valid `Authorization: Bearer <token>` header, then they return `401 Unauthorized`.
- AC-M7-7: Given any REST endpoint, when it encounters an error, then the response body conforms to RFC-7807 `application/problem+json`.
- AC-M7-8: Given `cxg-daemon.toml` with `[api].listen = "127.0.0.1:7878"`, when the daemon starts, then it binds only to that address (TLS termination is the caller's responsibility; no built-in TLS).

Priority: P0
Dependencies: M1-S6, M3-S1
Open questions: Q7 — multi-tenant daemon (one per team vs shared): v1 is single-tenant; architect SHOULD resolve in ADR if scope grows
References: PRD §6.7 FR-API-1..11, US-13, US-16, US-17, US-18, US-20, US-21; plan §api/; plan §M7

---

### Story M7-S2: POST /v1/reset with confirmation token

As Devon, I can reset the graph DB for a specific repo or all repos via REST, with a confirmation token that prevents accidental wipes, so that recovery from bad indexes is safe and scriptable.

**Acceptance criteria:**

- AC-M7-9: Given a `POST /v1/reset` call with `{"target": "repo", "repo_name": "hermes-agent", "confirm_token": "<sha256-of-target-name>"}`, when called with a valid bearer token, then all graph nodes and edges tagged `BELONGS_TO_REPO` → `hermes-agent` are deleted and the staging cache for that repo is cleared.
- AC-M7-10: Given a `POST /v1/reset` call with an incorrect or missing `confirm_token`, when it is received, then the server returns `400 Bad Request` and no data is deleted.
- AC-M7-11: Given `{"target": "all"}` with the correct confirm token, when the call is received, then all repos' graph data and staging caches are wiped.

Priority: P0
Dependencies: M7-S1
Open questions: none
References: PRD §6.7 FR-API-5, US-19; plan §M7

---

### Story M7-S3: Git-URL ingestion via workspace clone manager

As Atlas, I can `POST /v1/ingest` with a git HTTPS URL and have the daemon clone the repo into a configured workspace directory, auto-detect `compile_commands.json`, and index it, so that I can index any public or credentialed repo without shell access.

**Acceptance criteria:**

- AC-M7-12: Given `POST /v1/ingest` with `{"source": {"git_url": "https://github.com/llvm/llvm-project", "ref": "main"}}`, when the URL is on the `allowed_hosts` list, then the daemon clones the repo under `[workspace].dir/<repo-name>-<short-sha>/` and starts indexing.
- AC-M7-13: Given the same `git_url` is posted a second time after an initial clone, when Phase 0.5 auto-detection runs on the workspace clone, then `git fetch` is used instead of a fresh clone.
- AC-M7-14: Given a git URL whose host is NOT in `[workspace].allowed_hosts`, when `POST /v1/ingest` receives it, then it returns `403 Forbidden` immediately without cloning.
- AC-M7-15: Given git HTTPS authentication, when the daemon clones, then credentials are read from the env var named in `[workspace].git_credentials_env`; no PAT appears in logs or API responses.
- AC-M7-16: Given a clone with `default_clone_depth = 1` in config, when the repo is cloned, then `--depth=1` is used (shallow clone).

Priority: P0
Dependencies: M7-S1, M1-S3
Open questions: Q6 — SSH git auth is accepted as deferred to v2 (no further resolution needed)
References: PRD §6.7 FR-API-2, FR-API-11, US-17; plan §workspace/; plan §M7

---

### Story M7-S4: Prometheus metrics endpoint

As Devon, I can scrape `GET /metrics` for Prometheus and alert on libclang error rate, cache hit rate, and indexing throughput, so that the daemon is observable in production.

**Acceptance criteria:**

- AC-M7-17: Given a running `cxg-daemon`, when `GET /metrics` is scraped, then it returns Prometheus text format including: `cxg_nodes_total`, `cxg_edges_total`, `cxg_nodes_per_second`, `cxg_edges_per_second`, `cxg_cache_hit_ratio`, `cxg_libclang_errors_total`, `cxg_queue_depth`.
- AC-M7-18: Given `GET /metrics`, when called without an `Authorization` header, then it returns `200 OK` with metrics (no auth required on this read-only endpoint).
- AC-M7-19: Given `cxg_queue_depth` exceeding the `job_queue_max` configured limit, when `POST /v1/ingest` is received, then it returns `429 Too Many Requests` and `cxg_queue_depth` is visible in the metrics output.

Priority: P0
Dependencies: M7-S1
Open questions: none
References: PRD §6.7 FR-API-8, US-14; PRD §7 NFR observability; plan §M7

---

### Story M7-S5: Docker image and CI

As Devon, I have a Docker image with libclang 18 baked in and a CI pipeline that runs unit + integration tests on every commit to `main`, so that the indexer is deployable and regressions are caught automatically.

**Acceptance criteria:**

- AC-M7-20: Given a `Dockerfile` in the repo root, when `docker build` runs, then the resulting image includes `cxg-index`, `cxg-resolve-cross-repo`, and `cxg-daemon` binaries with libclang 18 linked.
- AC-M7-21: Given a commit to `main`, when CI runs, then it executes `cargo test` (unit + integration) on Linux and macOS; any test failure blocks the merge.
- AC-M7-22: Given a commit that causes >20% regression on the LLVM indexing benchmark, when CI runs, then the benchmark step fails and blocks merge.

Priority: P1
Dependencies: M7-S1
Open questions: none
References: PRD §7 NFR portability; plan §M7; PRD §8.3

---

### Story M7-S6: Runbook — recover from corrupted staging directory

As Devon, I have a runbook for recovering from a corrupted Phase 1 staging directory, so that I can restore the indexer to a healthy state without re-indexing from scratch when possible.

**Acceptance criteria:**

- AC-M7-23: Given `runbook.md` in the handoff directory, when it is read, then it contains step-by-step instructions for: (a) identifying a corrupted `.cxg-cache/` directory, (b) clearing it safely, (c) triggering a full re-index, and (d) verifying the graph is complete after re-index.
- AC-M7-24: Given `POST /v1/reset` with `target: repo`, when it is used as part of the recovery procedure, then the runbook documents the exact `confirm_token` derivation (sha256 of target name).

Priority: P1
Dependencies: M7-S2
Open questions: none
References: PRD US-15; plan §M7

---

### Story M7-S7: M7 exit gate — one-week unattended run + git-URL round-trip

As the team, we need `cxg-daemon` to run unattended for one week on hermes-agent and a git-URL ingest to round-trip successfully, so that M7 / v1 GA criteria are met.

**Acceptance criteria:**

- AC-M7-25: Given `cxg-daemon` running on hermes-agent for 7 consecutive days with auto-reindex enabled, when no manual intervention is performed, then the daemon remains responsive (`GET /v1/status` returns 200) and has completed at least one successful ingest cycle per day.
- AC-M7-26: Given `POST /v1/ingest` with `{"source": {"git_url": "https://github.com/<public-repo>"}}` against a live `cxg-daemon`, when the job completes, then `GET /v1/jobs/{id}` returns `state: done` and the graph DB contains nodes tagged `BELONGS_TO_REPO` → the cloned repo.
- AC-M7-27: Given 7 days of daemon uptime, when `GET /metrics` is scraped at end of period, then `cxg_libclang_errors_total` divided by `cxg_nodes_total` is less than 0.01 (less than 1% error rate).

Priority: P0
Dependencies: M7-S1, M7-S3, M7-S4, M7-S5
Open questions: none
References: PRD §8.1 launch criterion; plan §M7 exit criterion; PRD §7 NFR availability

---

## Cross-milestone dependencies summary

| Consumer | Depends on | Reason |
|---|---|---|
| M2 | M1-S4 | Phase 1 visitor extension |
| M3 | M2-S2 | Full schema required for parallel throughput measurement |
| M4 | M1-S6, M1-S5 | REPO nodes + GraphSink + Phase 3 output |
| M5 | M2-S2 | MACRO build on existing visitor framework |
| M6 | M2-S1, M2-S2 | Schema enums must be complete for prompt codegen |
| M7 | M1-S6, M3-S1 | Both sinks + parallelism needed for daemon ingest |
| M6-all | Q5 ADR (adr-1.md) | **Hard blocker — architect must resolve before M6 developer dispatch** |
| M5-S3 | Q3 ADR | Architect resolves before M5 developer dispatch |
| M4-S2 | Q4 ADR | Cross-repo schema versioning resolution |

## Open questions (all)

| # | Question | Blocking | Resolution owner |
|---|---|---|---|
| Q5 | cpp-mcp boundary — does schema prompt live here or in cpp-mcp repo? | M6 (hard blocker) | Architect — adr-1.md MUST be written before M6 dispatch |
| Q3 | C++20 modules — implement now with caveats or defer to libclang 19+? | M5-S3 | Architect — resolve in ADR before M5 dispatch |
| Q4 | Cross-repo schema versioning — refuse mismatch or reconcile? | M4-S2, M6-S3 | Architect — resolve in ADR |
| Q2 | Build config handling — one graph per config tuple or merged with tagged edges? | M3-S1, M5 | Architect — resolve in ADR; default is separate graphs |
| Q7 | Multi-tenant daemon — single-tenant for v1 accepted; ADR if scope grows | M7-S1 | Architect — ADR with explicit defer rationale acceptable |
| Q8 | Bazel compile_commands.json probing — explicitly out of scope for v1 | M1-S3 | PM decision: No. Document in README. |
| Q6 | SSH git auth — accepted as deferred to v2 | None | No action needed |

## References

- PRD v1.1: `~/workspace/wiki/pages/planning/codexgraph-cpp-prd-v1.md`
- Engineering plan v1.1: `~/workspace/wiki/pages/planning/codexgraph-cpp-libclang-rust.md`
- CHARTER: `/Users/husam/workspace/cpp-indexer/.claude/handoff/v1/CHARTER.md`
- Cognee tag: `task:cpp-indexer role:product-manager`
