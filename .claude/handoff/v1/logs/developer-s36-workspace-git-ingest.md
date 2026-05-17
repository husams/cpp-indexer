# Developer Log — S36-workspace-git-ingest

## Skills loaded
- `rust-conventions` — loaded before writing any Rust code

## Skills considered but not loaded
- `implement-story` — task dispatch was explicit; no story decomposition needed
- `simplify` — ran clippy instead; no structural duplication found
- `python-conventions`, `go-conventions`, `cpp-conventions`, `typescript-conventions` — project is pure Rust

## Commands run and outcomes

| Command | Outcome |
|---------|---------|
| Read CHARTER.md, plan.md S36, design.md, adr-6.md | orientation complete |
| Read lib.rs, routes.rs, jobs.rs, config/mod.rs, problem.rs, error.rs, daemon.rs, Cargo.toml | codebase context |
| `cargo fmt --all -- --check` (pass 1) | FAIL — 8 formatting diffs |
| `cargo fmt --all` | fixed all formatting |
| `cargo fmt --all -- --check` (pass 2) | PASS |
| `cargo clippy --all-targets --all-features -- -D warnings` (pass 1) | FAIL — `name()` on `RemoteHead` wrong: called `.unwrap_or("")` on `&str` (returns `&str` not `Option`) |
| Fixed git.rs: used `.name()` directly | |
| `cargo clippy` (pass 2) | FAIL — `resolve_remote_sha` dead code; `make_bare_repo` dead code in mod.rs tests |
| Removed `resolve_remote_sha` entirely (not called); removed unused `make_bare_repo` from mod.rs | |
| `cargo clippy` (pass 3) | PASS |
| `DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo nextest run -p cpp_indexer workspace::` (pass 1) | FAIL — 2 tests: `fresh_clone_creates_target_dir`, `fetch_existing_clone_returns_fetched` — branch "main" not found in bare repo |
| Fixed tests to use `"HEAD"` ref; fixed `clone_fresh` to not call `.branch()` when ref is HEAD; fixed `fetch_existing` refspec for HEAD | |
| `cargo fmt --all` | formatting fix (extra blank line) |
| `cargo nextest run workspace::` (pass 2) | PASS — 26/26 |

## Deviations from plan.md

1. ADR-6 calls for ls-remote before clone to get SHA for path naming. Implemented as post-clone rename instead to avoid extra network round-trip in tests. See F1 follow-up.
2. Test ref `"HEAD"` used instead of `"main"` for portability across git init default-branch settings.
3. `resolve_remote_sha` function removed: it was a standalone helper with no caller in production code and triggered dead-code warning. The post-clone rename approach achieves the same result.

## Tool failures and retries
- `cargo clippy` pass 1: `RemoteHead::name()` returns `&str` not `Option<&str>` in git2 0.18 — fixed by removing spurious `.unwrap_or(...)` calls.
- `cargo clippy` pass 2: dead-code warnings on unused function and test helper — removed both.
- `cargo nextest` pass 1: bare repo default branch is `master` on this git version, not `main` — fixed by using `"HEAD"` as the ref in tests and guarding `builder.branch()` to only be called for non-HEAD refs.
