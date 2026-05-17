# cpp-indexer — Scenarios (M1–M7)

run_id: cpp-indexer-v1
stage: 2 of 8 — business-analyst
version: 1.0
date: 2026-05-17
upstream: requirements.md v1.0
downstream: architect reads this file; QA executes

---

## Requirements summary

### In-scope

- All AC IDs AC-M1-1 through AC-M7-27 (115 criteria across M1–M7).
- Boundary cases mandated by dispatch: empty repo, missing compile_commands.json, malformed USR, sink failure, bearer-token auth bypass, host allowlist miss.
- Edge cases identified from AC analysis.

### Out-of-scope

- Bazel compile_commands.json probing (Q8 — explicitly excluded from v1 per PM decision).
- SSH git authentication (Q6 — deferred to v2 per CHARTER).
- C++20 modules on libclang < 18 partial-support builds (Q3 — conditional per architect ADR).
- Multi-tenant daemon behaviour (Q7 — single-tenant for v1; ADR with defer rationale acceptable).

### Assumptions

- `assumed` tags indicate behaviour inferred from locked-in decisions or context not stated in an explicit AC.
- All fixture paths are relative to `tests/fixtures/` in the project repo unless stated otherwise.
- "non-zero exit" means OS exit code ≠ 0 and at least one error line on stderr.
- Parquet shards are accessible on the local filesystem under `.cxg-cache/` unless configured otherwise.
- `cxg-daemon` is started with a valid `cxg-daemon.toml` in all daemon scenarios unless the scenario tests misconfiguration.

### Open questions (flagged — do not resolve here)

| ID | Question | Blocking | Owner |
|----|----------|----------|-------|
| Q5 | cpp-mcp boundary: does the schema prompt live in this repo or in cpp-mcp repo? | All M6 scenarios — **hard blocker before M6 developer dispatch** | Architect — adr-1.md |
| Q3 | C++20 modules: implement now (libclang 18) or defer to libclang 19+? | M5-S3 scenarios | Architect — ADR before M5 dispatch |
| Q4 | Cross-repo schema versioning: refuse mismatch or reconcile? | M4-S2, M6-S3 scenarios | Architect — ADR |
| Q2 | Build config handling: one graph per config tuple or merged with tagged edges? | M3-S1, M5 scenarios | Architect — ADR; default separate graphs |
| Q7 | Multi-tenant daemon scope for v1 | M7-S1 | Architect — ADR with defer rationale |
| OQ-BA-1 | Malformed USR (non-empty string that fails USR format): AC-M1-17 specifies `usr` must be non-empty, but no AC specifies what happens when libclang returns a syntactically malformed USR string (not empty, not null). Behaviour is unspecified. | M1-S4 edge case | needs-clarification — PM or architect |
| OQ-BA-2 | Empty repo input: behaviour when `cxg-index` receives a path to a repo directory with no C++ source files is not covered by any AC. | M1 edge cases | needs-clarification — PM |
| OQ-BA-3 | AC-M3-1 states wall time "measurably less than sequential" but does not define the measurement method or acceptable ratio. QA cannot write an automated assertion without a numeric bound or test oracle. | M3-S1 | needs-clarification — PM should add a numeric threshold or defer to a benchmark-only scenario |
| OQ-BA-4 | AC-M7-25 specifies a 7-day soak. This cannot be a standard pytest-bdd scenario. Marked as a manual/operational acceptance scenario below; QA needs a separate harness or manual checklist. | M7-S7 | needs-clarification — QA and devops must agree on harness |

### Edge cases

| Case | AC home | Status |
|------|---------|--------|
| Empty repo (no C++ source files) | None | needs-clarification (OQ-BA-2) |
| Missing compile_commands.json | AC-M1-9, AC-M1-10 | confirmed |
| compile_commands.json present but syntactically malformed JSON | AC-M1-7 | confirmed |
| compile_commands.json valid JSON but missing required fields | AC-M1-7 | assumed |
| Malformed USR string returned by libclang | None direct | needs-clarification (OQ-BA-1) |
| Sink credential env var unset | AC-M1-24 | confirmed |
| Sink unreachable mid-batch write | AC-M3-6 | confirmed |
| Bearer token missing on write endpoint | AC-M7-6 | confirmed |
| Bearer token present but invalid/expired | AC-M7-6 | assumed (same 401 response) |
| Host not in allowlist on git-URL ingest | AC-M7-14 | confirmed |
| Queue full on POST /v1/ingest | AC-M7-19 | confirmed |
| Duplicate git_url POST (repo already cloned) | AC-M7-13 | confirmed |
| Phase 5 concurrent run (advisory lock) | AC-M4-6 | confirmed |
| Cache from prior libclang/schema version | AC-M3-9 | confirmed |
| TU that causes libclang parse error | AC-M1-16 | confirmed |
| Worker panic during parallel Phase 1 | AC-M3-2 | confirmed |

### Stakeholders

| Name | Role | Stories |
|------|------|---------|
| Atlas | Primary user — runs cxg-index and queries graph | M1-S3, M1-S6, M2-S3, M3-S3, M3-S5, M4-S1, M4-S2, M5-S3 |
| Devon | DevOps / CI operator — runs daemon, resets graph, monitors | M7-S1, M7-S2, M7-S4, M7-S5, M7-S6 |
| Nina | Agent consumer — queries indexed graph via cpp-mcp | M6-S1, M6-S2, M6-S3, M6-S4 |
| Team | Integration gate owners | M1-S7, M2-S4, M4-S4, M5-S4, M6-S4, M7-S7 |

---

## Gherkin

> Convention: each scenario is tagged `@<AC-ID>` for traceability. Additional tags indicate status: `@assumed`, `@needs-clarification`, `@manual`. pytest-bdd treats these as markers.

---

### Feature: M1-S1 Crate skeleton and base schema

```gherkin
Feature: M1-S1 Crate skeleton and base schema
  As a developer
  I want a buildable Rust crate skeleton with CodexGraph node and edge types
  So that all subsequent work has a stable type-safe foundation

  @AC-M1-1
  Scenario: Fresh checkout compiles without errors
    Given a fresh checkout of the cpp-indexer repo on a Linux or macOS host
    And libclang 18 dev headers are installed and linked
    When "cargo build" is run
    Then the build exits with code 0
    And no compiler errors are emitted to stderr

  @AC-M1-1
  Scenario: Cross-platform build succeeds on macOS
    Given a fresh checkout on a macOS host with libclang 18 dev headers installed
    When "cargo build" is run
    Then the build exits with code 0

  @AC-M1-2
  Scenario: All base node types are present as typed Rust structs or enums
    Given the schema module is compiled
    When it is inspected via "cargo test"
    Then the schema module exports typed representations for each of:
      | node_type       |
      | MODULE          |
      | CLASS           |
      | FUNCTION        |
      | METHOD          |
      | FIELD           |
      | GLOBAL_VARIABLE |
    And each representation includes its required attributes

  @AC-M1-3
  Scenario: All base edge types are present
    Given the schema module is compiled
    When it is inspected via "cargo test"
    Then the schema module exports typed representations for each of:
      | edge_type  |
      | CONTAINS   |
      | HAS_METHOD |
      | HAS_FIELD  |
      | INHERITS   |
      | USES       |
      | CALLS      |

  @AC-M1-4
  Scenario: Schema enum to Arrow schema round-trip passes
    Given the schema module is compiled
    When "cargo test" is run targeting schema round-trip tests
    Then all unit tests for the enum-to-Arrow-schema conversion pass
    And the exit code is 0
```

---

### Feature: M1-S2 Phase 0 compile_commands.json parser

```gherkin
Feature: M1-S2 Phase 0 compile_commands.json parser
  As a developer
  I want Phase 0 to read, deduplicate, and queue translation units from compile_commands.json

  @AC-M1-5
  Scenario: Valid compile_commands.json is parsed and all entries are deduplicated by Blake3 hash
    Given a valid "compile_commands.json" with 10 entries covering 8 unique (file, args) pairs
    When Phase 0 runs
    Then the TU work queue contains exactly 8 entries
    And each entry corresponds to a unique Blake3 hash of (file, args)

  @AC-M1-6
  Scenario: Duplicate (file, args) entries appear exactly once in the work queue
    Given a "compile_commands.json" where the entry for "src/foo.cpp" with "-O2" appears 3 times
    When Phase 0 runs
    Then the work queue contains exactly one entry for ("src/foo.cpp", "-O2")

  @AC-M1-7
  Scenario: Malformed JSON compile_commands.json causes non-zero exit naming the file
    Given the auto-detection has resolved "compile_commands.json" to "/repo/build/compile_commands.json"
    And that file contains malformed JSON (e.g., truncated or invalid syntax)
    When Phase 0 attempts to parse it
    Then the process exits with a non-zero code
    And the error message on stderr includes the string "/repo/build/compile_commands.json"

  @AC-M1-7 @assumed
  Scenario: compile_commands.json with missing required fields causes non-zero exit
    Given the auto-detection has resolved "compile_commands.json" to a concrete file path
    And that file is valid JSON but entries are missing the required "file" or "command" fields
    When Phase 0 attempts to parse it
    Then the process exits with a non-zero code
    And an error message on stderr names the offending file path
```

---

### Feature: M1-S3 Phase 0.5 auto-detect compile_commands.json

```gherkin
Feature: M1-S3 Phase 0.5 auto-detect compile_commands.json
  As Atlas
  I can point cxg-index at a file or directory without --compile-commands
  And the indexer finds compile_commands.json automatically

  @AC-M1-8
  Scenario: compile_commands.json found in build/ via upward walk and logged at INFO
    Given a C++ file at "<repo>/src/foo.cpp"
    And "compile_commands.json" exists at "<repo>/build/compile_commands.json"
    And no "--compile-commands" flag is supplied
    When "cxg-index <repo>/src/foo.cpp" is run
    Then the indexer exits successfully
    And the log output at INFO level includes the resolved path "<repo>/build/compile_commands.json"

  @AC-M1-9
  Scenario: No compile_commands.json found — non-zero exit listing all searched directories
    Given a C++ file at "<repo>/src/bar.cpp"
    And no "compile_commands.json" exists anywhere in the upward directory walk
    When "cxg-index <repo>/src/bar.cpp" is run
    Then the process exits with a non-zero code
    And the error message on stderr lists every directory that was searched
    And no heuristic fallback is attempted

  @AC-M1-10
  Scenario: Walk stops at .git boundary and emits not-found error
    Given a C++ file at "<repo>/src/baz.cpp"
    And a ".git" directory exists at "<repo>/.git"
    And no "compile_commands.json" has been found at or below the "<repo>" level
    When "cxg-index <repo>/src/baz.cpp" is run
    Then the walk stops at the ".git" boundary
    And the process exits with a non-zero code with a "not found" error message

  @AC-M1-11
  Scenario: Multiple candidates at same level — file-entry match is preferred
    Given "compile_commands.json" candidates exist at both "<repo>/build/" and "<repo>/out/"
    And the entry for "<repo>/src/main.cpp" appears only in "<repo>/build/compile_commands.json"
    When "cxg-index <repo>/src/main.cpp" is run
    Then the indexer selects "<repo>/build/compile_commands.json"

  @AC-M1-11
  Scenario: Multiple candidates with no file-entry match — lexicographically first is chosen
    Given "compile_commands.json" candidates exist at both "<repo>/build/" and "<repo>/out/"
    And the target file does not appear in either candidate
    When "cxg-index <repo>/src/main.cpp" is run
    Then the indexer selects the candidate whose parent directory sorts lexicographically first

  @AC-M1-12
  Scenario: Directory input restricts TU work queue to files under that directory
    Given "cxg-index" is given the directory "<repo>/src/" as input
    And "compile_commands.json" lists entries for files both under "<repo>/src/" and "<repo>/test/"
    When the indexer resolves the work queue
    Then the TU work queue contains only entries whose "file" field is under "<repo>/src/" after path normalization
    And no entries from "<repo>/test/" are included

  @AC-M1-13
  Scenario: Single-file input restricts TU work queue to the matching entry only
    Given "cxg-index" is given the single file "<repo>/src/foo.cpp" as input
    And "compile_commands.json" lists entries for multiple files including "<repo>/src/foo.cpp"
    When the indexer resolves the work queue
    Then the TU work queue contains exactly one entry matching "<repo>/src/foo.cpp"

  @needs-clarification
  Scenario: Empty repo — no C++ source files present
    Given "cxg-index" is given a repo directory that contains no C++ source files
    And a "compile_commands.json" is present but empty
    When the indexer runs
    Then the behaviour is unspecified — see OQ-BA-2
```

---

### Feature: M1-S4 Phase 1 base libclang visitor and Parquet staging

```gherkin
Feature: M1-S4 Phase 1 base libclang visitor and Parquet staging
  As a developer
  I want Phase 1 to emit base nodes and edges as Parquet shards without DB writes

  @AC-M1-14
  Scenario: All base node types are written to Parquet shards for a given TU
    Given a TU "src/example.cpp" in the work queue that contains a class, function, method, field, and global variable
    When Phase 1 processes the TU
    Then a per-worker Parquet shard contains node rows for:
      | node_type       |
      | MODULE          |
      | CLASS           |
      | FUNCTION        |
      | METHOD          |
      | FIELD           |
      | GLOBAL_VARIABLE |

  @AC-M1-15
  Scenario: Phase 1 makes no writes to Neo4j or IndraDB
    Given a TU in the work queue
    And Neo4j and IndraDB connection monitoring is active
    When Phase 1 processes the TU
    Then zero write operations are observed on the Neo4j connection
    And zero write operations are observed on the IndraDB connection

  @AC-M1-16
  Scenario: libclang parse error on one TU is recorded as diagnostic and does not abort the run
    Given a work queue containing TUs "src/good.cpp" and "src/broken.cpp"
    And "src/broken.cpp" causes a libclang parse error
    When Phase 1 processes the work queue
    Then the error from "src/broken.cpp" is recorded as a diagnostic
    And Phase 1 continues and processes "src/good.cpp"
    And the overall run does not abort

  @AC-M1-17
  Scenario: Each Parquet node row contains a non-empty usr field
    Given Phase 1 has processed a TU containing at least one CLASS node
    When the output Parquet shard is read
    Then every node row has a non-empty "usr" field populated via clang_getCursorUSR

  @needs-clarification
  Scenario: libclang returns a syntactically malformed (non-empty) USR string
    Given Phase 1 processes a TU
    And libclang returns a non-empty but malformed USR string for one cursor
    When the Parquet shard is written
    Then the behaviour is unspecified — see OQ-BA-1
```

---

### Feature: M1-S5 Phase 3 trivial in-memory resolution

```gherkin
Feature: M1-S5 Phase 3 trivial in-memory resolution
  As a developer
  I want Phase 3 to resolve within-repo edges and flag cross-repo candidates

  @AC-M1-18
  Scenario: All within-repo edge targets are resolved by USR lookup
    Given Phase 1 Parquet shards from the 5-file C++ fixture
    When Phase 3 runs
    Then all edges whose target USR exists in the current repo shard set are resolved
    And a final-edges Parquet file is produced

  @AC-M1-19
  Scenario: Edges whose target USR is not found are flagged as cross_repo_candidate
    Given Phase 1 Parquet shards where at least one edge target USR is not present in the current repo
    When Phase 3 runs
    Then that edge is present in the output with "cross_repo_candidate: true"
    And the edge is not dropped
```

---

### Feature: M1-S6 Both Neo4j and IndraDB sinks behind GraphSink trait

```gherkin
Feature: M1-S6 Both Neo4j and IndraDB sinks behind GraphSink trait
  As Atlas
  I can choose --sink neo4j or --sink indradb at runtime without rebuilding

  @AC-M1-20
  Scenario: --sink neo4j writes nodes and edges to Neo4j
    Given a compiled "cxg-index" binary
    And valid Neo4j credentials are available via the configured env var
    When "cxg-index --sink neo4j <fixture>" is run
    Then Phase 4 writes all nodes and edges from the fixture to Neo4j
    And the write uses a single transaction (batching not yet applied at M1)

  @AC-M1-21
  Scenario: --sink indradb writes nodes and edges to IndraDB
    Given a compiled "cxg-index" binary
    And a valid IndraDB endpoint is available
    When "cxg-index --sink indradb <fixture>" is run
    Then Phase 4 writes all nodes and edges from the fixture to IndraDB

  @AC-M1-22
  Scenario: cargo test exercises both sinks via the same integration fixture
    Given the GraphSink trait is implemented by both Neo4jSink and IndraDbSink
    When "cargo test" is run
    Then the integration fixture test runs against Neo4jSink
    And the same integration fixture test runs against IndraDbSink
    And both pass

  @AC-M1-23
  Scenario: Sink credentials are read from the env var named in config — not from the config file
    Given "cxg-index.toml" specifies "password_env = NEO4J_PASSWORD" for Neo4j
    And the env var "NEO4J_PASSWORD" is set to a valid credential
    When "cxg-index" starts
    Then the credential is read from the env var at runtime
    And no secret value appears in the config file or in the log output

  @AC-M1-24
  Scenario: Missing sink credential env var causes non-zero exit before any indexing
    Given "cxg-index.toml" specifies "password_env = NEO4J_PASSWORD"
    And the env var "NEO4J_PASSWORD" is not set
    When "cxg-index" starts
    Then the process exits with a non-zero code before any indexing begins
    And the error message on stderr identifies "NEO4J_PASSWORD" as the missing variable

  @AC-M1-24 @assumed
  Scenario: Invalid sink credential env var causes non-zero exit before any indexing
    Given the sink credential env var is set to an incorrect value
    When "cxg-index" starts
    Then the process exits with a non-zero code before any indexing begins
    And the error message identifies which credential is wrong
```

---

### Feature: M1-S7 M1 exit gate — isomorphic graph fixture test

```gherkin
Feature: M1-S7 M1 exit gate isomorphic graph fixture test
  As the team
  We need cxg-index to produce an isomorphic graph vs the Python CodexGraph reference

  @AC-M1-25
  Scenario: Neo4j sink matches golden-graph snapshot on 5-file fixture
    Given the 5-file C++ fixture in "tests/fixtures/"
    And a golden-graph snapshot generated from the Python CodexGraph reference
    When "cxg-index" runs against the fixture with "--sink neo4j"
    Then "cargo test" asserts that the node count matches the snapshot
    And the edge count matches the snapshot
    And all USR-keyed nodes in the snapshot subset are present in Neo4j

  @AC-M1-26
  Scenario: IndraDB sink produces a graph isomorphic to the Neo4j result
    Given the same 5-file fixture and golden-graph snapshot
    When "cxg-index" runs with "--sink indradb"
    Then the same node count, edge count, and USR-keyed node assertions pass
    And the result is isomorphic to the Neo4j result modulo internal node IDs

  @AC-M1-27
  Scenario: Test failure output identifies missing or extra nodes and edges
    Given the golden-graph snapshot and a run that produces a divergent graph
    When the fixture test fails
    Then the test output includes a diff identifying which nodes and edges are missing or extra
```

---

### Feature: M2-S1 C++ extension node types

```gherkin
Feature: M2-S1 C++ extension node types
  As a developer
  I want Phase 1 extended to emit NAMESPACE, TEMPLATE_DECL, SPECIALIZATION, TYPEDEF, ENUM, and HEADER nodes

  @AC-M2-1
  Scenario: NAMESPACE node emitted for each unique named namespace
    Given a C++ file containing "namespace myns { }" and "namespace myns { }" in another header
    When Phase 1 processes the file
    Then exactly one NAMESPACE node with qualified name "myns" and a non-empty USR is emitted

  @AC-M2-2
  Scenario: TEMPLATE_DECL node emitted for a class or function template
    Given a C++ file containing "template<typename T> class Vec { };"
    When Phase 1 processes the file
    Then a TEMPLATE_DECL node is emitted with "kind", "name", and "params" populated

  @AC-M2-3
  Scenario: SPECIALIZATION node emitted for a template specialization
    Given a C++ file containing a full or partial template specialization of a known template
    When Phase 1 processes the file
    Then a SPECIALIZATION node is emitted with "template_usr" matching the TEMPLATE_DECL USR
    And the "template_args" field is populated

  @AC-M2-4
  Scenario: HEADER node emitted for each included header file
    Given a C++ file with "#include <vector>" and "#include \"myheader.h\""
    When Phase 1 processes the file
    Then a HEADER node is emitted for "vector"
    And a HEADER node is emitted for "myheader.h"

  @AC-M2-5
  Scenario: TYPEDEF node emitted for typedef and using aliases
    Given a C++ file containing "typedef int MyInt;" and "using Size = std::size_t;"
    When Phase 1 processes the file
    Then a TYPEDEF node is emitted for "MyInt" with "underlying_type_usr" pointing to int
    And a TYPEDEF node is emitted for "Size" with "underlying_type_usr" pointing to std::size_t

  @AC-M2-6
  Scenario: ENUM node emitted for enum and enum class with scoped boolean
    Given a C++ file containing "enum Color { Red };" and "enum class Direction { North };"
    When Phase 1 processes the file
    Then a node of type ENUM is emitted for "Color" with "scoped: false"
    And a node of type ENUM is emitted for "Direction" with "scoped: true"
```

---

### Feature: M2-S2 C++ extension edge types

```gherkin
Feature: M2-S2 C++ extension edge types
  As a developer
  I want Phase 1 to emit INCLUDES, OVERRIDES, INSTANTIATES, SPECIALIZES, FRIEND_OF, and ADL_CANDIDATE edges

  @AC-M2-7
  Scenario: INCLUDES edge emitted from module to included HEADER node
    Given a C++ module file with "#include \"utils.h\""
    When Phase 1 processes the file
    Then an INCLUDES edge from the MODULE node to the "utils.h" HEADER node is emitted

  @AC-M2-8
  Scenario: OVERRIDES edge emitted with vtable_slot for a virtual override
    Given a C++ file with a virtual method "virtual void foo()" in base class B
    And a derived class D overriding "foo"
    When Phase 1 processes the file
    Then an OVERRIDES edge from D::foo to B::foo is emitted
    And the edge contains a "vtable_slot" attribute derived from clang_getOverriddenCursors

  @AC-M2-9
  Scenario: INSTANTIATES edge emitted from call site to TEMPLATE_DECL
    Given a C++ file containing "std::vector<int> v;" which instantiates the vector template
    When Phase 1 processes the file
    Then an INSTANTIATES edge is emitted from the call site to the TEMPLATE_DECL for vector

  @AC-M2-10
  Scenario: SPECIALIZES edge emitted from SPECIALIZATION to TEMPLATE_DECL
    Given a C++ file with a partial specialization of template class T
    When Phase 1 processes the file
    Then a SPECIALIZES edge is emitted from the SPECIALIZATION node to the TEMPLATE_DECL node

  @AC-M2-11
  Scenario: FRIEND_OF edge emitted for a friend declaration
    Given a C++ file with "class A { friend class B; };"
    When Phase 1 processes the file
    Then a FRIEND_OF edge is emitted from B to A

  @AC-M2-12
  Scenario: ADL_CANDIDATE edge emitted for unresolved ADL-eligible reference
    Given a C++ file with an unqualified function call that could be resolved by ADL
    When Phase 1 processes the file
    Then an ADL_CANDIDATE edge is emitted for that unresolved reference
```

---

### Feature: M2-S3 Unresolved references and system header filtering

```gherkin
Feature: M2-S3 Unresolved references and system header filtering
  As Atlas
  I want unresolved references kept as partial nodes and system headers filtered by config

  @AC-M2-13
  Scenario: Edge with unresolved target USR is emitted with resolved false — not dropped
    Given Phase 1 Parquet shards where one edge target USR is not found in the repo
    When Phase 3 runs
    Then the edge is present in the output Parquet
    And the "resolved" field on that edge row is false

  @AC-M2-14
  Scenario: System header nodes excluded when skip_system_headers is true
    Given a TU that includes "/usr/include/stdio.h"
    And the config has "skip_system_headers: true"
    When Phase 1 runs
    Then no nodes or edges originating from "/usr/include/stdio.h" appear in Parquet output

  @AC-M2-15
  Scenario: System header nodes included when skip_system_headers is false
    Given a TU that includes "/usr/include/stdio.h"
    And the config has "skip_system_headers: false"
    When Phase 1 runs
    Then nodes and edges from "/usr/include/stdio.h" are included in Parquet output
```

---

### Feature: M2-S4 M2 exit gate — Boost.Optional fixture

```gherkin
Feature: M2-S4 M2 exit gate Boost.Optional fixture
  As the team
  We need Boost.Optional to index completely with all within-repo references resolved

  @AC-M2-16
  Scenario: Zero cross_repo_candidate edges after indexing Boost.Optional
    Given the Boost.Optional single-header source is present in "tests/fixtures/boost-optional/"
    When "cxg-index" runs against the fixture
    Then "cargo test" asserts that the Parquet output contains zero edges with "cross_repo_candidate: true"

  @AC-M2-17
  Scenario: Template specialization nodes and SPECIALIZES edges present for boost::optional<T>
    Given the same Boost.Optional fixture and indexed graph
    When the graph is queried
    Then at least one SPECIALIZATION node for "boost::optional<T>" is present
    And at least one SPECIALIZES edge from that SPECIALIZATION to the "boost::optional" TEMPLATE_DECL is present
```

---

### Feature: M3-S1 Parallel Phase 1 with rayon and thread-local clang Index

```gherkin
Feature: M3-S1 Parallel Phase 1 with rayon and thread-local clang Index
  As Atlas
  I want Phase 1 to process TUs in parallel across all available CPU cores

  @AC-M3-1 @needs-clarification
  Scenario: Parallel processing is measurably faster than sequential on a multi-core host
    Given a work queue of N TUs
    And the host has W cores (W > 1)
    When Phase 1 runs in parallel mode
    Then wall time is measurably less than sequential processing of the same N TUs
    # Note: "measurably less" lacks a numeric definition — see OQ-BA-3

  @AC-M3-2
  Scenario: Worker-local clang Index panic on one TU does not stop other workers
    Given a work queue containing a TU that causes a worker panic during Phase 1
    When Phase 1 runs with rayon parallelism
    Then the failed TU is recorded as a diagnostic
    And remaining TUs in the queue are processed by other workers
    And Phase 1 exits without propagating the panic

  @AC-M3-3
  Scenario: Phase 1 completes LLVM fixture (~25k TUs) within 15 minutes on 32 cores
    Given the LLVM source checkout with approximately 25000 TUs
    And the host has 32 cores
    When Phase 1 runs with 32 workers
    Then wall time for Phase 1 alone is at most 15 minutes
```

---

### Feature: M3-S2 Batched Phase 4 writes — Neo4j and IndraDB

```gherkin
Feature: M3-S2 Batched Phase 4 writes Neo4j and IndraDB
  As a developer
  I want Phase 4 to write in batches meeting throughput targets

  @AC-M3-4
  Scenario: Neo4j write throughput meets 50k rows per second on LLVM fixture
    Given default config with "batch_size=10000" and "sessions=16"
    When Phase 4 writes to Neo4j on the LLVM fixture
    Then the measured throughput is at least 50000 rows per second

  @AC-M3-5
  Scenario: IndraDB write throughput meets 100k rows per second on LLVM fixture
    Given default config with "batch_size=10000" and "sessions=16"
    When Phase 4 writes to IndraDB on the LLVM fixture
    Then the measured throughput is at least 100000 rows per second

  @AC-M3-6
  Scenario: Write failure mid-batch retries without producing duplicate nodes
    Given Phase 4 is writing to Neo4j and a transient network failure occurs after the first batch
    When Phase 4 retries the failed batch
    Then no duplicate nodes appear in Neo4j (write is idempotent via USR-keyed MERGE or equivalent)

  @AC-M3-6 @assumed
  Scenario: Write failure mid-batch to IndraDB retries without producing duplicate nodes
    Given Phase 4 is writing to IndraDB and a transient failure occurs
    When Phase 4 retries
    Then no duplicate nodes appear (idempotent write)
```

---

### Feature: M3-S3 Content-hash cache for incremental re-indexing

```gherkin
Feature: M3-S3 Content-hash cache for incremental re-indexing
  As Atlas
  I can re-run cxg-index after editing one file and have only changed TUs re-indexed

  @AC-M3-7
  Scenario: No-change re-run processes zero TUs and exits within 30 seconds
    Given a completed index run with all TUs cached
    And no source files have been modified
    When "cxg-index" is run again
    Then zero TUs are processed by Phase 1
    And the run exits in under 30 seconds

  @AC-M3-8
  Scenario: One changed source file causes only that TU to be re-processed
    Given a completed index run
    And one source file has been modified (changing its source_hash)
    When "cxg-index" re-runs
    Then only the TU(s) whose "(source_hash, args_hash)" has changed are processed by Phase 1
    And all other TUs are cache hits

  @AC-M3-9
  Scenario: Cache from a prior libclang or schema version is invalidated
    Given an existing cache directory from a run with a different libclang version
    When "cxg-index" starts
    Then the cache is invalidated
    And all TUs are re-processed by Phase 1

  @AC-M3-9 @assumed
  Scenario: Cache from a prior schema version is invalidated
    Given an existing cache directory from a run with a different schema version
    When "cxg-index" starts
    Then the cache is invalidated
    And all TUs are re-processed by Phase 1

  @AC-M3-10
  Scenario: Incremental re-index on LLVM fixture (one file changed) completes within 1 minute
    Given a completed LLVM index run
    And one source file in the LLVM fixture has been modified
    When "cxg-index" re-runs
    Then the total end-to-end wall time is at most 60 seconds
```

---

### Feature: M3-S4 Memory budget enforcement

```gherkin
Feature: M3-S4 Memory budget enforcement
  As a developer
  I want Phase 1 peak memory capped at 16 GB and Phase 3 USR map spillable to RocksDB

  @AC-M3-11
  Scenario: Phase 1 peak RSS is at most 16 GB on LLVM fixture with 32 cores
    Given the LLVM fixture on a 32-core machine
    When Phase 1 completes
    Then peak RSS is at most 16384 MB as measured by the OS

  @AC-M3-12
  Scenario: Phase 3 USR map exceeding 8 GB spills to RocksDB without OOM
    Given a Phase 3 run where the in-memory USR map exceeds 8 GB
    When Phase 3 runs
    Then excess entries are spilled to a RocksDB store under ".cxg-cache/"
    And the run completes without an out-of-memory error
```

---

### Feature: M3-S5 Progress reporting

```gherkin
Feature: M3-S5 Progress reporting
  As Atlas
  I can see real-time progress on stderr while indexing

  @AC-M3-13
  Scenario: stderr shows TUs done / total, nodes/sec, edges/sec every 5 seconds during Phase 1
    Given an active indexing run processing multiple TUs in Phase 1
    When at least 5 seconds have elapsed
    Then stderr contains a progress line showing:
      | field            |
      | TUs done / total |
      | nodes per second |
      | edges per second |
    And that line has been updated at least once in the last 5 seconds

  @AC-M3-14
  Scenario: Cache-hit TU is counted as immediately done in the progress counter
    Given an active indexing run where some TUs are cache hits
    When Phase 1 skips a cache-hit TU
    Then the progress counter increments the "done" count for that TU immediately
```

---

### Feature: M4-S1 REPO nodes and BELONGS_TO_REPO edges

```gherkin
Feature: M4-S1 REPO nodes and BELONGS_TO_REPO edges
  As Atlas
  I can filter graph queries by repo using BELONGS_TO_REPO edges

  @AC-M4-1
  Scenario: REPO node is created with required attributes
    Given a repo indexed with "cxg-index"
    When the graph is written to the sink
    Then a REPO node exists with the following non-empty attributes:
      | attribute   |
      | name        |
      | root_path   |
      | commit_sha  |
      | commit_date |

  @AC-M4-2
  Scenario: Every emitted node has a BELONGS_TO_REPO edge to its REPO node
    Given a repo indexed with "cxg-index"
    When the graph is written
    Then every node emitted by the indexer has exactly one BELONGS_TO_REPO edge to the repo's REPO node

  @AC-M4-3
  Scenario: cxg-resolve-cross-repo refuses heterogeneous sink setup with a clear error
    Given a REPO node that records "sink: neo4j"
    And a second REPO node that records "sink: indradb"
    When "cxg-resolve-cross-repo" is run
    Then it exits with a non-zero code
    And the error message identifies the heterogeneous sink configuration
```

---

### Feature: M4-S2 cross_repo_candidate flag and Phase 5 binary

```gherkin
Feature: M4-S2 cross_repo_candidate flag and Phase 5 binary
  As Atlas
  I can run cxg-resolve-cross-repo to materialize EXTERNAL_REF edges between repos

  @AC-M4-4
  Scenario: cross_repo_candidate edges whose target USR exists in another repo become EXTERNAL_REF edges
    Given two repos indexed into the same Neo4j database
    And at least one edge in repo-A has "cross_repo_candidate: true" with a target USR present in repo-B
    When "cxg-resolve-cross-repo --config <toml>" is run
    Then all such candidate edges are materialized as EXTERNAL_REF edges with a "via:<original_edge_type>" attribute

  @AC-M4-5
  Scenario: cross_repo_candidate edge with no matching target USR remains with resolved false
    Given a "cross_repo_candidate: true" edge whose target USR is not found in any indexed repo
    When Phase 5 runs
    Then the edge remains in the graph with "resolved: false"
    And the unresolved edge is logged as a diagnostic

  @AC-M4-6
  Scenario: Concurrent Phase 5 runs serialize via advisory lock — no race condition
    Given two "cxg-resolve-cross-repo" processes starting simultaneously against the same DB
    When both attempt to write EXTERNAL_REF edges
    Then the second process waits for the advisory lock held by the first
    And no duplicate or inconsistent EXTERNAL_REF edges result

  @AC-M4-4 @needs-clarification
  Scenario: Schema version mismatch between repos causes Phase 5 to refuse
    Given two repos indexed with different schema versions
    When "cxg-resolve-cross-repo" is run
    Then it exits with an error identifying the schema version conflict
    # Note: Exact behaviour depends on Q4 ADR resolution — needs-clarification
```

---

### Feature: M4-S3 System-header canonicalization

```gherkin
Feature: M4-S3 System-header canonicalization
  As Atlas
  I want system-header USRs resolved to canonical system REPO nodes

  @AC-M4-7
  Scenario: USR resolving to /usr/include is pinned to system:libc or system:libstdc++ REPO node
    Given a USR that maps to a path under "/usr/include/"
    When Phase 5 canonicalises it
    Then the target is the "system:libstdc++" or "system:libc" REPO node
    And the assignment is independent of which user repo was indexed first

  @AC-M4-8
  Scenario: Vendored third-party header is assigned a repo:vendored:<pkg> REPO node
    Given a header at "third_party/zlib/zlib.h" referenced by the indexed repo
    When Phase 5 runs
    Then the header's REPO node is "repo:vendored:zlib"
    And it is distinct from both the user repo REPO node and any upstream "zlib" REPO node
```

---

### Feature: M4-S4 M4 exit gate — two-repo EXTERNAL_REF demo

```gherkin
Feature: M4-S4 M4 exit gate two-repo EXTERNAL_REF demo
  As the team
  We need EXTERNAL_REF edges to materialize correctly on a two-repo fixture

  @AC-M4-9
  Scenario: Cypher query finds at least one EXTERNAL_REF path after cross-repo resolution
    Given fixtures "lib-a" and "lib-b" where lib-a calls a function in lib-b
    And both repos are indexed into the same Neo4j database
    When "cxg-resolve-cross-repo" runs
    Then the Cypher query "MATCH p=()-[:EXTERNAL_REF]->() RETURN p LIMIT 1" returns at least one path

  @AC-M4-10
  Scenario: EXTERNAL_REF edge via attribute equals CALLS
    Given the two-repo fixture with lib-a calling into lib-b
    When the EXTERNAL_REF edge is queried
    Then its "via" attribute equals "CALLS"
```

---

### Feature: M5-S1 MACRO nodes and EXPANDS_TO edges

```gherkin
Feature: M5-S1 MACRO nodes and EXPANDS_TO edges
  As a developer
  I want MACRO nodes and EXPANDS_TO edges emitted for top-level macro expansions

  @AC-M5-1
  Scenario: MACRO node emitted for each macro definition (object-like and function-like)
    Given a C++ file defining "#define MAX(a,b) ((a)>(b)?(a):(b))" and "#define VERSION 42"
    When Phase 1 processes the file
    Then a MACRO node is emitted for "MAX" with "kind: function-like", "name: MAX", "file_path", and "params" populated
    And a MACRO node is emitted for "VERSION" with "kind: object-like"

  @AC-M5-2
  Scenario: EXPANDS_TO edge emitted from call-site to MACRO node
    Given a C++ file with an invocation of "MAX(x, y)" at function scope
    When Phase 1 processes the file
    Then an EXPANDS_TO edge is emitted from the call-site node to the "MAX" MACRO node

  @AC-M5-3
  Scenario: Only top-level macro expansions produce EXPANDS_TO edges — nested ones are not individually emitted
    Given a C++ file with a deeply nested macro expansion where OUTER expands to INNER
    When Phase 1 processes the file
    Then an EXPANDS_TO edge is emitted for the OUTER macro invocation
    And no EXPANDS_TO edges are emitted for the nested INNER macro expansion within OUTER

  @AC-M5-4
  Scenario: EXPANDS_TO edge count is bounded to at most 10x source line count on LLVM .def fixtures
    Given an LLVM ".def" file fixture with L source lines
    When Phase 1 processes the file
    Then the total EXPANDS_TO edge count is at most 10 * L
```

---

### Feature: M5-S2 Phase 2 optional deep decoration

```gherkin
Feature: M5-S2 Phase 2 optional deep decoration
  As a developer
  I want Phase 2 available as opt-in and skippable with --skip-phase2

  @AC-M5-5
  Scenario: Phase 2 runs by default and decorates Parquet with control-flow and exception-spec annotations
    Given "--skip-phase2" is NOT passed to the indexer
    When the indexer runs
    Then Phase 2 decorates TU ASTs with control-flow and exception-spec annotations in the Parquet output

  @AC-M5-6
  Scenario: --skip-phase2 skips Phase 2 entirely with wall time no worse than M3 baseline
    Given "--skip-phase2" is passed to the indexer
    When the indexer runs
    Then Phase 2 is entirely skipped
    And wall time is no greater than the M3 performance baseline
```

---

### Feature: M5-S3 C++20 modules support (conditional)

```gherkin
Feature: M5-S3 C++20 modules support (conditional)
  As Atlas
  I want C++20 module imports indexed when libclang 18 supports them

  @AC-M5-7 @needs-clarification
  Scenario: .cppm file is indexed when libclang 18 C++20 modules are available
    Given a build environment where libclang 18 supports C++20 module interfaces
    And a ".cppm" file is in the TU work queue
    When Phase 1 processes the file
    Then nodes and edges for exported declarations are emitted
    # Note: conditional on Q3 ADR — needs-clarification

  @AC-M5-8 @needs-clarification
  Scenario: .cppm file is skipped with a warning when libclang 18 lacks C++20 module support
    Given a build environment where libclang 18 does NOT support C++20 modules
    And a ".cppm" file is in the TU work queue
    When Phase 1 encounters the file
    Then a warning is logged
    And the TU is skipped
    And the indexer continues without aborting
    # Note: conditional on Q3 ADR — needs-clarification

  @AC-M5-9
  Scenario: cxg-index --version notes C++20 module limitation when unavailable
    Given a runtime environment where C++20 module support is unavailable
    When "cxg-index --version" is run
    Then the output includes a note indicating the C++20 module limitation
```

---

### Feature: M5-S4 Chromium fixture exit gate

```gherkin
Feature: M5-S4 Chromium fixture exit gate
  As the team
  We need Chromium base/ + net/ to index without segfault and produce valid EXPANDS_TO edges

  @AC-M5-10
  Scenario: Chromium base/ and net/ subtree indexes without segfault and exits zero
    Given the Chromium "base/" and "net/" subtree as a fixture
    When "cxg-index" runs against it
    Then the indexer completes without a segfault
    And the exit code is 0

  @AC-M5-11
  Scenario: At least one MACRO node and one EXPANDS_TO edge present in Chromium fixture output
    Given the same Chromium run
    When the Parquet output is inspected
    Then at least one MACRO node is present
    And at least one EXPANDS_TO edge is present
```

---

### Feature: M6-S1 Build-time schema prompt regeneration

```gherkin
Feature: M6-S1 Build-time schema prompt regeneration
  As Nina (the agent)
  I want the schema prompt regenerated from Rust schema enums at build time
  Note: ALL M6 scenarios are blocked on Q5 ADR (adr-1.md). Do not dispatch M6 stories until Q5 is resolved.

  @AC-M6-1 @needs-clarification
  Scenario: cargo build regenerates schema.txt from the Rust schema enums
    Given the schema enums exist in "src/schema/"
    When "cargo build" or a dedicated build script runs
    Then "prompt/graph_database/cpp/schema.txt" is updated to reflect all current node and edge types
    # Note: blocked on Q5 ADR — file location may change

  @AC-M6-2 @needs-clarification
  Scenario: Adding a new node type is reflected in schema.txt without manual intervention
    Given a new node type has been added to the schema enums in "src/schema/"
    When "cargo build" runs
    Then "prompt/graph_database/cpp/schema.txt" includes the new node type

  @AC-M6-3 @needs-clarification
  Scenario: CI fails when schema.txt is stale
    Given "prompt/graph_database/cpp/schema.txt" differs from what "cargo build" would generate
    When CI runs
    Then the CI step fails
    And the failure message instructs the developer to regenerate schema.txt
```

---

### Feature: M6-S2 C++ idiom examples file

```gherkin
Feature: M6-S2 C++ idiom examples file
  As Nina
  I want example.txt containing representative C++ query idioms

  @AC-M6-4 @needs-clarification
  Scenario: example.txt contains at least one example for each required idiom category
    Given "prompt/graph_database/cpp/example.txt" is read
    Then it contains at least one example for each of:
      | idiom_category               |
      | template instantiation query |
      | method override traversal    |
      | namespace-filtered lookup    |
      | #include edge traversal      |
    # Note: file location blocked on Q5 ADR

  @AC-M6-5 @needs-clarification
  Scenario: Agent produces valid Cypher for at least 3 of 4 idiom categories using the example file
    Given the example file is supplied as a translator prompt supplement to the CodexGraph agent
    When the agent is asked to produce Cypher queries for each of the 4 idiom categories
    Then at least 3 of the 4 queries are syntactically valid Cypher
    # Note: blocked on Q5 ADR; requires live agent integration
```

---

### Feature: M6-S3 Schema version field for drift detection

```gherkin
Feature: M6-S3 Schema version field for drift detection
  As Devon
  I want a schema_version field in the graph DB for drift detection

  @AC-M6-6 @needs-clarification
  Scenario: SchemaVersion node is present in the graph after indexing
    Given a graph written by "cxg-index"
    When the Neo4j or IndraDB database is queried
    Then a SchemaVersion node (or equivalent metadata entry) is present
    And it contains the current schema version string
    # Note: blocked on Q5 ADR and Q4 ADR

  @AC-M6-7 @needs-clarification
  Scenario: cpp-mcp returns an error on schema version mismatch
    Given a graph indexed with schema version "1.0"
    And cpp-mcp is running against schema version "2.0"
    When cpp-mcp attempts a query
    Then it returns an error identifying the version conflict
    And it does not return query results that may be incorrect
    # Note: blocked on Q5 ADR
```

---

### Feature: M6-S4 M6 exit gate — Streamlit agent inheritance query

```gherkin
Feature: M6-S4 M6 exit gate Streamlit agent inheritance query
  As the team
  We need the CodexGraph Streamlit agent to answer inheritance queries correctly

  @AC-M6-8 @needs-clarification
  Scenario: Agent correctly answers "what classes inherit from Foo?" against LLVM graph
    Given an LLVM-indexed graph in Neo4j
    And the CodexGraph Streamlit agent configured with the cpp schema prompt
    When the query "what classes inherit from Foo?" is issued
    Then the agent returns a non-empty answer
    And the answer references at least one real LLVM class hierarchy
    # Note: blocked on Q5 ADR; requires live Streamlit agent

  @AC-M6-9 @needs-clarification
  Scenario: Agent answers at least 8 of 10 hand-written NL questions correctly
    Given 10 hand-written NL questions about the LLVM graph
    When the agent answers all 10
    Then at least 8 answers are graded correct by manual review
    # Note: blocked on Q5 ADR; manual grading required — see OQ-BA-4 analog
```

---

### Feature: M7-S1 cxg-daemon binary and REST control plane

```gherkin
Feature: M7-S1 cxg-daemon binary and REST control plane
  As Devon
  I can start cxg-daemon and use its REST API to trigger, monitor, and reset jobs

  @AC-M7-1
  Scenario: POST /v1/ingest with a local path returns 202 Accepted and job_id within 50ms p99
    Given a running "cxg-daemon"
    When "POST /v1/ingest" is called with a valid local path body and a valid bearer token
    Then the response status is 202 Accepted
    And the response body contains a "job_id"
    And the response is received within 50 ms at p99

  @AC-M7-2
  Scenario: GET /v1/jobs/{id} returns job state, phase, and progress fields within 20ms p99
    Given a "job_id" obtained from a previous POST /v1/ingest
    When "GET /v1/jobs/{id}" is called
    Then the response status is 200
    And the response body contains "state" (one of queued|running|done|failed)
    And the response body contains "phase" (0 through 5)
    And the response body contains "tus_done", "tus_total", "nodes", "edges"
    And the response is received within 20 ms at p99

  @AC-M7-3
  Scenario: GET /v1/jobs?state=done&limit=10 returns 10 most recent completed jobs newest first
    Given a running "cxg-daemon" with more than 10 completed jobs
    When "GET /v1/jobs?state=done&limit=10" is called
    Then the response contains at most 10 jobs
    And all returned jobs have "state: done"
    And they are ordered most-recent first

  @AC-M7-4
  Scenario: GET /v1/status returns daemon uptime, queue depth, worker count, and sink connectivity
    Given a running "cxg-daemon"
    When "GET /v1/status" is called
    Then the response status is 200
    And the response body contains:
      | field              |
      | uptime             |
      | queue_depth        |
      | active_worker_count|
      | neo4j_status       |
      | indradb_status     |

  @AC-M7-5
  Scenario: GET /v1/repos returns tracked repos with required attributes
    Given a running "cxg-daemon" with at least one indexed repo
    When "GET /v1/repos" is called
    Then the response contains at least one repo entry
    And each entry includes:
      | field            |
      | name             |
      | root_path        |
      | commit_sha       |
      | last_indexed_at  |
      | node_count       |
      | edge_count       |
      | sink             |

  @AC-M7-6
  Scenario: POST /v1/ingest without Authorization header returns 401 Unauthorized
    Given a running "cxg-daemon"
    When "POST /v1/ingest" is called without an "Authorization: Bearer <token>" header
    Then the response status is 401 Unauthorized
    And no indexing job is started

  @AC-M7-6
  Scenario: POST /v1/reset without Authorization header returns 401 Unauthorized
    Given a running "cxg-daemon"
    When "POST /v1/reset" is called without a valid bearer token
    Then the response status is 401 Unauthorized

  @AC-M7-6 @assumed
  Scenario: POST /v1/ingest with an invalid or expired bearer token returns 401 Unauthorized
    Given a running "cxg-daemon"
    When "POST /v1/ingest" is called with an expired or malformed "Authorization: Bearer <bad-token>"
    Then the response status is 401 Unauthorized

  @AC-M7-7
  Scenario: Error responses conform to RFC-7807 application/problem+json
    Given any REST endpoint that encounters an error condition
    When it returns an error response
    Then the Content-Type header is "application/problem+json"
    And the response body conforms to RFC-7807 structure

  @AC-M7-8
  Scenario: Daemon binds only to the address specified in cxg-daemon.toml
    Given "cxg-daemon.toml" with "[api].listen = \"127.0.0.1:7878\""
    When "cxg-daemon" starts
    Then it listens only on "127.0.0.1:7878"
    And it does not bind to "0.0.0.0:7878" or any other interface
```

---

### Feature: M7-S2 POST /v1/reset with confirmation token

```gherkin
Feature: M7-S2 POST /v1/reset with confirmation token
  As Devon
  I can reset the graph DB for a repo or all repos with a confirmation token

  @AC-M7-9
  Scenario: POST /v1/reset with correct confirmation token deletes repo graph data and staging cache
    Given a running "cxg-daemon" with indexed repo "hermes-agent"
    And a valid bearer token
    When "POST /v1/reset" is called with body:
      | target        | repo         |
      | repo_name     | hermes-agent |
      | confirm_token | sha256("hermes-agent") |
    Then all graph nodes and edges tagged BELONGS_TO_REPO → "hermes-agent" are deleted
    And the staging cache for "hermes-agent" is cleared
    And the response status is 200

  @AC-M7-10
  Scenario: POST /v1/reset with incorrect confirm_token returns 400 and deletes nothing
    Given a running "cxg-daemon" with indexed repo data
    When "POST /v1/reset" is called with an incorrect "confirm_token"
    Then the response status is 400 Bad Request
    And no graph data is deleted

  @AC-M7-10
  Scenario: POST /v1/reset with missing confirm_token returns 400 and deletes nothing
    Given a running "cxg-daemon"
    When "POST /v1/reset" is called with no "confirm_token" field
    Then the response status is 400 Bad Request
    And no graph data is deleted

  @AC-M7-11
  Scenario: POST /v1/reset with target all and correct token wipes all repos
    Given a running "cxg-daemon" with two indexed repos
    When "POST /v1/reset" is called with "target: all" and the correct confirm token
    Then all repos' graph data and staging caches are wiped
    And the response status is 200
```

---

### Feature: M7-S3 Git-URL ingestion via workspace clone manager

```gherkin
Feature: M7-S3 Git-URL ingestion via workspace clone manager
  As Atlas
  I can POST /v1/ingest with a git HTTPS URL and have the daemon clone and index the repo

  @AC-M7-12
  Scenario: git_url on allowed_hosts list triggers clone into workspace dir and indexing
    Given a running "cxg-daemon"
    And "github.com" is in the "[workspace].allowed_hosts" list
    When "POST /v1/ingest" is called with body:
      | source.git_url | https://github.com/llvm/llvm-project |
      | source.ref     | main                                  |
    And the request includes a valid bearer token
    Then the daemon clones the repo under "[workspace].dir/llvm-project-<short-sha>/"
    And starts indexing

  @AC-M7-13
  Scenario: Posting the same git_url a second time uses git fetch instead of a fresh clone
    Given a prior clone of "https://github.com/llvm/llvm-project" exists in the workspace
    When "POST /v1/ingest" is called again with the same "git_url"
    Then "git fetch" is used to update the existing clone
    And no fresh clone is performed

  @AC-M7-14
  Scenario: git_url whose host is not in allowed_hosts returns 403 Forbidden without cloning
    Given "example.com" is NOT in the "[workspace].allowed_hosts" list
    When "POST /v1/ingest" is called with "git_url: https://example.com/repo.git" and a valid bearer token
    Then the response status is 403 Forbidden
    And no clone operation is performed

  @AC-M7-15
  Scenario: Credentials for HTTPS clone come from env var — no PAT in logs or responses
    Given "[workspace].git_credentials_env = GIT_PAT"
    And the env var "GIT_PAT" is set to a valid personal access token
    When the daemon clones a HTTPS repo
    Then the PAT value does not appear in any log line
    And the PAT value does not appear in any API response body

  @AC-M7-16
  Scenario: default_clone_depth=1 causes the clone to use --depth=1 (shallow clone)
    Given "default_clone_depth = 1" in config
    When the daemon clones a git repo
    Then git is invoked with "--depth=1"
    And the resulting clone is a shallow clone
```

---

### Feature: M7-S4 Prometheus metrics endpoint

```gherkin
Feature: M7-S4 Prometheus metrics endpoint
  As Devon
  I can scrape GET /metrics for Prometheus

  @AC-M7-17
  Scenario: GET /metrics returns Prometheus text format with all required metrics
    Given a running "cxg-daemon"
    When "GET /metrics" is scraped
    Then the response status is 200
    And the Content-Type indicates Prometheus text format
    And the response body includes all of:
      | metric_name                |
      | cxg_nodes_total            |
      | cxg_edges_total            |
      | cxg_nodes_per_second       |
      | cxg_edges_per_second       |
      | cxg_cache_hit_ratio        |
      | cxg_libclang_errors_total  |
      | cxg_queue_depth            |

  @AC-M7-18
  Scenario: GET /metrics returns 200 without an Authorization header
    Given a running "cxg-daemon"
    When "GET /metrics" is called without any Authorization header
    Then the response status is 200
    And metrics data is returned

  @AC-M7-19
  Scenario: POST /v1/ingest returns 429 when queue is full and cxg_queue_depth is visible
    Given "cxg-daemon" with "job_queue_max = 5" and 5 jobs already queued
    When "POST /v1/ingest" is called with a valid bearer token
    Then the response status is 429 Too Many Requests
    And "GET /metrics" shows "cxg_queue_depth" equal to 5
```

---

### Feature: M7-S5 Docker image and CI

```gherkin
Feature: M7-S5 Docker image and CI
  As Devon
  I have a Docker image with libclang 18 and a CI pipeline for every commit to main

  @AC-M7-20
  Scenario: Docker build produces an image with all three binaries and libclang 18
    Given a "Dockerfile" in the repo root
    When "docker build" runs
    Then the resulting image contains the "cxg-index" binary
    And the image contains the "cxg-resolve-cross-repo" binary
    And the image contains the "cxg-daemon" binary
    And libclang 18 is linked in the image

  @AC-M7-21
  Scenario: Commit to main triggers CI that runs cargo test on Linux and macOS and blocks on failure
    Given a commit is pushed to the "main" branch
    When CI runs
    Then "cargo test" (unit + integration) executes on Linux
    And "cargo test" executes on macOS
    And any test failure causes the CI run to fail and block the merge

  @AC-M7-22
  Scenario: More than 20% performance regression on LLVM benchmark blocks CI merge
    Given a commit that causes the LLVM indexing benchmark to run more than 20% slower than baseline
    When CI runs the benchmark step
    Then the benchmark step fails
    And the merge is blocked
```

---

### Feature: M7-S6 Runbook — recover from corrupted staging directory

```gherkin
Feature: M7-S6 Runbook recover from corrupted staging directory
  As Devon
  I have a runbook for recovering from a corrupted Phase 1 staging directory

  @AC-M7-23
  Scenario: runbook.md contains step-by-step recovery instructions
    Given "runbook.md" is read from the handoff directory
    Then it contains step-by-step instructions for:
      | step                                          |
      | Identifying a corrupted .cxg-cache/ directory |
      | Clearing the corrupted directory safely       |
      | Triggering a full re-index                    |
      | Verifying the graph is complete after re-index |

  @AC-M7-24
  Scenario: runbook.md documents the exact confirm_token derivation for POST /v1/reset
    Given "runbook.md" is read
    Then it documents that "confirm_token" is the SHA-256 hash of the target name string
    And it provides an example of the derivation for a named repo target
```

---

### Feature: M7-S7 M7 exit gate — one-week unattended run and git-URL round-trip

```gherkin
Feature: M7-S7 M7 exit gate one-week unattended run and git-URL round-trip
  As the team
  We need cxg-daemon to run unattended for one week and git-URL ingest to round-trip

  @AC-M7-25 @manual
  Scenario: cxg-daemon runs unattended for 7 days with at least one successful ingest per day
    Given "cxg-daemon" is deployed on hermes-agent with auto-reindex enabled
    When 7 consecutive days elapse with no manual intervention
    Then "GET /v1/status" returns 200 at each daily check
    And at least one ingest cycle completes successfully each day
    # Note: This is a manual/operational acceptance scenario — standard pytest-bdd cannot execute a 7-day soak.
    See OQ-BA-4: QA and devops must agree on a harness or manual checklist before M7 dispatch.

  @AC-M7-26
  Scenario: POST /v1/ingest with a public git URL round-trips to state done with correct graph nodes
    Given a live "cxg-daemon"
    When "POST /v1/ingest" is called with a public GitHub repo URL and a valid bearer token
    And the job completes
    Then "GET /v1/jobs/{id}" returns "state: done"
    And the graph DB contains nodes tagged "BELONGS_TO_REPO" → the cloned repo name

  @AC-M7-27 @manual
  Scenario: libclang error rate is under 1% over 7 days of daemon uptime
    Given 7 days of cxg-daemon uptime on hermes-agent
    When "GET /metrics" is scraped at end of period
    Then "cxg_libclang_errors_total" divided by "cxg_nodes_total" is less than 0.01
    # Note: Manual/operational check — requires the 7-day soak from AC-M7-25.
```

---

## AC coverage matrix

| AC ID | Scenario(s) | Status |
|-------|-------------|--------|
| AC-M1-1 | Fresh checkout compiles; Cross-platform macOS | confirmed |
| AC-M1-2 | All base node types present | confirmed |
| AC-M1-3 | All base edge types present | confirmed |
| AC-M1-4 | Schema round-trip passes | confirmed |
| AC-M1-5 | Valid compile_commands.json parsed and deduplicated | confirmed |
| AC-M1-6 | Duplicate entries appear exactly once | confirmed |
| AC-M1-7 | Malformed JSON exits non-zero; Missing required fields exits non-zero | confirmed / assumed |
| AC-M1-8 | compile_commands.json found in build/ and logged at INFO | confirmed |
| AC-M1-9 | No compile_commands.json — non-zero exit listing searched dirs | confirmed |
| AC-M1-10 | Walk stops at .git | confirmed |
| AC-M1-11 | File-entry match preferred; lexicographic first as tiebreak | confirmed |
| AC-M1-12 | Directory input restricts TU queue | confirmed |
| AC-M1-13 | Single-file input restricts TU queue | confirmed |
| AC-M1-14 | All base node types in Parquet shard | confirmed |
| AC-M1-15 | No DB writes during Phase 1 | confirmed |
| AC-M1-16 | libclang error recorded; run continues | confirmed |
| AC-M1-17 | Non-empty usr field in Parquet | confirmed |
| AC-M1-18 | Within-repo edges resolved by USR lookup | confirmed |
| AC-M1-19 | Cross-repo candidate edges flagged | confirmed |
| AC-M1-20 | --sink neo4j writes to Neo4j | confirmed |
| AC-M1-21 | --sink indradb writes to IndraDB | confirmed |
| AC-M1-22 | cargo test exercises both sinks | confirmed |
| AC-M1-23 | Credentials from env var; not in config or logs | confirmed |
| AC-M1-24 | Missing env var exits non-zero before indexing; invalid also | confirmed / assumed |
| AC-M1-25 | Neo4j golden-graph assertion | confirmed |
| AC-M1-26 | IndraDB isomorphic graph | confirmed |
| AC-M1-27 | Test failure diff identifies missing/extra nodes | confirmed |
| AC-M2-1 | NAMESPACE node emitted | confirmed |
| AC-M2-2 | TEMPLATE_DECL node emitted | confirmed |
| AC-M2-3 | SPECIALIZATION node emitted | confirmed |
| AC-M2-4 | HEADER node emitted | confirmed |
| AC-M2-5 | TYPEDEF node emitted | confirmed |
| AC-M2-6 | ENUM node with scoped boolean | confirmed |
| AC-M2-7 | INCLUDES edge emitted | confirmed |
| AC-M2-8 | OVERRIDES edge with vtable_slot | confirmed |
| AC-M2-9 | INSTANTIATES edge from call site | confirmed |
| AC-M2-10 | SPECIALIZES edge emitted | confirmed |
| AC-M2-11 | FRIEND_OF edge emitted | confirmed |
| AC-M2-12 | ADL_CANDIDATE edge emitted | confirmed |
| AC-M2-13 | Unresolved edge with resolved:false | confirmed |
| AC-M2-14 | System headers excluded when skip_system_headers:true | confirmed |
| AC-M2-15 | System headers included when skip_system_headers:false | confirmed |
| AC-M2-16 | Zero cross_repo_candidate edges on Boost.Optional | confirmed |
| AC-M2-17 | SPECIALIZATION and SPECIALIZES for boost::optional<T> | confirmed |
| AC-M3-1 | Parallel measurably faster than sequential | needs-clarification |
| AC-M3-2 | Worker panic does not stop others | confirmed |
| AC-M3-3 | LLVM Phase 1 ≤15 min on 32 cores | confirmed |
| AC-M3-4 | Neo4j ≥50k rows/s | confirmed |
| AC-M3-5 | IndraDB ≥100k rows/s | confirmed |
| AC-M3-6 | Mid-batch retry idempotent (Neo4j and IndraDB) | confirmed / assumed |
| AC-M3-7 | No-change re-run processes zero TUs in ≤30s | confirmed |
| AC-M3-8 | One changed file causes only that TU re-processed | confirmed |
| AC-M3-9 | Prior libclang version cache invalidated; prior schema version invalidated | confirmed |
| AC-M3-10 | Incremental re-index ≤1 min end-to-end | confirmed |
| AC-M3-11 | Phase 1 peak RSS ≤16 GB | confirmed |
| AC-M3-12 | Phase 3 USR map spills to RocksDB | confirmed |
| AC-M3-13 | Progress on stderr every 5s | confirmed |
| AC-M3-14 | Cache-hit TU counted as done immediately | confirmed |
| AC-M4-1 | REPO node with required attributes | confirmed |
| AC-M4-2 | Every node has BELONGS_TO_REPO edge | confirmed |
| AC-M4-3 | Heterogeneous sinks — refuse with error | confirmed |
| AC-M4-4 | cross_repo_candidate → EXTERNAL_REF with via; schema mismatch check | confirmed / needs-clarification |
| AC-M4-5 | Unresolved candidate remains with resolved:false | confirmed |
| AC-M4-6 | Concurrent Phase 5 serializes via advisory lock | confirmed |
| AC-M4-7 | /usr/include USR → system:libstdc++ or system:libc | confirmed |
| AC-M4-8 | third_party header → repo:vendored:<pkg> | confirmed |
| AC-M4-9 | Cypher EXTERNAL_REF query returns ≥1 path | confirmed |
| AC-M4-10 | EXTERNAL_REF via = CALLS | confirmed |
| AC-M5-1 | MACRO nodes for object-like and function-like macros | confirmed |
| AC-M5-2 | EXPANDS_TO edge from call-site | confirmed |
| AC-M5-3 | Only top-level expansions produce EXPANDS_TO | confirmed |
| AC-M5-4 | EXPANDS_TO count ≤10× source lines | confirmed |
| AC-M5-5 | Phase 2 runs by default with annotations | confirmed |
| AC-M5-6 | --skip-phase2 skips Phase 2 | confirmed |
| AC-M5-7 | .cppm indexed when libclang 18 C++20 available | needs-clarification |
| AC-M5-8 | .cppm skipped with warning when unavailable | needs-clarification |
| AC-M5-9 | --version notes C++20 limitation | confirmed |
| AC-M5-10 | Chromium exits zero without segfault | confirmed |
| AC-M5-11 | ≥1 MACRO node and ≥1 EXPANDS_TO edge in Chromium output | confirmed |
| AC-M6-1 | cargo build regenerates schema.txt | needs-clarification (Q5 blocker) |
| AC-M6-2 | New node type reflected in schema.txt | needs-clarification (Q5 blocker) |
| AC-M6-3 | CI fails on stale schema.txt | needs-clarification (Q5 blocker) |
| AC-M6-4 | example.txt has 4 idiom categories | needs-clarification (Q5 blocker) |
| AC-M6-5 | Agent produces valid Cypher for ≥3 of 4 idioms | needs-clarification (Q5 blocker) |
| AC-M6-6 | SchemaVersion node in graph | needs-clarification (Q5 + Q4 blocker) |
| AC-M6-7 | cpp-mcp returns error on version mismatch | needs-clarification (Q5 blocker) |
| AC-M6-8 | Agent answers inheritance query correctly | needs-clarification (Q5 blocker) |
| AC-M6-9 | Agent answers 8/10 NL questions correctly | needs-clarification (Q5 blocker) |
| AC-M7-1 | POST /v1/ingest 202 in ≤50ms p99 | confirmed |
| AC-M7-2 | GET /v1/jobs/{id} fields in ≤20ms p99 | confirmed |
| AC-M7-3 | GET /v1/jobs?state=done&limit=10 newest first | confirmed |
| AC-M7-4 | GET /v1/status fields | confirmed |
| AC-M7-5 | GET /v1/repos fields | confirmed |
| AC-M7-6 | 401 on missing token (POST /v1/ingest, POST /v1/reset); 401 on invalid token | confirmed / assumed |
| AC-M7-7 | RFC-7807 error bodies | confirmed |
| AC-M7-8 | Binds only to configured address | confirmed |
| AC-M7-9 | POST /v1/reset correct token deletes repo data | confirmed |
| AC-M7-10 | POST /v1/reset wrong/missing token returns 400 | confirmed |
| AC-M7-11 | POST /v1/reset target:all wipes everything | confirmed |
| AC-M7-12 | git_url on allowed_hosts triggers clone and index | confirmed |
| AC-M7-13 | Same git_url second POST uses git fetch | confirmed |
| AC-M7-14 | Host not in allowlist returns 403 | confirmed |
| AC-M7-15 | PAT from env var — not in logs or responses | confirmed |
| AC-M7-16 | default_clone_depth=1 uses --depth=1 | confirmed |
| AC-M7-17 | GET /metrics returns all required metrics | confirmed |
| AC-M7-18 | GET /metrics returns 200 without auth | confirmed |
| AC-M7-19 | POST /v1/ingest returns 429 on queue full | confirmed |
| AC-M7-20 | Docker image has all 3 binaries + libclang 18 | confirmed |
| AC-M7-21 | CI runs cargo test on Linux + macOS | confirmed |
| AC-M7-22 | >20% perf regression blocks CI | confirmed |
| AC-M7-23 | runbook.md has recovery steps | confirmed |
| AC-M7-24 | runbook.md documents confirm_token derivation | confirmed |
| AC-M7-25 | 7-day unattended run — manual/operational scenario | manual |
| AC-M7-26 | git-URL round-trip to state:done | confirmed |
| AC-M7-27 | Error rate <1% over 7 days — manual/operational scenario | manual |

Total AC IDs in requirements.md: 115 (AC-M1-1 through AC-M7-27)
Coverage: 115/115 — all AC IDs have at least one tagged scenario.

---

## References

- requirements.md: `/Users/husam/workspace/cpp-indexer/.claude/handoff/v1/requirements.md`
- CHARTER.md: `/Users/husam/workspace/cpp-indexer/.claude/handoff/v1/CHARTER.md`
- PRD v1.1: `~/workspace/wiki/pages/planning/codexgraph-cpp-prd-v1.md`
- Engineering plan v1.1: `~/workspace/wiki/pages/planning/codexgraph-cpp-libclang-rust.md`
- Cognee tags: `task:cpp-indexer role:business-analyst`
