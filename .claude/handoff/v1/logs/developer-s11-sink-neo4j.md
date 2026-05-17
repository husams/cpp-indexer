# Developer log — S11-sink-neo4j

## Skills loaded
- `rust-conventions` (triggered by Cargo.toml + *.rs presence)

## Skills considered but not loaded
- `implement-story` — plan.md story was already scoped; no additional story elaboration needed
- `simplify` — code not refactored from existing; net-new files only

## Files read for orientation
- CHARTER.md
- plan.md §S11 (lines 217–231)
- adr-2.md
- src/sink/mod.rs, factory.rs, lock.rs, mock.rs
- src/config/mod.rs, env.rs
- src/schema/nodes.rs, edges.rs
- src/stage/writer.rs
- design.md §Phase 4
- neo4rs-0.7.3 source: lib.rs, graph.rs, query.rs, convert.rs

## Key design decisions

### Node label scheme
Single `:Node` label with `kind` as a string property. design.md §Phase 4 says "USR-keyed MERGE on Neo4j" without specifying dual labels; single label is simpler and avoids label-proliferation issues with dynamic MERGE.

### Edges with `dst_usr = None`
Skipped. Phase 3 writes `cross_repo_candidate=true` + `resolved=false` for unresolved edges but the `dst_usr` field can still be `None` if Phase 3 hasn't matched it. An edge with no `dst_usr` has no stable idempotency key `(src_usr, dst_usr, kind)`, so MERGE is impossible. The factory contract documents this as intended behavior; Phase 5 resolves these via EXTERNAL_REF.

### Phase 5 lock
TTL-based leader-election using `timestamp()` — no APOC dependency. Lock node is `(:Phase5Lock {id:'singleton'})` with `holder` and `expires_at` properties. On match, sets holder only if `expires_at < timestamp()` (expired).

### Factory signature change
`factory::create` changed from sync `fn create(...)` to `async fn create(...)` to support `Neo4jSink::connect`. Callers that used `create` must now `.await` it. This affects `pipeline::run` (not yet implemented in S11 scope). The old tests that asserted "neo4j must error before impl lands" were replaced with tests for the actual behavior (missing subsection → Config, unset env → Sink, unknown backend → Config).

## Commands run + outcomes

| Command | Outcome |
|---|---|
| `cargo check --all-targets --all-features` | 1 warning (`unused import: Arc`); fixed |
| `cargo fmt --all -- --check` (pass 1) | FAIL: formatting diffs in neo4j.rs, factory.rs, mod.rs, sink_neo4j.rs |
| `cargo fmt --all` | Fixed all formatting |
| `cargo fmt --all -- --check` (pass 2) | EXIT 0 |
| `cargo clippy --all-targets --all-features -- -D warnings` | EXIT 0 (no warnings) |
| `DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo nextest run -p cpp_indexer sink::neo4j` | EXIT 0 — 12/12 passed |

## Deviations from plan

1. **`factory::create` is now `async`** — plan.md says "register in sink/factory.rs" without specifying sync/async. Neo4jSink::connect requires async. Callers (S13 pipeline) must await.
2. **`DYLD_LIBRARY_PATH` required for tests on macOS** — `libclang.dylib` is not on the default rpath on macOS developer installs; `DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib` (or Xcode toolchain equivalent) is needed. This is a pre-existing codebase issue, not introduced by S11.
3. **`mod neo4j` placed after `mod lock`** — formatter reordered relative to `#[cfg(any(test, ...))] mod mock` to maintain alphabetical order (lock → mock → neo4j is what rustfmt chose: factory, lock, neo4j, mock with cfg gate).

## Open items / follow-ups
- `factory::create` is now async; S13 (pipeline::run) must be updated to `.await` the factory call. Tag: @sr-dev.
- Integration test gate (`--ignored`) requires a live Neo4j instance. Docker compose at `tests/compose/neo4j.yml` is provided. CI wiring deferred to DevOps stage.
- DYLD_LIBRARY_PATH must be set in CI for macOS runners. Pre-existing issue, surfaced here because S11 is the first story to exercise neo4rs (which pulls in clang-sys indirectly). Tag: @sr-dev for CI config.
- Phase 5 lock: `Neo4jPhase5LockGuard::drop` uses fire-and-forget via `tokio::runtime::Handle::try_current()`. If no runtime is present at drop time (e.g. sync test context), the release is silently skipped. Documented in code; acceptable for v1.
