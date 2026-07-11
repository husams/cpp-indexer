# Contributor guide for coding agents

`cidx` is one semantic indexer with two implementations that must stay
behaviorally compatible: **Python** (`python/indexer/`, canonical) and **C++23**
(`src/`, built by CMake). Detailed guidance lives in project skills — load the
one that fits the task:

- **cidx-dual-implementation** — the shared contract and change discipline;
  landing a behavioral change in both languages.
- **cidx-build-and-test** — build/test both suites and pick the right gate.
- **cidx-codebase-map** — where each concern lives across the two trees.

## Rules and constraints

- **Respond with a summary only — never a long, detailed explanation. Give
  details only when the user explicitly asks for them.**
- Any change to the observable contract (schema/migrations, indexing/query
  semantics, CLI flags and text output, JSON shapes, path handling, exit
  behavior) must land in **both** languages in the **same** change, with tests
  in both suites. Python is the reference; do not defer C++ parity.
- Bump the schema version in `python/indexer/storage.py` and
  `src/storage/storage.cpp` together, with migrations and old-database tests.
- Do not reintroduce the removed Rust/Cargo, Neo4j, IndraDB, or daemon code.
- Do not add a new dependency when the standard library or an existing utility
  already covers the need. Prefer small changes within existing module
  boundaries.
- Keep text and JSON output deterministic — do not casually change ordering,
  field names, null handling, or formatting.
- Report exactly which checks ran and which were skipped. Never claim parity
  from only one language's tests.
- Git: agents may create worktrees and feature branches; keep each scoped to a
  single task and never remove a worktree or branch that may hold another
  contributor's work.
- Keep agent scratch files under `/tmp` only — never in the repository tree.
  Do not commit generated artifacts (build dirs, caches, `__pycache__`, temp
  databases, local virtualenvs). The one exception is the checked-in semantic
  index `index.db` (see below).
- The semantic index `index.db` is committed to the repo. Keep it current: after
  any change that alters what the index would contain (source under `src/` or
  `python/indexer/`, schema version, or indexing/query semantics), re-run the
  indexer to regenerate `index.db` and commit the refreshed database in the same
  change. Verify with `sqlite3 index.db "SELECT value FROM meta WHERE
  key='schema_version';"` — it must match the current schema version.
