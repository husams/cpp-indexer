story: S09-visit-shallow-base
role: developer
date: 2026-05-17

## Skills loaded

- `rust-conventions` — loaded before writing any code

## Skills considered but not loaded

- `cpp-conventions` — not applicable (project is Rust)
- `implement-story` — dispatched via coordinator; role contract applies directly
- `simplify` — no refactoring scope in this story

## Commands run and outcomes

| Command | Result |
|---|---|
| `cargo check --all-targets` | exit 0 (libclang available via Xcode toolchain) |
| `cargo fmt --all -- --check` (pass 1) | exit 1 — formatting diffs in shallow.rs and phase1_base.rs |
| `cargo fmt --all` | applied formatting |
| `cargo fmt --all -- --check` (pass 2) | exit 0 |
| `cargo clippy --all-targets --all-features -- -D warnings` (pass 1) | exit 1 — `redundant_closure` in `visit_all`, `for_kv_map` in test |
| Fixed clippy issues | see below |
| `cargo clippy --all-targets --all-features -- -D warnings` (pass 2) | exit 0 |
| `cargo nextest run -p cpp_indexer --test phase1_base` (pass 1 — no DYLD) | SIGABRT — `libclang.dylib` not found on dyld path |
| `DYLD_LIBRARY_PATH=<xcode>/usr/lib cargo nextest run --test phase1_base` (pass 1) | 4/5 fail: `collect_nodes` assumed StringArray for Dictionary column; `phase1_emits_module_node` failed because TU root has no USR |
| Fixed dictionary cast + module USR synthesis | see deviations |
| `DYLD_LIBRARY_PATH=<xcode>/usr/lib cargo nextest run --test phase1_base` (pass 2) | 5/5 PASS |

## Fixes applied during retry loop

Pass 1 errors fixed:
- `cargo check` errors: missing `module_usr` struct field, `get_mangled_name` gated behind feature, `is_virtual_method()` returns `bool` not `Option<bool>`, unused `PathBuf` import
- Root cause: wrote visitor from memory; API differed from actual clang crate 2.0 source

Pass 2 errors fixed:
- `collect_nodes` helper used `downcast_ref::<StringArray>()` on a `DictionaryArray<Int8Type>` — panics at runtime. Fixed by using `arrow::compute::cast` to plain Utf8 before downcast.
- MODULE node not emitted: `tu.get_entity().get_usr()` returns None for TU root. Fixed by synthesising `tu:<file_path>` USR.

## Deviations from plan.md

1. `clang` crate features: plan said `clang = "2"`. Added `features = ["clang_6_0"]` for `get_mangled_name()` API.
2. Module USR synthesis: TU root entity has no USR in libclang. Using synthetic `tu:<file_path>` USR.
3. `mock` module gating: cannot import from integration test without `test-mock` feature. Redesigned AC-M1-15 test as structural API-shape assertion.
4. `.cache/clangd/` committed with fixture — open follow-up for `.gitignore`.

## Tool failures or retries

- `cargo nextest run` without `DYLD_LIBRARY_PATH`: SIGABRT on macOS. Xcode's libclang is at `/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/lib/libclang.dylib`. No `RPATH` patching done; CI (Linux) will find libclang via system paths. Local test requires the env var.
