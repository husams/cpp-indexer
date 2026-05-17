# Developer Log — S27-phase2-decorate

## Skills loaded
- `rust-conventions` — toolchain, style, test, and error-handling conventions

## Skills considered but not loaded
- `cpp-conventions` — not applicable (project is Rust)
- `implement-story` — task was dispatched directly as developer; story context fully in plan.md
- `simplify` — no refactor needed; new code is net-new module

## Commands run + outcomes

| Command | Exit | Outcome |
|---------|------|---------|
| `git branch --show-current` | 0 | Confirmed `story/s27-phase2-decorate` |
| `cargo build` | 0 | First build: 3 errors (`CXXMethod`, `CXXThrowExpr`, `CXXForRangeStmt` — wrong variant names for clang crate v2) |
| `grep EntityKind ~/.cargo/.../clang-2.0.0/src/lib.rs` | 0 | Found correct names: `Method`, `ThrowExpr`, `ForRangeStmt` |
| `cargo build` (after fix) | 0 | Clean build |
| `cargo fmt --all -- --check` | 1 | 7 formatting diffs; applied with `cargo fmt --all` |
| `cargo fmt --all -- --check` | 0 | Gate 1 clear |
| `cargo clippy --all-targets --all-features -- -D warnings` | 0 | Gate 2 clear |
| `cargo nextest run -p cpp_indexer visit::decorate` | 104 | SIGABRT — `libclang.dylib` not on DYLD_LIBRARY_PATH |
| `find /usr /opt /Library -name "libclang*.dylib"` | 0 | Found at `/Library/Developer/CommandLineTools/usr/lib/libclang.dylib` |
| `DYLD_LIBRARY_PATH=... cargo nextest run -p cpp_indexer visit::decorate` | 0 | 5/5 PASS — Gate 3 clear |
| `git add -A && git -c commit.gpgsign=false commit -m "S27: phase2 decorate"` | 0 | Committed: `bd69e64` |

## Key design decisions

1. **Phase 2 writes separate `phase2-nodes-*.parquet` shards** (not overwriting Phase 1). `load_nodes_from_stage` updated to dedup by USR preferring `phase=2`. This avoids touching Phase 1 shard files.

2. **`exception_spec` via type display name.** The `clang` crate v2 does not expose `clang_getExceptionSpecificationType` at the safe level. Used `entity.get_type().get_display_name()` string matching as a best-effort proxy. Documented clearly in module doc.

3. **Control-flow classification by child entity-kind walk.** Phase 2 re-parses each TU and recursively visits children of Function/Method bodies, setting `has_throw`, `has_loop`, `has_return` flags. Priority order: throw > loop > return > basic.

4. **EntityKind naming correction.** The `clang` crate v2 uses `Method` (not `CXXMethod`), `ThrowExpr` (not `CXXThrowExpr`), `ForRangeStmt` (not `CXXForRangeStmt`). Verified by inspecting crate source directly.

## Deviations from plan.md

- `src/bin/index.rs` not changed (flag already wired from prior story)
- `load_nodes_from_stage` dedup logic added to pipeline/mod.rs (not called out in plan; advisor flagged as load-bearing)

## Tool failures / retries

- Build pass 1 failed on 3 incorrect `EntityKind` variant names; corrected after inspecting crate source. One retry.
- Formatter pass 1 non-zero; applied `cargo fmt --all`, then re-ran check. One retry.
- Nextest pass 1 aborted with SIGABRT (missing `libclang.dylib`); set `DYLD_LIBRARY_PATH`. One retry.
- Total: 3 issues resolved in first pass; all gates clear without exhausting the 3-pass retry limit.
