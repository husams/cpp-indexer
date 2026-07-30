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
- **backlog** (`backlog-plugin:backlog`) — the only place planned work is
  tracked; see [Backlog](#backlog-mandatory) below.

## Backlog (mandatory)

- **Load the `backlog` skill before planning, grooming, starting, reviewing, or
  closing any work.** Features, stories, subtasks, acceptance criteria,
  dependencies, review threads and PR links live there — never in Markdown
  checklists, ad-hoc SQLite, TODO comments, or chat.
- **Store:** shared PostgreSQL (`BACKLOG_DB=postgres`), project slug
  `cpp-indexer`, resolved from the repository directory name. Confirm with
  `backlog where` before acting; `backlog board` / `backlog next --actor <you>`
  is the way in.
- **Transition workflow:** `.backlog/workflow.yaml` — the shipped default flow
  (Created → Ready → In Progress → In Review → Accepted → Done, with an
  Incomplete refinement path and a Needs Work review loop). `backlog statuses`
  prints the live per-type statuses and their gates.
- **Never request a destination status.** Run `backlog actions <KEY>`, then
  `backlog action <KEY> <ACTION> --actor <you>`. The workflow chooses the state
  and runs the gates; a refusal (exit 1) is the rule, not an obstacle to work
  around.
- **Record Git state as it happens:** `backlog set <KEY> --branch <branch>`
  before `work.started`, and `backlog pr set <KEY> --url <URL> --state open` as
  soon as the PR exists — do not wait for the `pr_recorded` gate to fail.
- **CI mirrors the PR into the backlog.** `.github/workflows/backlog.yml`
  records `pr.*` and `check.*` actions from GitHub, but only when the branch —
  or failing that the PR title — carries the backlog key (`S-070-short-desc`,
  `[S-070] …`). Without a key the sync is skipped, so name the branch after the
  backlog key, not only the Linear issue.
- **Never merge unless `backlog gate <KEY> --for merge` exits 0**, and record
  `backlog pr set <KEY> --state merged` immediately after merging.
- **Every review thread needs a reply and a reviewer decision.** Use the
  `backlog review` commands (`open` / `reply` / `inbox`); `artifact add` is for
  durable documents, not feedback.
- **Never touch the store directly** — no `sqlite3`, no `psql`, no SQL against
  `.backlog/*.db` or the PostgreSQL schema. Direct access bypasses the flow,
  the gates, and the audit trail.

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
- For implementations involving multiple stories, track progress in the backlog
  (see [Backlog](#backlog-mandatory)) — never in Markdown files or a hand-rolled
  database.
- When asked for the status of a story, provide only a one-line summary. Give
  details only when explicitly requested.
