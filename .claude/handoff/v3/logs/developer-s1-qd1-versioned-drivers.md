# Developer session log — s1-qd1-versioned-drivers

## Skills loaded
- `rust-conventions` — loaded before writing any Rust code

## Skills considered but not loaded
- `implement-story` — task was narrowly scoped (single predicate extension); skill not needed
- `simplify` — no refactoring required; new helpers are minimal and self-contained

## Commands run

| Command | Outcome |
|---------|---------|
| `cargo fmt --all` (auto-format) | Modified `compile_commands.rs` (long assert_eq line wrapped) |
| `cargo fmt --all -- --check` (pass 1) | exit 1 (long line) |
| `cargo fmt --all` | exit 0 |
| `cargo fmt --all -- --check` (pass 2) | exit 0 |
| `cargo clippy --workspace --all-targets -- -D warnings` | exit 0 |
| `cargo test --workspace` | exit 101 — only `schema_txt_contains_all_promoted_fields` failed |
| `git stash && cargo test --test schema_drift schema_txt_contains_all_promoted_fields` | exit 101 — confirmed pre-existing |
| `git stash pop` | exit 0 |

## Changes made

### `src/bootstrap/compile_commands.rs`

Added two private helpers after the existing `is_driver_basename`:

- `is_versioned_driver(stem: &str) -> bool` — iterates `KNOWN_STEMS`, strips known prefix + hyphen, delegates version part to `is_numeric_version`.
- `is_numeric_version(s: &str) -> bool` — splits on `.`, rejects empty parts or non-ASCII-digit bytes. Matches `[0-9]+(\.[0-9]+)*`.

Extended `is_driver_basename` to call `is_versioned_driver(stem)` as a third condition (literal match → suffix match → versioned match).

Updated unit test table:
- Renamed `sanitize_versioned_driver_clang_18_is_kept` → `sanitize_versioned_driver_clang_18_is_stripped` (assert changed from kept → stripped).
- Added: `sanitize_versioned_driver_gpp_12_is_stripped`, `sanitize_versioned_driver_gcc_10_is_stripped`, `sanitize_versioned_driver_clangpp_18_1_is_stripped`, `sanitize_clang_tidy_is_not_stripped`, `sanitize_clang_format_is_not_stripped`.

### `tests/integration/qa_boundary.rs`

Removed `#[ignore = "QD-1: ...]` attribute from:
- `qa_versioned_driver_clang_18_is_stripped`
- `qa_versioned_driver_gpp_12_is_stripped`

Updated section doc comment and top-level module doc to reflect QD-1 resolved.

## Exit gate results (pass 2 — all clear)

- BUILD: `cargo fmt --all -- --check` → 0
- LINT: `cargo clippy --workspace --all-targets -- -D warnings` → 0
- TEST: `cargo test --workspace` → only pre-existing `schema_txt_contains_all_promoted_fields` fails

QD-1 integration tests status:
- `qa_versioned_driver_clang_18_is_stripped` → PASS (no longer ignored)
- `qa_versioned_driver_gpp_12_is_stripped` → PASS (no longer ignored)
- `qa_clang_tidy_is_not_stripped` → PASS (guard test, was already passing)

## Deviations from plan

None.

## Pre-existing failures

- `schema_drift::schema_txt_contains_all_promoted_fields` — fails on unmodified main HEAD. Not caused by this story.
