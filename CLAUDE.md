# Contributor guide for coding agents

`cidx` is a semantic C/C++ indexer. **Indexing is C++23 only** (`src/`, built by
CMake) on the **Clang C++ / LibTooling API — libclang (the C API) has been fully
removed** (no `clang-c/*`, no libclang link). The **Python** tree
(`python/indexer/`) is being narrowed to storage + graph read/query; its
libclang-based indexer is legacy pending removal in a **separate phase** — do not
extend it. Detailed guidance lives in project skills — load the one that fits the
task:

- **cidx-dual-implementation** — the shared contract and change discipline;
  landing a behavioral change in both languages.
- **cidx-build-and-test** — build/test both suites and pick the right gate.
- **cidx-codebase-map** — where each concern lives across the two trees.

## Rules and constraints

- **Respond with a summary only — never a long, detailed explanation. Give
  details only when the user explicitly asks for them.**
- The byte-identical dual-implementation contract is **retired**: C++ (LibTooling)
  is the sole indexer, and its AST-traversal order legitimately differs from the
  old libclang/Python output, so `index.db` is no longer byte-comparable to
  Python's. Land indexing/query/CLI/schema changes in **C++ with C++ tests**; only
  touch the Python tree for storage/read-query parity until its indexer is retired.
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
- **Baseline before you build.** Run the relevant test gates (default, and
  `clang`/parity as the change warrants) and record their green/red state
  **before** starting work on any feature or fix. Do not begin coding until the
  starting state is known; if the baseline is already red, surface it and
  investigate first.
- **Investigate EVERY failing test — related to your change or not.** Never
  dismiss a red as "pre-existing." Run it, read the exact assertion, root-cause
  it, and either fix it or prove factually that it is not a regression (e.g. show
  the diff is comment-only, or compare against the pre-work baseline) and state
  exactly how to make it pass. No hand-waving. Applies to your own runs and any
  delegated ones.
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
