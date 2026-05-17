## Developer log — S13-pipeline-orchestration-m1-gate

date: 2026-05-17
role: developer
story: S13-pipeline-orchestration-m1-gate

### Skills loaded
- rust-conventions (loaded; project has Cargo.toml)

### Skills considered but not loaded
- implement-story — not loaded; task was pre-dispatched with explicit plan
- simplify — not loaded; no duplication sweep needed for new code
- cpp-conventions — not loaded; project is Rust only

### Commands run

1. `cargo check --all-targets --all-features` — initial check after writing pipeline/mod.rs; FAIL: missing `tempfile` (dev-dep only in lib), uuid v4 not available with current feature set
2. Fixed: replaced `tempfile::TempDir` with pid+timestamp temp dir; re-added `TuEntry` import
3. `cargo check --all-targets --all-features` → PASS
4. `cargo fmt --all -- --check` → FAIL (formatting issues in 4 files)
5. `cargo fmt --all` → applied formatting
6. `cargo fmt --all -- --check` → PASS
7. `cargo clippy --all-targets --all-features -- -D warnings` → PASS
8. `DYLD_LIBRARY_PATH=.../XcodeDefault.xctoolchain/usr/lib cargo nextest run -p cpp_indexer --test m1_exit_gate --features test-mock` → FAIL: `AstDeserialization` error in libclang
9. Root cause: compile_commands.json args include the source file path as a positional argument; libclang receives it again via `Index::parser(file_path)`, causing confusion. Also the compiler executable (clang++) must be stripped.
10. Fixed: added `filter_compiler_args()` helper in pipeline to strip argv[0] compiler name and source file path from args
11. `cargo fmt --all` → formatting applied
12. `cargo clippy --all-targets --all-features -- -D warnings` → PASS
13. `DYLD_LIBRARY_PATH=... cargo nextest run -p cpp_indexer --test m1_exit_gate --features test-mock` → PASS (1 passed, 2 skipped)
14. `DYLD_LIBRARY_PATH=... cargo nextest run -p cpp_indexer --test m1_exit_gate --features test-mock -- --ignored` → PASS (2 passed — skipped gracefully, no live servers)
15. `cargo fmt --all -- --check` → PASS (gate 1 clear)
16. `cargo clippy --all-targets --all-features -- -D warnings` → PASS (gate 2 clear)
17. `git add ... && git -c commit.gpgsign=false commit -m "S13: M1 pipeline gate"` → commit 949df66

### Deviations from plan.md
1. Plan exit criteria omit `--features test-mock`. Required because `m1_exit_gate` imports `MockSink` gated behind that feature. Flagged in implementation-notes.
2. `filter_compiler_args` added (not in plan). Required to fix `AstDeserialization` from libclang when compile_commands args include the source file as a positional argument.

### Tool failures / retries
- Pass 1: `cargo check` failed — tempfile/uuid issues. Fixed inline.
- Pass 2: `cargo fmt --check` failed — 4 files reformatted. Fixed via `cargo fmt --all`.
- Pass 3: test failed with `AstDeserialization`. Fixed with `filter_compiler_args`. All gates clear on pass 3.

### Open items (not blocking)
- Neo4j / IndraDB roundtrip tests require live servers. Tests are `#[ignore]` and skip gracefully without them. Noted in implementation-notes as follow-ups for qa-engineer.
