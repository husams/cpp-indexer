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

## Agent workflow preferences

- When implementing features, Luna may be used with High or Extra effort.
- When discussing or asking about design features, Sol may be used with High effort.
- When creating a new thread, do not copy the existing context. Summarize the
  context and include only the information needed for the task.
- For implementations involving multiple stories, track progress in Markdown
  files or a SQLite database.
- When asked for the status of a story, provide only a one-line summary. Give
  details only when explicitly requested.
