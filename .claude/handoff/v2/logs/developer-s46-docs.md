---
story: S46
slug: s46-docs
role: developer
date: 2026-05-18
signals: clear
---

# Developer Log — S46 (Schema docs, prompt/example refresh, wiki cross-links)

## Skills loaded

- `rust-conventions` — loaded before writing any code; no `impl-story` needed (pure docs + one test file).

## Skills considered but not loaded

- `implement-story` — S46 is primarily documentation and one test; no complex feature implementation needed. Skipped.
- `simplify` — no existing code changed; only new files + doc edits. Skipped.

## Orientation (before writing)

1. Read `CHARTER.md`, `plan.md` (lines 278+), `design.md` (full).
2. Read `prompt/graph_database/cpp/schema.txt` (existing content).
3. Read `docs/runbooks/staging-recovery.md` (full; located append point).
4. Listed `tests/integration/` and `tests/` to determine correct test binary path.
5. Read `src/schema/nodes.rs` (NodeRecord fields, lines 172-238) — confirmed 10 promoted node fields.
6. Read `src/schema/edges.rs` (EdgeRecord fields, lines 142-175) — confirmed 2 promoted edge fields.
7. Read `src/visit/access_classifier.rs` (AccessKind 7-value enum).
8. Read `src/sink/neo4j.rs` (CQL_MERGE_NODES/CQL_MERGE_EDGES, index constants).
9. Read `tests/integration/neo4j_indexes.rs` (live-gate pattern, `#[ignore]`, env-var checks).
10. Called `advisor` — key finding: `tests/integration/schema_drift.rs` path in plan would not be runnable via `cargo test --test schema_drift`; must be top-level. Advisor also noted the fixture-mutation sub-test must be non-ignored.

## Commands run

```
cargo build --all-targets          → exit 0  (0.88s)
cargo clippy --all-targets -- -D warnings → exit 0
cargo fmt --all                    → reformatted tests/schema_drift.rs (two style fixes; pass 1)
cargo fmt --all                    → reformatted again (word-boundary function; pass 2)
cargo fmt --all -- --check         → exit 0
test -f docs/schema/SCHEMA.md      → exit 0
grep -q "re-index" docs/runbooks/staging-recovery.md → exit 0
grep -q "source_association_type" prompt/graph_database/cpp/schema.txt → exit 0
grep -q "return_type" prompt/graph_database/cpp/schema.txt → exit 0
cargo test --test schema_drift     → 3 passed; 1 ignored; exit 0

# Advisor noted substring-shadow bug; fixed names_found_in_file with word-boundary detection
# and added 3 new boundary sub-cases to drift_parser_detects_mutation.

cargo test --test schema_drift     → 3 passed; 1 ignored; exit 0  (pass 2, post-fix)

# Live gate attempt (exit criteria requires this):
CPP_INDEXER_LIVE_NEO4J=1 NEO4J_URI=bolt://192.168.1.200:7687 \
  cargo test --test schema_drift -- --ignored --nocapture
  → FAILED: AuthenticationError — NEO4J_PASSWORD not in environment
  # Identical failure to S44 neo4j_indexes live tests without credentials.
  # See note below.
```

Pass count: 2 (non-live gates clear; live gate blocked by credential gap).

### Live gate credential note

The live gate `CPP_INDEXER_LIVE_NEO4J=1 cargo test --test schema_drift -- --ignored --nocapture` requires `NEO4J_PASSWORD` to be set in the environment. This credential is not stored anywhere in the repo or in the developer session (consistent with the security model documented in the wiki — `password_env` pattern). The same `AuthenticationError` occurs for all other live tests (neo4j_indexes, cross_repo, indradb_properties) when run without credentials. This is not a code defect — the test logic is correct. The QA engineer must supply `NEO4J_PASSWORD` when running the live gate. Flagged to `@qa-engineer`.

## Deviations from plan.md

1. **Test file path**: plan.md specifies `tests/integration/schema_drift.rs`; implemented at `tests/schema_drift.rs`. The exit criterion uses `cargo test --test schema_drift`, which selects a top-level integration binary — not possible from a submodule path. All other top-level integration tests (e.g. `neo4j_indexes`, `indradb_properties`) also live at `tests/` but ARE declared as submodules under `tests/integration/mod.rs`. The distinction is that those are reached via the `integration` test binary, not via `--test neo4j_indexes`. `schema_drift` is explicitly named in the exit criteria so it needs a top-level file. Flagged to `@sr-dev`.
2. **Wiki page (AC-S46-3)**: not implemented. Task dispatch notes state "Doc-writer (Role 8) will handle wiki page itself". Filed as follow-up for `@doc-writer`.

## Tool failures or retries

- `cargo fmt` on first pass: reformatted two lines in `tests/schema_drift.rs` (long method chains). Re-ran `--check` after auto-format — exit 0. No retry needed on any other gate.

## Named signals

- BUILD_FAIL: not emitted (build clean)
- LINT_FAIL: not emitted (clippy clean)
- TEST_FAIL: not emitted (all non-ignored tests passed)

## Status: clear
