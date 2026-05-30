# Requirements: graph-symbol-ids
Feature: Compact integer IDs in CodexGraph — SQLite symbol/file maps + LRU cache

## Fixed Design Decisions (ACCEPTED — not open for discussion)
- **D1 — Per-repo ID namespace.** Integer IDs are allocated per indexed repo. Phase 5 cross-repo resolution (`src/resolve/cross_repo.rs`, `EXTERNAL_REF`) continues to match references in USR-STRING space. Integer IDs MUST NOT be compared across repos.
- **D2 — Both sinks.** The ID-only graph and SQLite maps apply to both the Neo4j sink (`src/sink/neo4j.rs`) and the IndraDB sink (`src/sink/indradb.rs`).

## Out of Scope
- gRPC work (M9/M10) — no changes to `cxg-daemon` gRPC surface.
- New node or edge KINDS.
- Changes to libclang visit semantics beyond emitting integer IDs instead of USR strings.
- Cross-repo ID unification.

---

## Stories

---

### S1 — SQLite symbol/file map store

Story: As a cpp-indexer operator, I want a per-repo SQLite database that durably stores `usr → id` and `path → id` mappings, so that integer IDs can be allocated once and reused across re-index runs without breaking existing graph references.

Acceptance criteria:
- Given a fresh repo, when the indexer runs for the first time, then a SQLite database is created at the configured path with tables `symbols(id INTEGER PRIMARY KEY, usr TEXT NOT NULL UNIQUE)` and `files(id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE)`.
- Given an existing SQLite database with previously allocated IDs, when the indexer runs again on the same repo, then known USRs and paths receive their original integer IDs (get-or-insert semantics; no truncate-and-realloc).
- Given two different repos, when each is indexed, then each gets its own independent SQLite database and IDs are not shared or compared across them (D1).
- Given the SQLite database path is unset, then the database is created at a sensible default location relative to the workspace/output directory.

Priority: P0 — foundational; all other stories depend on this store.
Dependencies: none
Open questions:
- What is the exact default database path convention (e.g., `<output-dir>/cxg-symbols.db`)? Recommend deciding in design phase.
References: CHARTER.md; constraint C3, C6; wiki [[pages/code/cpp-indexer]]

---

### S2 — Thread-safe ID allocator with LRU cache

Story: As a cpp-indexer developer, I want a thread-safe USR↔ID and path↔ID allocator backed by SQLite and fronted by a configurable LRU cache, so that parallel TU visits can resolve IDs without data races and without hitting SQLite on every lookup.

Acceptance criteria:
- Given parallel TU visits (`src/pipeline/parallel.rs`), when multiple threads allocate or look up IDs concurrently, then no data races or deadlocks occur; existing `src/sink/lock.rs` patterns are reused where applicable (C4).
- Given a cache hit (USR→id or id→USR, path→id or id→path), when a lookup is performed, then SQLite is not queried; the cached value is returned.
- Given a cache miss, when a lookup is performed, then SQLite is queried and the result is inserted into the cache before returning.
- Given cache eviction (LRU policy), when an entry is evicted, then the eviction is always safe because SQLite remains the authoritative source; no data is lost (C5 write-through).
- Given `cache_size = 0`, when the allocator is constructed, then it operates correctly with no cache (every lookup hits SQLite directly) (C5 disabled path).
- Given a configured positive cache size, when the cache reaches that size and a new entry is inserted, then the least-recently-used entry is evicted (C5 LRU eviction).
- Given two directions (usr→id and id→usr; path→id and id→path), the cache supports lookups in both directions (C5 both directions).

Priority: P0 — required for correct parallel write.
Dependencies: S1
Open questions: none
References: CHARTER.md; constraints C4, C5; `src/pipeline/parallel.rs`, `src/sink/lock.rs`

---

### S3 — LRU cache and SQLite path configuration surface

Story: As a cpp-indexer operator, I want the LRU cache maximum size and the SQLite database path to be configurable via CLI flag, environment variable, and config struct, so that I can tune memory usage and storage location without recompiling.

Acceptance criteria:
- Given the CLI, environment variables, and `src/config/mod.rs` + `src/config/env.rs`, when a user sets `--symbol-cache-size <N>` (or equivalent) or `CXG_SYMBOL_CACHE_SIZE=<N>`, then the allocator uses that size (C5 configurable size).
- Given the CLI or `CXG_SYMBOL_DB_PATH=<path>` env var, when a user sets the SQLite database path, then the indexer uses that path instead of the default (C6 configurable path).
- Given no configuration, when the indexer starts, then a documented sensible default cache size and default database path are used (C5, C6).
- Given `cache_size = 0` set via any of the three surfaces, then the allocator disables caching and operates correctly (C5).

Priority: P1 — operator usability; can land alongside S2 in the same PR.
Dependencies: S1, S2
Open questions: none
References: CHARTER.md; constraints C5, C6; `src/config/mod.rs`, `src/config/env.rs`

---

### S4 — Write path: both sinks emit integer IDs

Story: As a cpp-indexer developer, I want both the Neo4j and IndraDB sinks to store only integer IDs (not USR strings) on nodes and edges, so that the graph is compacted and long USR strings are no longer repeated on every node and edge.

Acceptance criteria:
- Given the Neo4j sink (`src/sink/neo4j.rs`), when a node or edge is written, then the node carries the integer symbol ID (not the USR string) and edges carry integer `src_id`/`dst_id` (not `src_usr`/`dst_usr`); filenames are stored as integer file IDs (D2).
- Given the IndraDB sink (`src/sink/indradb.rs`), when a node or edge is written, then the same ID-only representation is used (D2).
- Given a representative C++ repo (e.g., spdlog or {fmt}), when indexed before and after this change, then the per-node/per-edge byte size in the graph is measurably reduced; a target of ≥30% reduction in total node+edge storage bytes is the success criterion (measurable graph-size reduction).
- Given SCHEMA_VERSION in `src/schema/version.rs`, when this change is merged, then SCHEMA_VERSION is bumped in the same commit (C2, ADR-9).

Priority: P0 — core feature.
Dependencies: S1, S2, S3
Open questions:
- Exact field names for integer IDs on nodes/edges (e.g., `symbol_id`, `src_id`, `dst_id`, `file_id`) — decide in design phase.
References: CHARTER.md; constraints C2; `src/sink/neo4j.rs`, `src/sink/indradb.rs`, `src/schema/nodes.rs`, `src/schema/edges.rs`, `src/schema/arrow.rs`, `src/schema/version.rs`; wiki [[pages/code/cpp-indexer]]

---

### S5 — Read/query path: ID → USR/path resolution

Story: As a user of the cpp-mcp tools and daemon, I want every consumer that returns symbols or filenames to resolve integer IDs back to USR strings and file paths from SQLite, so that I always see human-readable symbol names in query results rather than opaque integers.

Acceptance criteria:
- Given `get_definition`, `get_references`, `get_ast`, `query_graphdb`, and related cpp-mcp tool surfaces, when a graph query returns nodes or edges with integer IDs, then the read layer resolves each ID to its USR string or path before returning results to the caller (C1).
- Given the daemon (`cxg-daemon`) `GET /v1/repos` and any other endpoint that surfaces symbol or filename data, when the endpoint is called, then IDs are resolved to USR strings or paths before the HTTP response is sent (C1).
- Given SQLite is unavailable or the ID is missing (e.g., a stale graph), when resolution is attempted, then an explicit error or a documented degradation behaviour is returned — not a silent corruption (C7 downstream guard).
- Given any read path that was previously returning USR strings directly, when this story is complete, then the callers' contract (string-based output) is preserved.

Priority: P1 — required for C1 compliance; can be parallelised with S4 if sinks and readers are developed concurrently.
Dependencies: S1, S2, S4
Open questions:
- Scope of read-path coverage: are there consumers beyond cpp-mcp tools and the daemon? Enumerate in design phase and record any deferred consumers as explicit follow-ups.
References: CHARTER.md; constraint C1; wiki [[pages/code/cpp-mcp]], [[pages/code/cpp-indexer]]

---

### S6 — Migration / compatibility behaviour for USR-string graphs

Story: As a cpp-indexer operator, I want a clearly defined and documented behaviour when the indexer encounters an existing graph written with USR strings, so that existing deployments are not silently corrupted.

Acceptance criteria:
- Given an existing graph (SCHEMA_VERSION < bumped version) written with USR strings, when the indexer starts, then it detects the version mismatch and either (a) requires a reset/re-index with a clear error message, or (b) executes a documented migration path — whichever is decided in design (C7).
- Given the chosen path (reset or migration), when it is executed, then no graph data is silently corrupted; the operator is informed of any data loss before it occurs (C7).
- Given the new SCHEMA_VERSION, when a fresh index is run, then the resulting graph is fully in the integer-ID format with no USR strings stored in node/edge properties.
- Given operator documentation (`runbook.md`), when this feature ships, then the upgrade procedure (re-index recipe or migration command) is described.

Priority: P1 — correctness and safety for existing deployments.
Dependencies: S4
Open questions:
- Should the default be hard-error (require explicit `--reset`) or auto-migrate? Escalate to design/ADR phase.
References: CHARTER.md; constraint C7; `src/schema/version.rs`; wiki [[pages/code/cpp-indexer]]

---

### S7 — Tests: SQLite map, LRU cache, ID stability, both-sink round-trip

Story: As a developer, I want a comprehensive test suite covering the SQLite map operations, LRU cache behaviour, per-repo ID stability across re-index, and both-sink round-trip, so that regressions are caught automatically.

Acceptance criteria:
- Given the SQLite map store, when tests run, then unit tests cover: insert-new-USR allocates a new ID, insert-duplicate-USR returns the same ID, insert-new-path allocates a new ID, insert-duplicate-path returns the same ID, and database persistence across process restart.
- Given the LRU cache, when tests run, then unit tests cover: cache hit (no SQLite query), cache miss (SQLite query + cache population), eviction (LRU entry removed when size limit reached), size-0 disabled path (all lookups hit SQLite), and both-direction lookups (usr→id and id→usr; path→id and id→path).
- Given a re-index run, when tests run, then an integration test verifies that a known USR receives the same integer ID on the second index run as on the first (C3 ID stability).
- Given both sinks, when tests run, then an integration test writes nodes and edges via the Neo4j sink and the IndraDB sink with integer IDs, then reads back and resolves to the original USR strings and paths (both-sink round-trip).
- Given all existing tests, when the test suite runs after this feature, then all previously green tests remain green (except the pre-existing unrelated `schema_drift` failure).

Priority: P1 — success criterion for the feature.
Dependencies: S1, S2, S4, S5
Open questions: none
References: CHARTER.md; success criteria in feature brief; `src/pipeline/parallel.rs`
