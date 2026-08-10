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
- **Wiki use is opt-in only.** Do not read, search, ingest, lint, create, update,
  or otherwise modify anything under `~/workspace/wiki/` unless the user
  explicitly asks you to use or update the wiki in the current request. Never
  update wiki pages automatically as a side effect of research, planning,
  implementation, or answering a question.
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
  databases, local virtualenvs, `index.db`).

## Semantic index (`index.db`)

`index.db` is **not in git** — it is ~158 MiB, is listed in `.gitignore`, and
lives in the private `cidx-index` bucket on `minio-api.senussi.me`
(Tailscale-only). [`scripts/index-db.sh`](scripts/index-db.sh) is the only
supported way to move it: `push` / `pull` / `list`. Credentials come from the
`minio` secret in the cluster's `infrastructure` namespace, or from
`MINIO_ACCESS_KEY` / `MINIO_SECRET_KEY`.

- **Pull before you start work — always.** The first thing in any session,
  branch, or worktree: `./scripts/index-db.sh pull`. Never start a task against
  a missing index, and never work around a missing one by rebuilding it.
- **Never run a full re-index.** The three-pass `import → index → resolve`
  rebuild is measured in minutes-to-hours and is not part of ordinary work. It
  is never an acceptance criterion, an exit gate, or a merge blocker. Rebuild
  from scratch only when the user explicitly asks for it.
- **Re-index only the files you changed**, incrementally, as you change them.
  Export `INDEXER_CACHE` once for every `cidx` invocation in the shell — a
  per-command prefix leaves `index`/`resolve` pointed at the global
  `~/.cache/cidx/index.db`. Run from the checkout root; relative paths resolve
  against `$PWD`, not the component root.

  ```bash
  export INDEXER_CACHE="$(pwd)"
  ./build/cidx file set pending=True --file src/util/env.cpp   # once per changed file
  ./build/cidx index src/util/env.cpp                          # ~5 s per TU
  ./build/cidx resolve                                         # relink edges, ~5 s
  ```

  Always finish with `resolve`; skipping it leaves Layer-1 (`entity_node` /
  `entity_edge`, `meta.graph_resolved_at`) stale. For a **newly added** source
  file, register it first with `./build/cidx import --db "$(pwd)/build" --name
  cpp-indexer` (no `--force` — `--force` deletes and re-indexes the whole
  component), then index just that file. For a **deleted** file, use
  `./build/cidx file rm`.
- **Upload only after the PR is merged**, never before, and only from the
  canonical checkout `/Users/husam/workspace/cpp-indexer` — absolute paths are
  baked into the DB, so a worktree-rooted database must never be pushed. After
  merging and bringing local `main` up to date, incrementally re-index the files
  the PR touched, then `./scripts/index-db.sh push`. A feature branch's index
  changes are local scratch; they die with the branch.

### Using the index inside a git worktree

Give the worktree its own copy of the database and re-point the repository at
it — do not query or mutate the canonical checkout's copy from a worktree.

```bash
WT=~/.claude/worktrees/cpp-indexer/<branch>
cd "$WT" && export INDEXER_CACHE="$WT"
/Users/husam/workspace/cpp-indexer/scripts/index-db.sh pull "$WT/index.db"
CIDX=/Users/husam/workspace/cpp-indexer/build/cidx   # or the worktree's own build
"$CIDX" repo add-clone cpp-indexer "$WT" --label <branch>
"$CIDX" repo switch    cpp-indexer <branch>
```

`repo switch` rewrites the component root to the worktree, so the incremental
recipe above then indexes *your* copy of each file. Verify with `cidx repo show
cpp-indexer` — the active clone must be the worktree. The worktree's `index.db`
is disposable: delete it with the worktree, and never `push` it.

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
