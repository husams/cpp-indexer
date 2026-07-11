# cidx shared contract and change discipline

## The observable contract

The Python and C++ implementations share an observable contract. Changing any of
these means changing both implementations in the same change:

- SQLite schema and migrations
- Indexing and query semantics
- CLI flags and text output
- JSON shapes
- Path handling
- Exit behavior

The current main database schema version appears in `python/indexer/storage.py`
and `src/storage/storage.cpp`; update both together.

## Change discipline

- Treat Python behavior as the reference, but do not leave C++ parity for a
  later change.
- Add focused regression coverage in **both** suites for shared behavior. Match
  existing nearby test structure and fixtures.
- Preserve database compatibility. Schema changes need matching version bumps,
  migrations, and tests for opening older databases in both implementations.
- Keep text and JSON output deterministic. Do not casually change ordering,
  field names, null handling, or formatting.
- Keep fixtures read-only during tests; copy database fixtures to a temporary
  directory before migration.
- Prefer small changes in the existing module boundaries. Do not add a new
  dependency when the standard library or an existing utility already covers the
  need.
- Do not reintroduce the removed Rust/Cargo, Neo4j, IndraDB, or daemon code.

## Validation reporting

- Report exactly which checks were run and whether any were skipped because of a
  missing optional tool or fixture.
- Do not claim parity from only one language's tests.
- When a full suite is impractical during iteration, run the narrow test first,
  then the required gates before handoff.

See the `cidx-build-and-test` skill for the gate commands.
