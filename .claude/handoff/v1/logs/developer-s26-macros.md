# Developer log — S26-macros

task-slug: s26-macros
worktree: /Users/husam/workspace/cpp-indexer/.worktrees/s26-macros
branch: story/s26-macros
commit: f40e3cf

## Skills loaded
- rust-conventions

## Skills considered but not loaded
- cpp-conventions: not applicable (Rust project)
- implement-story: plan already provided, not needed
- simplify: post-implementation review, not invoked for this story

## Orientation reads
- CHARTER.md — project paths, worktree convention, exit gate rules
- plan.md lines 446-460 — S26 story spec
- design.md §3 visit/ module layout, §Phase 1, §5.5 testing
- requirements.md AC-M5-1..4
- src/schema/nodes.rs — existing NodeKind enum
- src/schema/edges.rs — existing EdgeKind enum
- src/schema/version.rs — SCHEMA_VERSION = 3
- src/schema/arrow.rs — Arrow round-trip (uses NodeKind::all()/EdgeKind::all(); no hardcoded lists to update beyond enum changes)
- src/visit/shallow.rs — Collector struct and visit dispatch
- src/visit/cursor_map.rs — entity_kind_to_node_kind (confirmed MACRO is intentionally absent)
- src/visit/mod.rs — existing module list
- clang-rs crate source — confirmed EntityKind::MacroDefinition(501), EntityKind::MacroExpansion(502), is_builtin_macro(), is_function_like_macro(), get_reference(), SourceLocation::get_spelling_location()/get_expansion_location()

## Advisor call
Called advisor before writing code. Key guidance followed:
1. Bump SCHEMA_VERSION in version.rs in the same PR → done (3→4)
2. arrow.rs uses NodeKind::all()/EdgeKind::all() dynamically → no direct edit required
3. Do NOT extend cursor_map.rs; dispatch macro kinds before entity_kind_to_node_kind → done
4. Top-level filter: compare spelling location file == expansion location file → implemented in is_top_level_expansion()
5. src_usr for EXPANDS_TO: module USR fallback → implemented

## Commands run + outcomes

| Command | Exit code | Notes |
|---|---|---|
| cargo check --all-targets --all-features | 0 | Initial check; 1 warning (dead_code on parse_macro_attrs) |
| cargo fmt --all -- --check | 1 | BUILD_FAIL: 2 formatting issues in macros.rs and shallow.rs |
| (fix formatting) | — | Expanded if-else in macros.rs; collapsed chain in shallow.rs |
| cargo fmt --all -- --check | 0 | PASS |
| cargo clippy --all-targets --all-features -- -D warnings | 0 | PASS (0 warnings) |
| cargo nextest run -p cpp_indexer visit::macros (no env) | 104/SIGABRT | dyld: libclang.dylib not found on rpath |
| LIBCLANG_PATH=... DYLD_LIBRARY_PATH=... cargo nextest run -p cpp_indexer visit::macros | 0 | 9/9 PASS |
| git add -A && git -c commit.gpgsign=false commit -m "S26: macros" | 0 | commit f40e3cf |

## Deviations from plan.md
1. cursor_map.rs NOT touched — design already excludes macro kinds from cursor_map (comment: "MACRO (S22) is not mapped here"). Dispatch added in Collector::visit directly.
2. SCHEMA_VERSION bumped to 4 — ADR-9 requires this; plan.md did not explicitly call it out for S26 but the CHARTER invariant mandates it.
3. Test invocation requires LIBCLANG_PATH/DYLD_LIBRARY_PATH on this macOS host.

## Tool failures / retries
- cargo fmt check: failed pass 1 (2 formatting issues). Fixed + reran → pass.
- cargo nextest run (no env): SIGABRT due to missing libclang.dylib. Added LIBCLANG_PATH env → pass.
- Total passes: 2 (formatter retry counted as pass 1 → pass 2 cleared all gates within 2 passes).
