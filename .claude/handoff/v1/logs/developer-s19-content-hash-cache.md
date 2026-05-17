## Developer log — S19: content-hash-cache

### Skills loaded
- rust-conventions

### Skills considered but not loaded
- cpp-conventions: not applicable (Rust project)
- python-conventions: not applicable
- implement-story: task was dispatched directly, not via implement-story pattern

### Commands run

1. Read CHARTER.md, plan.md S19, design.md — orientation
2. Read src/stage/manifest.rs, src/pipeline/mod.rs, Cargo.toml — existing code state
3. Read src/visit/shallow.rs, src/stage/writer.rs, tests/integration/phase1_base.rs — context
4. `cargo check --all-targets --all-features` — clean (2m 09s, first cold build)
5. `cargo clippy --all-targets --all-features -- -D warnings` — one lint fixed (collapsible_if)
6. `cargo fmt --all -- --check` — clean
7. `DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo nextest run -p cpp_indexer --test incremental_cache --features test-mock` — first attempt: AstDeserialization errors in all 4 tests
8. Diagnosed: macOS symlink `/var/folders` → `/private/var/folders` caused path mismatch in `filter_compiler_args`; `write_compile_commands` fixed to canonicalize paths
9. Re-ran tests — 4/4 passed
10. `git add -A && git -c commit.gpgsign=false commit -m "S19: content hash cache"` — committed on branch story/s19-content-hash-cache

### Deviations from plan.md
- plan.md exit criterion does not include `--features test-mock`; added it because `incremental_cache` requires the `test-mock` feature (same pattern as m1_exit_gate, m2_exit_gate).

### Tool failures / retries
- First test run failed with AstDeserialization from libclang (macOS symlink path issue in test helpers). Fixed by canonicalizing temp dir paths in `write_compile_commands` and `make_run_opts`. Second run passed.
- `cargo check` took ~2m due to cold dependency build (rocksdb, libgit2, openssl).

### Named signals at close
- BUILD_FAIL: cleared (cargo fmt --check passes)
- LINT_FAIL: cleared (clippy passes)
- TEST_FAIL: cleared (4/4 integration tests pass)
