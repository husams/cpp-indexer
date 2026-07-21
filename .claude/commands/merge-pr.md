---
description: Safely merge a cidx PR into main — integrate main, resolve conflicts, gate, regenerate index.db, merge, then update local main
argument-hint: <PR-number>
allowed-tools: Bash(git:*), Bash(gh:*), Bash(cmake:*), Bash(ctest:*), Bash(sqlite3:*), Bash(python3:*), Read, Edit, Write
---

Merge PR **#$1** into `main` for the `cidx` repo, following the project's merge discipline exactly. Do not skip any step; if a step fails, stop and report — never force a merge past a red gate or an unresolved conflict.

## 1. Assess
- `gh pr view $1 --repo husams/cpp-indexer --json state,mergeable,mergeStateStatus,headRefName,baseRefName`.
- If already `MERGED`, say so and stop. If `mergeable` is `UNKNOWN`, wait a few seconds and re-check.
- Identify the PR's worktree/branch and work there (a git worktree may exist under `~/workspace/`); never operate in the wrong clone.

## 2. Integrate main & resolve conflicts (only if not CLEAN)
- `git fetch origin main` then `git merge origin/main --no-edit` on the PR branch.
- **Schema collision check (critical):** compare `kSchemaVersion` in `src/storage/storage.hpp` and `SCHEMA_VERSION` in `python/indexer/storage.py` against main's. If both sides bumped to the same version, bump this branch one higher in BOTH files and reconcile the migrations.
- Typical conflicts:
  - `CMakeLists.txt` / `tests/CMakeLists.txt` — both sides usually append source/test entries to a list: **keep both**.
  - `index.db` — binary; do NOT hand-merge. Leave it conflicted and regenerate it in step 4.
- After resolving code, `git grep -n '^<<<<<<<\|^>>>>>>>\|^=======$' -- ':!index.db'` must return nothing.

## 3. Build + baseline gates
- `cmake . && cmake --build . -j8` in `build/` — must compile clean.
- `ctest -L default` and `ctest -L clang` — both must be 100% green. Investigate EVERY failure; never dismiss one as pre-existing without proof. (`python3 -m pytest -q` in `python/` if storage/read-query changed; the 2 known `test_repository.py` worktree-name failures are pre-existing.)

## 4. Regenerate index.db (mandatory after any src/ or python/indexer change)
- Into a temp cache (indexing takes >2 min — run it backgrounded):
  `INDEXER_CACHE=<tmp> build/cidx import --db build/compile_commands.json --name cpp-indexer` → `build/cidx index` → `build/cidx resolve` (all three passes; `resolve` populates the entity graph).
- Verify: schema_version == the branch's version, `SELECT path FROM component;` == `.`, index log shows `0 failed`.
- `cp <tmp>/index.db index.db` and `git add index.db`.

## 5. Commit merge & push
- `git commit --no-edit` (merge commit) and `git push`.
- Re-check `gh pr view $1 --json mergeable,mergeStateStatus` — must be `MERGEABLE` / `CLEAN`.

## 6. Merge the PR
- `gh pr merge $1 --repo husams/cpp-indexer --merge --delete-branch`.
- Confirm: `gh pr view $1 --json state,mergedAt,mergeCommit` shows `MERGED`. Never claim merged without this check.

## 7. Update local main (MANDATORY — per CLAUDE.md)
- `git fetch origin main:main` (fast-forwards local `main` without switching branches), or `git pull --ff-only` if `main` is checked out.
- Verify `git rev-parse main` == `git rev-parse origin/main`.

## 8. Report
- One concise summary: merge commit SHA, which conflicts were resolved and how, gate results, index.db counts, and that local `main` is up to date.
