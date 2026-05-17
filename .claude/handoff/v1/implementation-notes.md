story: S09-visit-shallow-base
date: 2026-05-17

## Files changed

- `src/visit/mod.rs` — new, exposes `cursor_map` and `shallow` submodules
- `src/visit/cursor_map.rs` — new, maps `clang::EntityKind` to `NodeKind` for M1 base set
- `src/visit/shallow.rs` — new, Phase 1 visitor (`visit_tu`, `VisitOptions`, `Collector`, `visit_all`)
- `src/lib.rs` — changed `pub mod visit {}` stub to `pub mod visit;`
- `Cargo.toml` — added `clang_6_0` feature to clang dep; added `walkdir = "2"` dev dep; added `[[test]] name = "phase1_base"`
- `tests/fixtures/m1_5file/` — new: `base.h`, `shapes.h`, `shapes.cpp`, `utils.h`, `utils.cpp`, `broken.cpp`, `compile_commands.json`
- `tests/integration/phase1_base.rs` — new, 5 integration tests for AC-M1-14/15/16/17

## Tests added/run

Command: `DYLD_LIBRARY_PATH=/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/lib cargo nextest run -p cpp_indexer --test phase1_base`

Result: 5/5 PASS
- `phase1_api_does_not_accept_graph_sink` — AC-M1-15
- `phase1_emits_expected_node_kinds` — AC-M1-14/17 (CLASS, METHOD, FIELD)
- `phase1_emits_module_node` — AC-M1-14 (MODULE kind)
- `phase1_parse_error_produces_partial_and_continues` — AC-M1-16
- `phase1_emits_global_variable_from_utils` — AC-M1-14 (GLOBAL_VARIABLE)

All exit gates clear:
1. `cargo fmt --all -- --check` — exit 0
2. `cargo clippy --all-targets --all-features -- -D warnings` — exit 0
3. `cargo nextest run -p cpp_indexer --test phase1_base` — 5/5 PASS exit 0

## Deviations from plan

1. `clang` crate features: plan specified `clang = "2"` without features. Added `features = ["clang_6_0"]` to unlock `get_mangled_name()`. Additive, non-breaking.
2. Module USR synthesis: TU root entity does not return a USR from libclang. Synthesised as `tu:<file_path>` — stable and unique per TU.
3. `walkdir` dev-dependency: added for integration test helper. Dev-only, no production impact.
4. `DYLD_LIBRARY_PATH` on macOS: needed at test runtime. CI (Linux) uses system libclang path automatically.
5. `mock` module gating: `#[cfg(any(test, feature = "test-mock"))]` does not fire for external test binaries. The zero-sink-calls assertion was redesigned as a structural/API-shape test.

## Follow-ups

- `visit_all()` helper: `total_nodes` counter is a TU count placeholder; replace with actual node counts in S13. Tag: sr-dev.
- `.cache/clangd/` committed with fixture; add to `.gitignore` before W5 merge. Tag: sr-dev.
- S17 (parallel): `visit_all` creates one worker dir per TU; S17 will need to adapt per ADR-7 rayon design.

## References

- plan.md §S09
- design.md §Phase 1
- ADR-7 (rayon deferred to S17)
