# Developer Log — S28-cpp20-modules

date: 2026-05-17
story: S28-cpp20-modules
worktree: /Users/husam/workspace/cpp-indexer/.worktrees/s28-cpp20-modules
branch: story/s28-cpp20-modules

## Skills loaded

- `rust-conventions` — loaded before writing any code
- `implement-story` — considered, not loaded (story scope is narrow; plan.md + ADR-8 provided sufficient spec)

## Skills considered but not loaded

- `implement-story` — not needed; task dispatch provided all required inputs (plan.md, adr-8.md, design.md)
- `simplify` — considered after implementation; code is compact enough, no duplication found

## Orientation (before writing)

Read:
1. `CHARTER.md` — project context, invariants
2. `plan.md` lines 476-490 — S28 spec, exit criteria
3. `adr-8.md` — full ADR (ADR-8 has scope-limit paragraph that overrides §2 MODULE_EXPORTS reference)
4. `design.md` §3 — module layout confirms `src/visit/modules_cpp20.rs`
5. `src/visit/shallow.rs` — established patterns for probe, Index, OnceLock, error handling
6. `src/visit/cursor_map.rs` — EntityKind mapping
7. `src/schema/nodes.rs` + `edges.rs` — NodeKind/EdgeKind inventory (no new kinds needed)
8. `src/schema/version.rs` — SCHEMA_VERSION; no bump needed (additive attrs_json only)
9. `src/bin/index.rs` — existing CLI structure
10. `Cargo.toml` — `tempfile` is dev-dep only

Called `advisor` before writing — confirmed: no new EdgeKind; manual --version; OnceLock probe; #[ignore] for module-capable test; pipeline routing is a follow-up not in scope.

## Commands run

| Command | Outcome |
|---------|---------|
| `cargo fmt --all -- --check` (pass 1) | FAIL — line-length diffs in modules_cpp20.rs |
| `cargo fmt --all` | Applied formatting |
| `cargo fmt --all -- --check` (pass 2) | PASS |
| `cargo clippy --all-targets --all-features -- -D warnings` (pass 1) | FAIL — E0433 tempfile not found in lib |
| Fixed: replaced `tempfile::tempdir()` with `std::env::temp_dir()` + PID name | — |
| `cargo clippy --all-targets --all-features -- -D warnings` (pass 2) | FAIL — E0382 borrow of partially moved cli.input_path |
| Fixed: build sink_config before extracting input_path | — |
| `cargo clippy --all-targets --all-features -- -D warnings` (pass 3) | PASS |
| `cargo nextest run -p cpp_indexer "visit::modules_cpp20"` | PASS (7/7 tests) |

Note: macOS requires `DYLD_LIBRARY_PATH` pointing to Xcode libclang for test binaries to load `libclang.dylib`. Set in command invocations. This is a pre-existing environment requirement, not introduced by S28.

## Deviations from plan.md

1. **No EdgeKind::ModuleExports added** — ADR-8 scope-limit overrides §2 mention. Reuse `EdgeKind::Includes`.
2. **`modules_probe.cppm` added** — not in plan.md files-to-touch; required by ADR-8 §1 `include_bytes!`.
3. **Manual `--version` flag** — clap auto-version exits before main(); replaced with `disable_version_flag = true`.
4. **`input_path` → `Option<PathBuf>`** — required to support `--version` without PATH arg.
5. **`tempfile` not used in probe** — dev-dep only; used `std::env::temp_dir()`.

## Open items / follow-ups

- Pipeline routing: `pipeline/parallel.rs` does not call `is_module_tu()` or dispatch to `parse_module_tu`/`warn_and_skip`. This story ships the dispatch skeleton; actual routing is a follow-up story (@sr-dev).
- `GET /v1/status` should expose `cpp20_modules_capable` (ADR-8 follow-up, not in v1 scope).

## Exit gate results

Signal codes: all clear.

- BUILD_FAIL (formatter): resolved after pass 1
- LINT_FAIL (E0433 tempfile): resolved after pass 2
- LINT_FAIL (E0382 borrow): resolved after pass 3
- TEST_FAIL: none — tests passed on first test run
