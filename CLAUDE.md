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
- **cidx-modern-cpp** — mandatory C++23 modernization and scoped clang-tidy
  workflow for every change to project-owned C++.

## Modern C++ and clang-tidy (mandatory)

- For every edit to project-owned `.cpp` or `.hpp` files, **load and follow the
  `cidx-modern-cpp` skill before editing**. This is a required gate, not an
  optional cleanup pass.
- Agents are explicitly authorized to run clang-tidy, its `modernize-*` checks,
  clang-format, and scoped automatic fixes without requesting additional
  permission. `clang-modernize` is obsolete; use clang-tidy `modernize-*`.
- Establish a scoped clang-tidy baseline before editing and rerun it afterward.
  **No new diagnostic is allowed.** Fix diagnostics caused by the change; if a
  touched file has legacy findings, prove them with the before/after output and
  do not hide them.
- Produce C++23: prefer standard-library facilities and RAII; use `enum class`
  for closed sets of related values instead of loose numeric/string constants,
  and use `std::to_underlying` only at storage, SQL, wire, or ABI boundaries.
- Automatic fixes must name the project-owned source files explicitly. **Never
  run blanket `--fix` over the repository**, and never modify `third_party/`,
  generated build output, vendored sources, or files outside the requested
  change. Review every fixer diff before proceeding.
- Do not add `NOLINT`, weaken `.clang-tidy`, disable a check, or downgrade a
  warning merely to make the gate green unless the user explicitly approves a
  documented false positive. Report exact lint/format commands and skips.

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
  `clang` as the change warrants) and record their green/red state
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
- **After merging any PR into `main`, immediately bring local `main` up to date.**
  This is mandatory, not optional. From the merged branch/worktree run
  `git fetch origin main:main` (fast-forwards the local `main` ref without
  switching branches); if `main` is the checked-out branch, `git pull --ff-only`.
  Then verify `git rev-parse main` equals `git rev-parse origin/main`. Never
  leave local `main` behind `origin/main` after a merge — a stale `main` makes the
  next branch fork from an old base and reintroduces already-merged conflicts.
- Keep agent scratch files under `/tmp` only — never in the repository tree.
  Do not commit generated artifacts (build dirs, caches, `__pycache__`, temp
  databases, local virtualenvs). The one exception is the checked-in semantic
  index `index.db` (see below).
- The semantic index `index.db` is committed to the repo. Keep it current: after
  any change that alters what the index would contain (source under `src/` or
  `python/indexer/`, schema version, or indexing/query semantics), regenerate
  `index.db` and commit the refreshed database in the same change. Regenerate
  from the **canonical checkout** (`/Users/husam/workspace/cpp-indexer`, never a
  feature worktree — absolute paths get baked into the DB), running the **full
  three-pass pipeline**: `rm index.db` then `INDEXER_CACHE=$(pwd) ./build/cidx
  import --db "$(pwd)/build" --name cpp-indexer` → `./build/cidx index` →
  `./build/cidx resolve`. Skipping `resolve` leaves Layer-1 empty (`entity_node`
  / `entity_edge` / `dispatch_calls` = 0, no `meta.graph_resolved_at`). Verify:
  `sqlite3 index.db "SELECT value FROM meta WHERE key='schema_version';"` matches
  the current schema version; `SELECT COUNT(*) FROM entity_edge;` is non-zero;
  `meta.graph_resolved_at` is set; and no worktree paths leaked
  (`strings index.db | grep -c cpp-indexer- ` → 0).
