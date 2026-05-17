# Developer Log — S33-daemon-rest

story: S33-daemon-rest
worktree: /Users/husam/workspace/cpp-indexer/.worktrees/s33-daemon-rest
branch: story/s33-daemon-rest
date: 2026-05-17

## Skills loaded
- rust-conventions (loaded before writing any code)

## Skills considered but not loaded
- cpp-conventions — project is Rust only
- python-conventions — no Python files
- implement-story — direct dispatch from coordinator; not routed via skill

## Orientation reads
- CHARTER.md — run_id, blackboard paths, locked-in decisions
- plan.md S33 (lines 552-565) — files to touch, exit criteria, AC coverage
- design.md — module layout (src/api/), §3 crate structure
- adr-5.md — axum 0.7, RFC-7807, bearer-token middleware spec, job state machine, reset contract
- src/lib.rs — confirmed `pub mod api {}` stub
- src/config/mod.rs — ApiConfig fields (listen, auth_token_env, job_queue_max)
- src/bin/daemon.rs — was stub; replaced with full implementation
- src/sink/mod.rs — GraphSink trait (health(), preflight())
- src/sink/mock.rs — MockSink for tests
- src/metrics.rs — existing Prometheus counter pattern
- src/observability.rs — init_tracing() exists

## Commands run + outcomes

```
cargo check --all-targets   → error: tower_http::trace (missing feature)
```
Fixed: added `features = ["trace"]` to tower-http dependency.

```
cargo check --all-targets   → error: type annotations needed for Arc<_, _>
```
Fixed: `let sink: Arc<dyn cpp_indexer::sink::GraphSink> = sink;`

```
cargo check --all-targets   → OK
cargo fmt --all             → reformatted 5 files
cargo fmt --all -- --check  → EXIT 0
cargo clippy --all-targets --all-features -- -D warnings
  → error: unused import IngestOptions (routes.rs:31)
  → error: unused variable `app` (routes.rs:415)
```
Fixed both, re-ran clippy → EXIT 0.

```
DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib \
  cargo test --lib api
  → 2 failures: post_ingest_returns_202 and post_ingest_returns_429
```
Root cause: `_rx` dropped at end of `make_app()` closed the mpsc channel;
`try_send` returned `TrySendError::Closed`. Fixed by returning
`(Router, JobReceiver)` from test helpers so caller keeps receiver alive.

```
DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib \
  cargo test --lib api
  → 26 passed, 0 failed  ✓
```

Exit gates (final pass):
```
cargo fmt --all -- --check                                  → EXIT 0  ✓
cargo clippy --all-targets --all-features -- -D warnings    → EXIT 0  ✓
DYLD_LIBRARY_PATH=... cargo nextest run -p cpp_indexer api::
  → 25 passed, 262 skipped, EXIT 0  ✓
```

## Post-commit ADR-5 compliance fixes (commit 5020e4f)

Two deviations identified by advisor review and corrected before closing:

1. **IngestSource wire format**: Changed `#[serde(tag = "type", rename_all = "snake_case")]` to
   `#[serde(untagged)]` and added `#[serde(rename = "ref")]` on `git_ref` field.
   Wire format now matches ADR-5: `{"path": "..."}` or `{"git_url": "...", "ref": "..."}`.
   Updated all 3 test request bodies in routes.rs (removed `"type": "path"` discriminator field).

2. **UUID v7**: Changed `Uuid::new_v4()` → `Uuid::now_v7()` in jobs.rs.
   Updated Cargo.toml uuid features: `["v4", "v5"]` → `["v5", "v7"]`.

Exit gates re-run after fixes:
```
cargo fmt --all -- --check                                  → EXIT 0  ✓
cargo clippy --all-targets --all-features -- -D warnings    → EXIT 0  ✓
DYLD_LIBRARY_PATH=... cargo nextest run -p cpp_indexer api::
  → 25 passed, 262 skipped, EXIT 0  ✓
```

## Deviations from plan.md (residual)
1. `GET /v1/repos` uses in-memory RepoRegistry instead of GraphSink query (sink/mod.rs is parallel-safe boundary).
2. `POST /v1/ingest` git_url → 501 Not Implemented (S36 scope).
3. Worker loop in daemon.rs is placeholder (mark done immediately); pipeline::run wired in S36/S37.

## Tool failures or retries
- cargo check needed 2 passes (tower-http feature, then type annotation)
- cargo clippy needed 2 passes (2 lint errors)
- cargo test needed 2 passes (mpsc receiver lifetime bug)
- All resolved within 3 retry passes

## Pre-existing infra issue noted
`libclang.dylib` not in dyld path on macOS — all test binaries that link libclang fail with SIGABRT unless `DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib` is set. This is not specific to S33; affects all integration tests (autodetect, phase1_base, etc.). CI must set this env var on macOS builders.
