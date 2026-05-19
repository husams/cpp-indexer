run_id: tu-parse-fail-v3
story: S1 — Sanitise libclang args
story-slug: s1-sanitise-args
stage: developer

## Skills loaded
- rust-conventions (loaded before any code writing)
- implement-story (not loaded — task scope is narrow, plan.md fully specifies files and algorithm)

## Commands run

| Command | Outcome |
|---|---|
| `rg "\.args" src/bootstrap/ src/pipeline/` | Audit: two pipeline consumers (`parallel.rs`, `mod.rs`) read sanitised args only; no raw-form dependency |
| `cargo fmt --all -- --check` | Pass (exit 0) |
| `cargo clippy --workspace --all-targets -- -D warnings` | Pass (exit 0) |
| `cargo test --workspace` | 332/333 pass; 1 pre-existing failure (`schema_drift::schema_txt_contains_all_promoted_fields`, confirmed pre-existing on unmodified HEAD via `git stash` + re-run) |
| `cargo test -p cpp_indexer --lib bootstrap::compile_commands::tests` | Pass — 17/17 (5 existing + 12 new AC-2 tests) |

## Deviations from plan

1. **`parse_valid_arguments_form` assertion updated.** The plan states "all existing tests MUST continue to pass". After sanitisation, `entries[0].args[0]` changes from `"/usr/bin/c++"` to `"-std=c++17"`. Updated assertion to reflect sanitised form (`assert_eq!(entries[0].args, vec!["-std=c++17"])`). This is a necessary correction — the test was asserting the pre-sanitisation raw form. Flagged in implementation-notes.md.

2. **`clang-18` test asserts KEPT, not stripped.** ADR-1's predicate does not match versioned drivers (`clang-18` stem ends in `-18`, not `-gcc`/`-g++`/`-clang`/`-clang++`). Implemented ADR-1 verbatim. Test `sanitize_versioned_driver_clang_18_is_kept` documents this gap. Follow-up tagged sr-dev in implementation-notes.md.

3. **Pre-existing `schema_drift` failure.** `schema_txt_contains_all_promoted_fields` fails on unmodified HEAD. Not introduced by S1. Not in scope to fix. Noted here for QA traceability.

## Tool failures / retries
None. All three gates passed on first run (excluding pre-existing schema_drift failure).

## References
- plan.md §S1
- adr-1.md (Status: accepted)
- scenarios.md Feature S1
- src/bootstrap/compile_commands.rs
