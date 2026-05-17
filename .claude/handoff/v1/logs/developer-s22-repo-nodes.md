# Developer Log — S22-repo-nodes

task-slug: cpp-indexer
story: S22-repo-nodes
role: developer
date: 2026-05-17

## Skills loaded
- rust-conventions (loaded before writing any code)

## Skills considered but not loaded
- cpp-conventions — project is Rust only
- python-conventions — no Python files
- implement-story — coordinator dispatched directly, did not need the skill scaffold
- cognee-memory — orientation was via file reads, not semantic recall

## Orientation reads
- CHARTER.md — paths, invariants, project context
- plan.md lines 386–400 — S22 story spec
- design.md — Phase 4 contract, module layout
- adr-9.md — SCHEMA_VERSION bump policy, SchemaVersion node spec
- src/schema/nodes.rs, edges.rs, version.rs — existing schema state
- src/sink/mod.rs, neo4j.rs — attrs_json handling in CQL_MERGE_NODES
- src/pipeline/mod.rs — RunOptions struct, run() function phases
- src/bin/index.rs — RunOptions call site
- src/sink/mock.rs — MockCall variants for integration tests
- Cargo.toml — git2, tempfile, serde_json deps confirmed; existing [[test]] entries

## Advisor call
Called advisor after orientation, before writing code. Key guidance acted on:
1. Read AC-M4-1/2/3 verbatim — done. AC-M4-3 enforcement deferred to S23 (S22 carries the `sink` attr).
2. SCHEMA_VERSION 2 → 3 mandatory — done.
3. Confirm git2 in Cargo.toml — confirmed.
4. Fix stale doc comment in nodes.rs (`MACRO (S22)`) — done.
5. REPO node attrs in attrs_json — done (root_path, commit_sha, commit_date, sink).
6. BELONGS_TO_REPO emission in pipeline::run, not in visitor — done.
7. Use `sink.backend_name()` from the Arc<dyn GraphSink> — done.
8. SchemaVersion + WRITTEN_WITH_SCHEMA not in S22 — noted as follow-up.
9. Integration test uses git2::Repository::init + one commit in TempDir — done.

## Commands run + outcomes
```
cargo build --lib                                              # exit 0
cargo build --tests --features test-mock                       # exit 0
cargo fmt --all -- --check                                     # exit 1 (formatting diffs)
cargo fmt --all                                                # exit 0 (applied)
cargo clippy --all-targets --all-features -- -D warnings       # exit 1 (manual_range_contains)
# fixed: !(1970..=9999).contains(&year)
cargo clippy --all-targets --all-features -- -D warnings       # exit 0
DYLD_LIBRARY_PATH=... cargo nextest run --test repo_meta --features test-mock  # 2/2 pass
DYLD_LIBRARY_PATH=... cargo nextest run --lib --features test-mock             # 154/154 pass
cargo fmt --all -- --check                                     # exit 0
git add -A && git -c commit.gpgsign=false commit -m "S22: repo nodes"  # committed
```

## Deviations from plan.md
- Added `skip_repo_node: bool` to `RunOptions` (not in plan). Required to preserve existing tests that run without a git repository. Default false in production; true in legacy tests. Noted in implementation-notes.md.
- SchemaVersion/WRITTEN_WITH_SCHEMA edge skipped — ADR-9 says Phase 4 write; S23 is the enforcement story. Not a deviation, a clarification.

## Tool failures / retries
- clippy pass 1 failed: `clippy::manual_range_contains` on `year < 1970 || year > 9999`. Fixed on first retry.
- fmt pass 1 failed: style diffs in repo_meta.rs and integration test. Applied with `cargo fmt --all`.

## Named signals
- BUILD_FAIL: cleared
- LINT_FAIL: cleared (after range-contains fix)
- TEST_FAIL: cleared (2 integration + 154 unit, all pass)
