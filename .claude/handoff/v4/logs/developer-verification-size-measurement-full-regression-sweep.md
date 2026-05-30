# Developer Log: Story 6 — Verification: size measurement + full regression sweep

## Skills loaded
- rust-conventions (loaded at start per task instruction)
- graph-symbol-ids (skill invoked per dispatch)

## Skills considered but not loaded
- implement-story: task is verification/regression sweep, not a new feature story
- cpp-conventions: project is Rust

## Precondition check
- `src/resolve/symbol_map.rs` — EXISTS (Story 1 landed)
- `SCHEMA_VERSION == 6` — CONFIRMED (src/schema/version.rs line 23)
- `NodeRecord` has `symbol_id`/`file_id` — CONFIRMED
- `EdgeRecord` has `src_id`/`dst_id`/`dst_repo_name` — CONFIRMED

## Commands run + outcomes

1. `git stash` + `cargo test --lib -- stage::writer::tests::write_and_read_back_row_counts` → PASS (pre-feature)
2. `git stash pop` → restored Stories 1-5 in-tree
3. `cargo test --lib` (post-feature) → FAIL: `stage::writer::tests::write_and_read_back_row_counts` (left:25, right:23)
4. Determined regression: Story 3 added 2 node columns + 3 edge columns to arrow.rs without updating writer.rs assertion
5. Fixed `src/stage/writer.rs`: node assertion 23→25, edge assertion 11→14
6. `cargo fmt --all -- --check` → PASS
7. `cargo clippy --all-targets --all-features -- -D warnings` → PASS (0 warnings)
8. `cargo test --lib` → 391 passed, 0 failed, 1 ignored
9. `cargo test --tests` → all 26 targets pass, 0 failed

## Deviations from plan
- `src/stage/writer.rs` was not in Story 6's files-to-touch list (it wasn't in Story 3's either), but fixing the regression is within scope: S7-SC-15 requires "no NEW failures vs pre-feature baseline"
- `tests/integration/mod.rs` listed in files-to-touch but required no edits (placeholder file)

## Tool failures or retries
- `cargo fmt --all -- --check` failed on first attempt (formatting diff in symbol_id_size.rs); resolved with `cargo fmt --all`, then rechecked
- No other retries

## Summary
All three gates pass. Regression in `write_and_read_back_row_counts` was identified, root-caused to Story 3's column additions, and fixed. `tests/symbol_id_size.rs` and Cargo.toml entry created.
