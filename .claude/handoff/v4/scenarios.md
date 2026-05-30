# Requirements Summary

## In-scope
- Per-repo SQLite symbol/file map store (S1)
- Thread-safe LRU-cached ID allocator, both directions (S2)
- Config surface: CLI, env var, config struct (S3)
- Both sinks (Neo4j, IndraDB) write integer IDs only (S4)
- Read/query path: integer ID → USR/path resolution (S5)
- Migration/compatibility behaviour for USR-string graphs (S6)
- Test suite: unit + integration covering all above (S7)

## Out of scope
- gRPC / M9/M10 work
- New node or edge kinds
- Cross-repo integer ID unification
- Changes to libclang visit semantics beyond ID emission

## Assumptions
- `assumed` LRU eviction is write-through to SQLite (C5); no in-flight data loss on eviction.
- `assumed` "cache_size=1" is the smallest functioning LRU (a single slot); it behaves correctly under eviction.
- `assumed` "unknown integer ID on read" (stale graph) returns an explicit error or documented degradation, never silent corruption (C7).
- `assumed` Cross-repo EXTERNAL_REF phase continues to operate in USR-string space; integer IDs are never compared across repos (D1).

## Open questions
- **OQ-1** `needs-clarification` — S1: exact default SQLite path convention (e.g., `<output-dir>/cxg-symbols.db`). Decide in design phase.
- **OQ-2** `needs-clarification` — S4: exact integer field names on nodes/edges (e.g., `symbol_id`, `src_id`, `dst_id`, `file_id`). Decide in design phase.
- **OQ-3** `needs-clarification` — S5: full enumeration of read-path consumers beyond cpp-mcp tools and daemon; deferred consumers must be listed as explicit follow-ups in design.
- **OQ-4** `needs-clarification` — S6: hard-error-on-`--reset` vs auto-migrate as the default behaviour. Escalate to ADR.

## Edge cases
- `confirmed` LRU eviction — evicted entry still resolvable via SQLite (C5 write-through).
- `confirmed` cache_size=0 — allocator runs entirely without cache; every lookup hits SQLite.
- `assumed` cache_size=1 — smallest functioning LRU; entry evicted on very next insert.
- `confirmed` unknown USR (first-seen) — allocates a new integer ID (get-or-insert).
- `confirmed` unknown integer ID on read (stale/missing) — explicit error, not silent corruption (C7).
- `confirmed` concurrent get-or-insert of the same USR — both callers receive the same ID; no duplicate allocation.
- `confirmed` re-index ID stability — same USR receives the same ID on a second run (C3).
- `confirmed` both-sink round-trip — IDs written via Neo4j sink and IndraDB sink both resolve back to original USR strings and paths.
- `confirmed` cross-repo EXTERNAL_REF — USR-string matching still resolves across repos; integer IDs from two repos are never compared.
- `needs-clarification` existing USR-string graph migration/reset — behaviour depends on OQ-4 (ADR outcome).

## Stakeholders
- cpp-indexer operator (S1, S3, S6)
- cpp-indexer developer (S2, S4, S7)
- cpp-mcp / daemon consumer (S5)

---

# Gherkin

## Feature: SQLite symbol/file map store (S1)

```gherkin
Feature: SQLite symbol/file map store

  # S1-SC-01
  Scenario: Fresh repo creates SQLite database with correct schema
    Given no SQLite database exists at the configured path
    When the indexer runs for the first time against a repo
    Then a SQLite database is created at that path
    And the database contains table "symbols(id INTEGER PRIMARY KEY, usr TEXT NOT NULL UNIQUE)"
    And the database contains table "files(id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE)"

  # S1-SC-02
  Scenario: Re-index preserves existing IDs (get-or-insert semantics)
    Given a SQLite database with USR "c:@F@foo" mapped to id 1 and path "/src/foo.cpp" mapped to id 1
    When the indexer runs again on the same repo containing the same USR and path
    Then USR "c:@F@foo" is returned with id 1
    And path "/src/foo.cpp" is returned with id 1
    And no new IDs are allocated for already-known entries

  # S1-SC-03
  Scenario: Two repos use independent databases with non-shared IDs (D1)
    Given repo A has a SQLite database at path A with USR "c:@F@foo" mapped to id 1
    And repo B has a separate SQLite database at path B
    When repo B is indexed
    Then repo B's database is created at path B independently
    And id 1 in repo B's database is NOT assumed to refer to the same symbol as id 1 in repo A

  # S1-SC-04 — needs-clarification (OQ-1: default path convention)
  Scenario: Default SQLite path is used when none is configured
    Given the SQLite database path is not set in config, CLI, or env
    When the indexer starts
    Then a SQLite database is created at a documented sensible default location relative to the output directory
    # NOTE: exact default path TBD in design phase (OQ-1)
```

---

## Feature: Thread-safe LRU-cached ID allocator (S2)

```gherkin
Feature: Thread-safe LRU-cached ID allocator

  # S2-SC-01
  Scenario: Cache hit returns value without querying SQLite
    Given the allocator has USR "c:@F@bar" → id 42 in its LRU cache
    When a thread requests the id for USR "c:@F@bar"
    Then id 42 is returned
    And SQLite is not queried

  # S2-SC-02
  Scenario: Cache miss queries SQLite and populates cache
    Given the LRU cache does not contain USR "c:@F@baz"
    And USR "c:@F@baz" exists in SQLite with id 7
    When a thread requests the id for USR "c:@F@baz"
    Then SQLite is queried
    And id 7 is returned
    And USR "c:@F@baz" → id 7 is inserted into the cache

  # S2-SC-03
  Scenario: LRU eviction removes least-recently-used entry but SQLite remains authoritative
    Given the cache is at its capacity limit with entries A, B, C (A least recently used)
    When a new entry D is inserted
    Then entry A is evicted from the cache
    And entry A is still resolvable by querying SQLite directly (C5 write-through safety)
    And entry D is present in the cache

  # S2-SC-04
  Scenario: cache_size=0 disables caching; all lookups hit SQLite
    Given the allocator is constructed with cache_size = 0
    When any USR or path lookup is performed
    Then SQLite is queried on every lookup
    And no in-memory cache structure is consulted

  # S2-SC-05 (edge: smallest functioning LRU)
  Scenario: cache_size=1 evicts on every second insert
    Given the allocator is constructed with cache_size = 1
    And USR "c:@F@x" → id 10 is cached (the only slot)
    When USR "c:@F@y" is looked up and inserted into the cache
    Then USR "c:@F@x" is evicted from the cache
    And USR "c:@F@y" → id Y occupies the single cache slot
    And USR "c:@F@x" remains resolvable via SQLite

  # S2-SC-06
  Scenario: Both-direction cache lookup supported (usr→id and id→usr)
    Given USR "c:@F@foo" → id 5 is in the cache
    When a lookup for id 5 is performed (reverse direction)
    Then USR "c:@F@foo" is returned without querying SQLite

  # S2-SC-07
  Scenario: Both-direction cache lookup supported (path→id and id→path)
    Given path "/src/main.cpp" → id 3 is in the cache
    When a lookup for id 3 is performed (reverse direction)
    Then path "/src/main.cpp" is returned without querying SQLite

  # S2-SC-08 (concurrent get-or-insert — same USR)
  Scenario: Concurrent get-or-insert of the same unknown USR allocates exactly one ID
    Given USR "c:@C@Widget" is not yet in SQLite or the cache
    When two threads simultaneously call get-or-insert for USR "c:@C@Widget"
    Then exactly one integer ID is allocated for "c:@C@Widget"
    And both threads receive the same ID
    And no duplicate rows exist in the symbols table

  # S2-SC-09 (concurrent get-or-insert — distinct USRs)
  Scenario: Concurrent get-or-insert of distinct unknown USRs allocates distinct IDs
    Given USRs "c:@C@Foo" and "c:@C@Bar" are not yet in SQLite or the cache
    When two threads simultaneously call get-or-insert for "c:@C@Foo" and "c:@C@Bar" respectively
    Then "c:@C@Foo" and "c:@C@Bar" each receive a unique, distinct integer ID
    And no data race or deadlock occurs

  # S2-SC-10 (negative: no deadlock under parallel TU visits)
  Scenario: Parallel TU visits do not deadlock or produce data races
    Given 8 TUs are visited in parallel as per src/pipeline/parallel.rs
    When all TUs concurrently allocate IDs for their symbols and files
    Then all IDs are allocated successfully
    And the process completes without deadlock, panic, or data corruption
```

---

## Feature: Config surface for cache size and SQLite path (S3)

```gherkin
Feature: Config surface for cache size and SQLite path

  # S3-SC-01
  Scenario: CLI flag sets cache size
    Given the indexer is invoked with "--symbol-cache-size 512"
    When the allocator is constructed
    Then it uses an LRU cache of maximum size 512

  # S3-SC-02
  Scenario: Environment variable sets cache size
    Given environment variable CXG_SYMBOL_CACHE_SIZE=256 is set
    When the indexer starts
    Then the allocator uses an LRU cache of maximum size 256

  # S3-SC-03
  Scenario: CLI flag sets SQLite database path
    Given the indexer is invoked with "--symbol-db-path /data/repo.db"
    When the indexer starts
    Then the SQLite database is opened or created at /data/repo.db

  # S3-SC-04
  Scenario: Environment variable sets SQLite database path
    Given environment variable CXG_SYMBOL_DB_PATH=/tmp/test.db is set
    When the indexer starts
    Then the SQLite database is opened or created at /tmp/test.db

  # S3-SC-05
  Scenario: cache_size=0 via any config surface disables caching
    Given cache_size is set to 0 via CLI, env var, or config struct
    When the allocator is constructed
    Then it operates without an in-memory cache (all lookups hit SQLite)

  # S3-SC-06
  Scenario: Defaults are used when no config is provided
    Given no cache size and no database path are specified
    When the indexer starts
    Then a documented default cache size is used
    And a documented default database path is used
```

---

## Feature: Both sinks emit integer IDs (S4)

```gherkin
Feature: Both sinks emit integer IDs

  # S4-SC-01
  Scenario: Neo4j sink writes integer symbol ID on nodes, not USR string
    Given the Neo4j sink is active
    When a symbol node is written
    Then the node carries an integer symbol_id property
    And the node does NOT carry a "usr" string property
    # NOTE: exact field name (symbol_id / id) to be confirmed in design (OQ-2)

  # S4-SC-02
  Scenario: Neo4j sink writes integer src_id/dst_id on edges, not USR strings
    Given the Neo4j sink is active
    When an edge between two symbols is written
    Then the edge carries integer src_id and dst_id properties
    And the edge does NOT carry "src_usr" or "dst_usr" string properties

  # S4-SC-03
  Scenario: Neo4j sink stores filenames as integer file IDs
    Given the Neo4j sink is active
    When a file-location property is written on a node or edge
    Then an integer file_id is stored, not the path string

  # S4-SC-04
  Scenario: IndraDB sink writes integer IDs on nodes (D2)
    Given the IndraDB sink is active
    When a symbol node is written
    Then the node carries an integer symbol_id
    And the node does NOT carry a "usr" string property

  # S4-SC-05
  Scenario: IndraDB sink writes integer IDs on edges (D2)
    Given the IndraDB sink is active
    When an edge is written
    Then the edge carries integer src_id and dst_id
    And the edge does NOT carry "src_usr" or "dst_usr"

  # S4-SC-06
  Scenario: Graph storage is measurably smaller after switch to integer IDs
    Given the spdlog repo (or equivalent representative C++ repo) is indexed before this change
    When the same repo is indexed after this change
    Then the total node+edge storage bytes are reduced by at least 30%

  # S4-SC-07
  Scenario: SCHEMA_VERSION is bumped in the same commit as the ID representation change
    Given src/schema/version.rs contains the current SCHEMA_VERSION
    When this feature is merged
    Then SCHEMA_VERSION in src/schema/version.rs is higher than the pre-merge value
    And the SCHEMA_VERSION bump is in the same commit as the first integer-ID write (ADR-9, C2)
```

---

## Feature: Read/query path ID resolution (S5)

```gherkin
Feature: Read/query path ID resolution

  # S5-SC-01
  Scenario: Graph query result integer IDs are resolved to USR strings before returning
    Given the graph contains nodes with integer symbol IDs
    When a caller invokes get_definition, get_references, get_ast, or query_graphdb
    Then each integer ID in the result is resolved to its USR string via SQLite
    And no raw integer IDs appear in the caller-facing output (C1)

  # S5-SC-02
  Scenario: Daemon endpoint resolves integer IDs before HTTP response
    Given the daemon is running and the graph contains integer IDs
    When GET /v1/repos or another symbol-bearing endpoint is called
    Then all integer IDs in the response are resolved to USR strings or file paths
    And no raw integer IDs are present in the HTTP response body

  # S5-SC-03 (negative: SQLite unavailable or ID missing)
  Scenario: Missing integer ID on read returns explicit error, not silent corruption
    Given SQLite is unavailable OR an integer ID exists in the graph with no corresponding SQLite row
    When ID resolution is attempted
    Then an explicit error or a documented degradation response is returned
    And the caller is not silently given corrupted or fabricated data (C7)

  # S5-SC-04
  Scenario: Callers' string-based output contract is preserved after migration
    Given a caller that previously received USR strings directly from the read path
    When the read path is updated to resolve IDs and the same query is re-run
    Then the caller receives the same USR strings and file paths as before
    And no interface contract is broken
```

---

## Feature: Migration and compatibility for USR-string graphs (S6)

```gherkin
Feature: Migration and compatibility for USR-string graphs

  # S6-SC-01 — needs-clarification (OQ-4: hard-error vs auto-migrate)
  Scenario: Indexer detects SCHEMA_VERSION mismatch on startup
    Given an existing graph written with a previous SCHEMA_VERSION (USR-string format)
    When the indexer starts
    Then it detects the version mismatch
    And it either (a) exits with a clear error message requiring explicit reset/re-index
              OR (b) executes a documented migration path
    # Decision on (a) vs (b) deferred to ADR (OQ-4)

  # S6-SC-02
  Scenario: No silent data corruption during chosen migration/reset path
    Given the chosen path is a reset (hard-error) or auto-migrate
    When the operator executes the described procedure
    Then no graph data is silently corrupted
    And if data will be lost, the operator is informed before the destructive step proceeds (C7)

  # S6-SC-03
  Scenario: Fresh index produces a graph with no USR strings on node/edge properties
    Given a new SCHEMA_VERSION is in effect
    When a fresh index is run against any C++ repo
    Then no "usr", "src_usr", or "dst_usr" string properties appear on any node or edge in the graph

  # S6-SC-04
  Scenario: Runbook documents the upgrade procedure
    Given runbook.md exists in the handoff dir
    When this feature ships
    Then runbook.md contains the re-index recipe or migration command for upgrading from a USR-string graph
```

---

## Feature: Test coverage — SQLite map, LRU cache, ID stability, both-sink round-trip (S7)

```gherkin
Feature: Test coverage

  # S7-SC-01
  Scenario: Unit test — insert new USR allocates a new ID
    Given an empty symbols table
    When get-or-insert is called for USR "c:@F@new"
    Then a new integer ID is returned
    And a row is created in the symbols table

  # S7-SC-02
  Scenario: Unit test — insert duplicate USR returns the same ID
    Given USR "c:@F@dup" already exists in the symbols table with id 99
    When get-or-insert is called for USR "c:@F@dup" again
    Then id 99 is returned
    And no additional row is created

  # S7-SC-03
  Scenario: Unit test — insert new path allocates a new ID
    Given an empty files table
    When get-or-insert is called for path "/foo/bar.cpp"
    Then a new integer ID is returned
    And a row is created in the files table

  # S7-SC-04
  Scenario: Unit test — insert duplicate path returns the same ID
    Given path "/foo/bar.cpp" exists in the files table with id 5
    When get-or-insert is called for "/foo/bar.cpp" again
    Then id 5 is returned
    And no additional row is created

  # S7-SC-05
  Scenario: Unit test — database persistence across process restart
    Given the indexer ran and allocated IDs, then the process exited
    When a new indexer process opens the same SQLite database
    Then previously allocated IDs are still present and unchanged

  # S7-SC-06
  Scenario: Unit test — LRU cache hit does not query SQLite
    Given an entry is present in the LRU cache
    When that entry is looked up
    Then SQLite is not queried

  # S7-SC-07
  Scenario: Unit test — LRU cache miss queries SQLite and populates cache
    Given an entry is absent from the LRU cache but present in SQLite
    When that entry is looked up
    Then SQLite is queried and the result is stored in the cache

  # S7-SC-08
  Scenario: Unit test — LRU eviction removes the least-recently-used entry
    Given a cache at capacity with a known LRU order
    When a new entry is inserted
    Then the least-recently-used entry is evicted

  # S7-SC-09
  Scenario: Unit test — cache_size=0 disables caching
    Given the allocator is built with cache_size = 0
    When any lookup is performed
    Then SQLite is always queried

  # S7-SC-10
  Scenario: Unit test — both-direction lookups work (usr→id and id→usr)
    Given an entry usr→id in the cache
    When the reverse lookup id→usr is performed
    Then the correct USR string is returned

  # S7-SC-11
  Scenario: Unit test — both-direction lookups work (path→id and id→path)
    Given an entry path→id in the cache
    When the reverse lookup id→path is performed
    Then the correct file path string is returned

  # S7-SC-12 (integration)
  Scenario: Integration test — re-index ID stability (C3)
    Given the indexer has run once against a C++ repo and allocated IDs
    When the same repo is re-indexed
    Then every USR that appeared in the first run receives the same integer ID as before

  # S7-SC-13 (integration)
  Scenario: Integration test — both-sink round-trip
    Given nodes and edges are written via the Neo4j sink with integer IDs
    When the IDs are resolved through the SQLite read path
    Then the original USR strings and file paths are recovered correctly

  # S7-SC-14 (integration)
  Scenario: Integration test — both-sink round-trip for IndraDB
    Given nodes and edges are written via the IndraDB sink with integer IDs
    When the IDs are resolved through the SQLite read path
    Then the original USR strings and file paths are recovered correctly

  # S7-SC-15 (regression)
  Scenario: All previously green tests remain green after this feature
    Given the full test suite ran and passed before this feature (excluding the pre-existing schema_drift failure)
    When the test suite is run after this feature is implemented
    Then all previously passing tests still pass
    And the pre-existing schema_drift failure remains the only known exception
```

---

No further cases identified beyond those listed.

---

# References

- `/Users/husam/workspace/cpp-indexer/.claude/handoff/v4/requirements.md`
- `/Users/husam/workspace/cpp-indexer/.claude/handoff/v4/CHARTER.md`
- Wiki: [[pages/code/cpp-indexer]] — pipeline phases, sink files, SCHEMA_VERSION, Issue 0001 context
- Wiki: [[pages/code/cpp-mcp]] — read-path consumer surface (get_definition, get_references, get_ast, query_graphdb, describe_graph_schema)
- Cognee tags: `task:graph-symbol-ids`, `role:business-analyst`
