# Developer Log — S12-sink-indradb

## Skills loaded
- `rust-conventions` — loaded before writing any code

## Skills considered but not loaded
- `implement-story` — skipped; dispatch payload was specific enough that the skill's story-parsing overhead wasn't needed
- `cpp-conventions` — not applicable (Rust project)
- `simplify` — not loaded; implementation is new code with no pre-existing duplication to reduce

## Orientation steps

1. Read `CHARTER.md`, `plan.md` S12 (lines 233–247), `adr-2.md`, `src/sink/mod.rs`.
2. Read `src/sink/factory.rs`, `src/sink/mock.rs` to understand existing patterns.
3. Read `src/config/mod.rs`, `src/config/env.rs`, `src/error.rs`, `src/schema/nodes.rs`, `src/schema/edges.rs`.
4. Read indradb-proto-5.0.0 `client.rs` + indradb-lib-5.0.0 `queries.rs`, `properties.rs`, `json.rs`, `bulk_insert.rs` to understand the actual API.
5. Called `advisor` for design guidance before writing code.
6. Found that `uuid` crate needed `v5` feature (not in Cargo.toml), added it.
7. Found that `tonic` was transitive; added direct dep for `tonic::Code`.

## Commands run + outcomes

| Command | Exit | Notes |
|---|---|---|
| `cargo check --lib` (1st attempt) | 1 | `hostname` crate not in deps; `blocking_lock` in async; wrong `Json.into_inner()` |
| `cargo check --lib` (2nd attempt) | 1 | `tonic` not in scope; `VertexWithPropertyValueQuery.equal` field doesn't exist; `SpecificVertexQuery.properties()` trait method; `Json.as_ref()` wrong |
| `cargo check --lib` (3rd attempt) | 1 | `Json.as_ref()` - `AsRef` not implemented; needed `&*named.value` |
| `cargo check --lib` (4th) | 0 | |
| `cargo check --all-targets --all-features` | 0 | |
| `cargo fmt --all -- --check` | 1 | Format diffs in `indradb.rs` and `tests/sink_indradb.rs` |
| `cargo fmt --all` | 0 | Applied |
| `cargo fmt --all -- --check` | 0 | Gate 1 PASS |
| `cargo clippy --all-targets --all-features -- -D warnings` | 0 | Gate 2 PASS |
| `cargo nextest run -p cpp_indexer sink::indradb` (no DYLD) | 104 | `libclang.dylib` not in rpath on macOS |
| `DYLD_LIBRARY_PATH=... cargo nextest run -p cpp_indexer sink::indradb` | 0 | 16/16 PASS — Gate 3 PASS |
| `git commit -m "S12: indradb sink"` | 0 | commit `7340f97` on `story/s12-sink-indradb` |
| `cargo check --lib` (post-refactor) | 0 | After Arc/Mutex removal |
| `cargo fmt --all -- --check` (post-refactor) | 0 | Gate 1 PASS |
| `cargo clippy --all-targets --all-features -- -D warnings` (post-refactor) | 0 | Gate 2 PASS |
| `DYLD_LIBRARY_PATH=... cargo nextest run -p cpp_indexer sink::indradb` (post-refactor) | 0 | 16/16 PASS — Gate 3 PASS |
| `git commit -m "S12: remove Arc<Mutex<>> from IndraDbSink"` | 0 | commit `f9f4499` |

## Deviations from plan.md

1. **USR → Uuid mapping** — ADR-2 specifies idempotency on `(usr, repo_name)` but not the concrete mapping. Chose UUIDv5 with a fixed project namespace UUID. Flagged for sr-dev validation.
2. **`factory::create` async** — S11 already made it async. S12 inherits that change.
3. **`uuid` + `tonic` as direct deps** — required for `v5` feature and `Code` enum; not listed in plan's files-to-touch.
4. **`resolve_indradb_token_opt` in env.rs** — plan didn't mention this helper; needed because factory takes `&SinkConfig`, not `&Config`.
5. **`endpoint()` helper unused in live tests** — `live_sink()` reads the env var directly; the `endpoint()` helper is still there for potential future conditional use but clippy would catch it if it becomes dead code.

## Tool failures / retries

- `Json.into_inner()` — method does not exist; fixed to `&*json_value` (Deref pattern).
- `ClientError::Transport` construction for test — cannot construct without a real transport error object, so test inverted to test a non-transient case instead.

## Completed post-commit refactor

Advisor flagged `Arc<Mutex<indradb_proto::Client>>` as a concurrency bottleneck: every `write_nodes`/`write_edges` call serialised on the lock just to `.clone()` the client before each RPC. Since `indradb_proto::Client` is `Clone` and holds only an internally-pooled tonic `Channel`, the fix is to store the client directly and clone it per call. Applied in commit `f9f4499`: removed `Arc`, `Mutex`, and `tokio::sync::Mutex` from both `IndraDbSink` and `IndraDbPhase5LockGuard`. All 16 unit tests pass.

## Open items for downstream

- `qa-engineer`: Run full integration suite with live IndraDB (see `tests/compose/indradb.yml`).
- `sr-dev`: Validate USR→Uuid namespace UUID; confirm it won't conflict with M5+ cross-repo Phase 5.
