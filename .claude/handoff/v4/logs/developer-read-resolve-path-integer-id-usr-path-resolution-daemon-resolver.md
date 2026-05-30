# Developer Log: Story 4 — Read/resolve path (graph-symbol-ids)

## Skills loaded
- `rust-conventions` — loaded at start per language convention rule
- `implement-story` — considered but not loaded; task dispatch specified the story directly

## Skills considered but not loaded
- `implement-story`: task dispatch already provided plan/design/scenarios; skill would be redundant
- `cognee-memory`: no semantic search needed; all context in handoff files

## Commands run + outcomes

1. Read plan.md, design.md, scenarios.md — orientation (all 3 files)
2. `ls src/resolve/ src/api/` — found no symbol_map.rs or id_resolver.rs pre-existing
3. `grep -r "symbol_map|SymbolAllocator|id_resolver"` — confirmed Stories 1-3 not implemented
4. `cat Cargo.toml` — confirmed no rusqlite/lru dep
5. Added `rusqlite = { version = "0.31", features = ["bundled"] }` and `lru = "0.12"` to Cargo.toml
6. Wrote `src/resolve/symbol_map.rs` (SymbolAllocator + ReadOnlyResolver + BiCache)
7. Wrote `src/resolve/id_resolver.rs` (IdResolver + build_resolver)
8. Updated `src/resolve/mod.rs` — added two pub mod entries
9. Updated `src/api/routes.rs` — added S5-SC-02 test
10. Replaced `tests/integration/mod.rs` placeholder with round-trip tests
11. Added `[[test]] name = "integration"` to Cargo.toml
12. `cargo fmt --all -- --check` → diff found; ran `cargo fmt --all` → clean
13. `cargo clippy --all-targets --all-features -- -D warnings` → LINT_FAIL: dead_code `write_db`
14. Removed unused `write_db` helper from id_resolver.rs tests
15. `cargo clippy` re-run → exit 0
16. `DYLD_LIBRARY_PATH=... cargo test --lib resolve::id_resolver` → 6/6 ok
17. `DYLD_LIBRARY_PATH=... cargo test --lib resolve::symbol_map` → 15/15 ok
18. `DYLD_LIBRARY_PATH=... cargo test --lib api::routes` → 10/10 ok
19. `DYLD_LIBRARY_PATH=... cargo test --test integration --features=test-mock` → 3/3 ok

## Deviations from plan.md
- Stories 1-3 not pre-implemented; implemented symbol_map.rs (Story 1 spec) as prerequisite.
  See implementation-notes.md for detail.

## Tool failures / retries
- Pass 1 formatter: diff → fixed with `cargo fmt --all`
- Pass 1 clippy: `dead_code` on `write_db` → removed helper
- No test failures; all gates cleared on pass 2 (single retry for fmt+lint, tests first-run clean)

---

# Developer Log: Story 4 — Second pass (this session)

## Context
Previous session had already implemented `symbol_map.rs` (Story 1). This session added `id_resolver.rs` and tests for Story 4.

## Skills loaded
- `rust-conventions` — loaded at start

## Skills considered but not loaded
- `implement-story` — not needed; plan was explicit
- `cognee-memory` — not needed

## Commands run + outcomes

1. Read plan.md, design.md, scenarios.md, CHARTER.md
2. Read `src/resolve/mod.rs`, `src/resolve/symbol_map.rs` — confirmed Stories 1 already done
3. Read `src/api/routes.rs`, `tests/integration/symbol_id_integration.rs`
4. Checked Cargo.toml `[[test]]` — confirmed `integration` → `symbol_id_integration.rs` with `required-features=test-mock`
5. `cargo test --test integration -- --list` → feature gate error; noted need for `--features test-mock`
6. Read `cross_repo.rs` (orientation: stage_dirs path convention)
7. Called `advisor` — confirmed approach, noted handle-caching complexity, confirmed routes S5-SC-02 is N/A
8. Wrote `src/resolve/id_resolver.rs`
9. Updated `src/resolve/mod.rs`
10. Added S5-SC-02 test to `src/api/routes.rs`
11. Added S7-SC-13/14 round-trip tests to `tests/integration/symbol_id_integration.rs`
12. `cargo fmt --all -- --check` → FAIL (formatting) → `cargo fmt --all` → OK
13. `cargo clippy --all-targets --all-features -- -D warnings` → FAIL (missing ReadOnlyHandle import, unused `tmp`) → fixed → OK
14. `cargo test --lib -- resolve::id_resolver resolve::symbol_map api::routes` → 32 passed
15. `cargo test --test integration --features test-mock` → 4 passed
16. All exit gates clear

## Deviations from plan.md
- Exit gate `cargo test --test integration` needs `--features test-mock` (Cargo.toml required-features). Tagged sr-dev.
- `IdResolver` uses per-call `open_readonly` (no handle-level caching struct); BiLru inside ReadOnlyHandle handles in-call amortisation.
- S5-SC-02 asserted N/A via test (no integer ids in RepoInfo).

## Tool failures / retries
- Pass 1 fmt: diff → fixed → pass 2 OK
- Pass 1 clippy: 2 errors → fixed → pass 2 OK
- Tests: first-run clean (4+32 passed)
- All named signals clear after 2 passes
