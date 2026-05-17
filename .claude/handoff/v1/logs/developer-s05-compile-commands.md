story: S05-compile-commands-parser
date: 2026-05-17
worktree: /Users/husam/workspace/cpp-indexer/.worktrees/s05-compile-commands
branch: story/s05-compile-commands

## Skills loaded
- rust-conventions (loaded; project has Cargo.toml + *.rs files)

## Skills considered but not loaded
- cpp-conventions — not applicable (Rust project)
- python-conventions — not applicable
- typescript-conventions — not applicable
- implement-story — not needed; dispatch provides explicit plan.md story reference

## Commands run

| Command | Exit | Notes |
|---|---|---|
| `cargo fmt --all` | 0 | Auto-formatted two diffs in compile_commands.rs |
| `cargo fmt --all -- --check` | 0 | Gate 1 passes |
| `cargo clippy --all-targets --all-features -- -D warnings` | 0 | Gate 2 passes; full cold build ~80s |
| `cargo nextest run -p cpp_indexer bootstrap::compile_commands` | 0 | Gate 3: 6/6 passed |
| `git add ... && git commit` | 0 | Commit 2ccdb12 on story/s05-compile-commands |

## Implementation decisions

- Handled both `"arguments"` (preferred) and `"command"` (shell-split) forms of the JSON Compilation Database spec. Neither was explicit in plan.md but real-world cc.json files use both.
- `resolve_file` best-effort canonicalises: uses `PathBuf::canonicalize()` with a fallback to the joined path when the file doesn't exist on disk (safe for unit tests using synthetic paths, and for files only present at index-time on the user's machine).
- Dedup key is `blake3(canonical_file || 0x00 || args.join(0x00))` exactly per design.md §Phase 0.
- No scope filter (AC-M1-12/13) — that is S06's responsibility per plan.md wave decomposition.
- `TuEntry` does not derive `serde::Serialize`; `blake3::Hash` has no built-in serde support and no downstream story in this wave needs serialization.

## Deviations from plan.md

None.

## Tool failures / retries

- formatter auto-fixed two formatting issues on first `cargo fmt --all` run; `--check` passed on second run. Not a failure — standard workflow.
- clippy waited on build-dir lock from background first run; resolved automatically.

## Open items

None.
