# cidx shared contract and change discipline

## The split (post libclang cutover)

C++ (`src/`) is the SOLE indexer and CLI. The Python tree (`python/indexer/`)
is storage + graph read-query only; its libclang indexer is legacy pending
removal in a separate phase — **do not extend it**, and do not treat it as a
behavioral reference for indexing.

## What still lands in BOTH languages in the same change

- SQLite schema and migrations — the schema version appears in
  `python/indexer/storage.py` and `src/storage/storage.cpp`; update both
  together, with migrations and old-database tests in both suites.
- Storage/read-query semantics the Python API exposes (readers of the shared
  tables: symbols, edges, entities, template args, …).

## What is C++-only

- Indexing semantics (the `src/ast` visitors), CLI flags and text output,
  JSON shapes, path handling, exit behavior. Land these in C++ with C++
  tests. The retired byte-parity gate is NOT the bar: the semantic contract
  is the normalized Layer-0 row set — compare with
  `scripts/dump_layer0.sh` and the index golden gate
  (`tests/index_golden_test.cpp`).

## Change discipline

- Add focused regression coverage for shared storage/read behavior in both
  suites; for indexing behavior, in the C++ suites (`ast_visitor_test`, the
  golden gate). Match existing nearby test structure and fixtures.
- Preserve database compatibility. Schema changes need matching version
  bumps, migrations, and tests for opening older databases in both
  implementations.
- Keep text and JSON output deterministic. Do not casually change ordering,
  field names, null handling, or formatting. Database ROW SETS are the
  contract, not insertion order.
- Keep fixtures read-only during tests; copy database fixtures to a temporary
  directory before migration.
- Prefer small changes in the existing module boundaries. Do not add a new
  dependency when the standard library or an existing utility already covers
  the need.
- Do not reintroduce the removed Rust/Cargo, Neo4j, IndraDB, daemon, or
  libclang code.

## Validation reporting

- Report exactly which checks were run and whether any were skipped because
  of a missing optional tool or fixture.
- Never claim a shared-storage change is validated from only one language's
  tests.
- When a full suite is impractical during iteration, run the narrow test
  first, then the required gates before handoff.

See the `cidx-build-and-test` skill for the gate commands.
