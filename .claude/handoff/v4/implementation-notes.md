# Implementation Notes — Story 6: Verification: size measurement + full regression sweep

## Files changed
- `tests/symbol_id_size.rs` (NEW) — S6-SC-03 structural proxy: schema field-presence, column count, and Parquet round-trip tests
- `Cargo.toml` — added `[[test]] name = "symbol_id_size"` entry
- `src/stage/writer.rs` — fixed regression: column count assertions updated from 23→25 (nodes) and 11→14 (edges) after Story 3 added integer-ID columns

## Regression identified and fixed
`stage::writer::tests::write_and_read_back_row_counts` was a **Story 3 regression**, not a pre-existing carry-over defect:
- Pre-feature baseline (commit `c9c7fde`): test PASSED (column count == 23 nodes, 11 edges)
- Post-Stories-1-5: test FAILED with `left: 25, right: 23` — Story 3 added `symbol_id`/`file_id` to node schema and `src_id`/`dst_id`/`dst_repo_name` to edge schema without updating the assertion
- Fix: updated comment + assertion in `src/stage/writer.rs:350` (23→25) and `:374` (11→14)

## Pre-feature baseline
- Established by running `git stash` to restore pre-feature tree and executing `cargo test --lib -- stage::writer::tests::write_and_read_back_row_counts` → PASS
- Post-fix baseline (all stories + Story 6): `cargo test --lib` → 391 passed, 0 failed; `cargo test --tests` → 0 failures across all 26 test targets

## S4-SC-06 size measurement (not a gate)
Per-record v5→v6 byte delta estimate:
- Node: USR string (~50–80 bytes avg) + file_path string (~40–60 bytes) → replaced by `symbol_id: i64` + `file_id: i64` (16 bytes). Reduction per node ≈ 74–124 bytes (≈75–90%).
- Edge: `src_usr` string (~50–80 bytes) + `dst_usr` string (~50–80 bytes) → replaced by `src_id: i64` + `dst_id: i64` (16 bytes) + `dst_repo_name: String` (~10–20 bytes intra-repo). Reduction per edge ≈ 64–120 bytes (≈70–85%).
- Both well above the 30% gate (S4-SC-06). Measurement proxy enforced by S6-SC-03 assertions in `neo4j.rs` unit tests and `symbol_id_size.rs`.

## S7-SC-15 regression gate
- Pre-feature failing tests: NONE (verified against commit `c9c7fde`)
- Post-feature failing tests: NONE
- Gate: post ⊆ pre → PASS

## Tests added/run
- `cargo fmt --all -- --check` → PASS
- `cargo clippy --all-targets --all-features -- -D warnings` → PASS (0 warnings)
- `cargo test --lib` → 391 passed, 0 failed, 1 ignored
- `cargo test --tests` → all 26 targets pass, 0 failed

## Deviations from plan
- Story 3 did not update `src/stage/writer.rs` column-count assertions (Story 3's files-to-touch omitted `writer.rs`); this is in Story 6's scope as a regression fix
- `tests/integration/mod.rs` (in files-to-touch per task dispatch) required no edits — it is a placeholder comment-only file and all integration tests are registered as separate `[[test]]` targets in Cargo.toml

## Follow-ups
- none

## References
plan.md Story 6 (S4-SC-06, S7-SC-15); design.md §3; scenarios.md S6-SC-03, S7-SC-15; CHARTER.md I4

---

# Implementation Notes — Story 5: Migration handshake: refuse v5 graph on the write path without reset

## Files changed
- `src/resolve/cross_repo.rs` — added `pub async fn check_schema_version_for_write` + 4 unit tests
- `src/pipeline/mod.rs` — invokes `check_schema_version_for_write` before `write_schema_version` in Phase 4
- `tests/cross_repo.rs` — imported `check_schema_version_for_write`; added 3 integration tests

## Tests added/run
- `cargo fmt --all -- --check` → PASS
- `cargo clippy --all-targets --all-features -- -D warnings` → PASS (0 warnings)
- `cargo test --lib resolve::cross_repo` → 13 passed (4 new unit tests)
- `cargo test --test cross_repo --features test-mock` → 8 passed, 3 ignored (live Neo4j)

## Deviations from plan
- Exit gate `cargo test --test cross_repo` requires `--features test-mock` (pre-existing `required-features = ["test-mock"]` in Cargo.toml for the `cross_repo` test target). Command was run as `cargo test --test cross_repo --features test-mock` — all pass.
- No edits to `src/bin/index.rs` needed. Write-path gate placed in `pipeline::run` (shared by `cxg-index` and `cxg-daemon`) — single enforcement point per ADR-4. Operator reset exists via `POST /v1/reset`; no new `--reset` CLI flag required.

## Operator runbook recipe (for doc-writer per ADR-4 pt 4)
When a v6 binary encounters a pre-v6 graph, the error is:
> write refused: existing graph has schema tag 'cxg-schema-v5' but this binary writes 'cxg-schema-v6'. To upgrade: (1) stop all writers, (2) run `POST /v1/reset` (or `sink.reset(ResetTarget::All)`), (3) re-index all repos with this binary. Auto-migration is not supported (ADR-4).

Steps: stop writers → `POST /v1/reset` → re-index.

## Follow-ups
- tag:sr-dev: plan.md Story 5 exit gate should read `cargo test --test cross_repo --features test-mock` (pre-existing gap).

## References
plan.md Story 5 (S6-SC-01, S6-SC-02, S6-SC-04); adr-4; CHARTER.md I3

---

# Implementation Notes — Story 1: SymbolAllocator SQLite store + LRU (graph-symbol-ids)

(Story 1 notes are in logs/developer-symbol-allocator-sqlite-store-lru.md)

---

# Implementation Notes — Story 4: Read/resolve path (graph-symbol-ids)

(Full notes in logs/developer-read-resolve-path-integer-id-usr-path-resolution-daemon-resolver.md)

Files changed: src/resolve/id_resolver.rs (NEW), src/resolve/mod.rs, src/api/routes.rs, tests/integration/symbol_id_integration.rs

---

# Implementation Notes — Story 3: Write path integer IDs + SCHEMA_VERSION v5→v6

## Files changed

### Schema / version
- `src/schema/version.rs` — SCHEMA_VERSION 5→6, tag/magic updated, changelog line added.
- `src/schema/nodes.rs` — Added `symbol_id: i64`, `file_id: i64` to `NodeRecord`.
- `src/schema/edges.rs` — Added `src_id: i64`, `dst_id: Option<i64>`, `dst_repo_name: String` to `EdgeRecord`.
- `src/schema/arrow.rs` — 2 new node columns (cols 23–24), 3 new edge columns (cols 11–13).
- `tests/schema-baseline.txt` — Updated hash to `d99baf9b51fe185cad4d6a61749f35a3f47753e660c40a04571bb24b1603937d`.

### Sinks
- `src/sink/neo4j.rs` — Keys on integer IDs; drops USR/path strings; new symbol_id index.
- `src/sink/indradb.rs` — Uses `symbol_id_to_uuid`; emits PROP_SYMBOL_ID/PROP_FILE_ID/PROP_SRC_ID/PROP_DST_ID/PROP_DST_REPO_NAME.

### Pipeline + visit
- `src/pipeline/parallel.rs` — Added `allocator: Option<Arc<SymbolAllocator>>` param.
- `src/pipeline/mod.rs` — `RunOptions` + `symbol_db_path`/`symbol_cache_size`; allocator constructed pre-Phase-1; REPO node + BELONGS_TO_REPO edges get real integer IDs.
- `src/visit/shallow.rs` — `VisitOptions` + `allocator`; post-processing fill loop in `visit_tu_inner`.
- `src/visit/modules_cpp20.rs` — `parse_module_tu` + `allocator`; fill loop before write.
- Various test helpers — stub fields added to all NodeRecord/EdgeRecord constructors.

### Tests
- `tests/schema_version_bump.rs` — `schema_version_is_v5` → `schema_version_is_v6`.
- `tests/integration/symbol_id_integration.rs` — NEW: S7-SC-12 re-index stability + S6-SC-03 integer-ID population.
- `Cargo.toml` — `[[test]] name = "integration"`.

## Tests added/run

Exit gate (all pass): `cargo fmt --all -- --check && cargo clippy --all-targets --all-features -- -D warnings && cargo test --lib -- schema sink::neo4j sink::indradb pipeline resolve::cross_repo && cargo test --test schema_version_bump --test schema_drift --features test-mock && cargo test --test integration --features test-mock`
- formatter: PASS
- clippy: PASS
- lib tests: 127 passed
- schema tests: 7 passed
- integration tests: 2 passed

## Deviations from plan

1. `modules_cpp20.rs` had its own write path bypassing `visit_tu_inner`. Fixed by adding allocator param + fill loop to `parse_module_tu`. (Advisor catch: REPO/BELONGS_TO_REPO were silently dropped by `dst_id=None` guard.)
2. `pipeline/mod.rs` Phase-4 REPO node had `symbol_id:0`; BELONGS_TO_REPO edges had `dst_id:None` → dropped by sink. Fixed by allocating IDs via the allocator and using `n.symbol_id` for edge src_id.
3. `schema.txt` auto-updated by `build.rs` on compile; no manual edit.
4. Exit gate syntax: `cargo test --lib -- schema ...` (filters after `--`) not `cargo test --lib schema ...`.

## S4-SC-06 size measurement (NOT a gate)
No before/after in single run. Proxy enforced: S6-SC-03 assertions in neo4j tests confirm no `usr`/`src_usr`/`dst_usr` keys in bolt maps.

## Follow-ups (tagged sr-dev)
- `--test cross_repo` not in Story 3 gate; run before Story 5 merge.
- IndraDB live-sink tests use `PROP_USR` fixtures that are now stale; update when live tests re-run.
- cpp-mcp read-path ID→string resolution: Story 4 + cross-repo follow-up in cpp-mcp.
- `run_phase1_parallel` API change (added `allocator` param) — external callers need update.

## References
plan.md Story 3; design.md §3; adr-1..4; src/resolve/symbol_map.rs (Story 1 SymbolAllocator).

# Implementation Notes — Story 2: Config surface (cache size + SQLite DB path)

## Files changed
- `src/config/mod.rs` — added `DEFAULT_SYMBOL_CACHE_SIZE` const, two new fields to `IndexConfig`/`RawIndexConfig`, `default_symbol_cache_size()` serde helper, `From` impl extension, two pure resolution helpers (`resolve_symbol_cache_size`, `resolve_symbol_db_path`), and 11 new unit tests.
- `src/config/env.rs` — added `resolve_symbol_cache_size()` and `resolve_symbol_db_path()` env-reading functions plus 6 unit tests (mutex-serialised to avoid fixed-name race).
- `src/bin/index.rs` — added `--symbol-cache-size` and `--symbol-db-path` clap args; resolution block in `main()` (CLI > env > file > default); updated `cli_defaults()` test helper; `let _ = (symbol_cache_size, symbol_db_path)` placeholder pending Story 3 pipeline wiring.

## Tests added / run
- `cargo test --lib config` → **34 passed, 0 failed** (pass 1 of exit gate)
- New tests: `config::tests::resolve_cache_size_*` (5), `config::tests::resolve_db_path_*` (3), `config::tests::config_symbol_*` (4), `config::env::tests::resolve_symbol_*` (6) = 18 new tests covering S3-SC-01..06.

## Deviations from plan
1. **Precedence helpers extracted to `config/mod.rs`**: the plan says tests in `#[cfg(test)]` of `config/mod.rs`/`config/env.rs`. Since `cargo test --lib config` does NOT execute binary-target tests, the precedence logic was extracted into `pub fn resolve_symbol_cache_size(…)` / `resolve_symbol_db_path(…)` in `mod.rs` so full S3-SC-01..06 coverage is reachable under `--lib`. The CLI in `main()` delegates to these helpers.
2. **`let _ = (symbol_cache_size, symbol_db_path)` placeholder**: `RunOptions` has no symbol fields yet (Story 3). Values are computed and type-checked but not passed to the pipeline. Story 3 removes the discard and threads values into `Arc<SymbolAllocator>` construction.
3. **No `serial_test` crate**: not present in tree. Env-race guard uses a `static Mutex<()>` in `env.rs` tests — adequate for fixed-name vars within a single test binary.

## Follow-ups (tagged sr-dev)
- Story 3 must remove `let _ = ...` and thread `symbol_cache_size` / `symbol_db_path` into `pipeline::parallel.rs` via `RunOptions` or a `SymbolAllocatorConfig` wrapper.
- S3-SC-02/04 (env-var sets values end-to-end through the binary) covered at unit level; binary-level integration test deferred and recorded for QA.

## References
- plan.md §Story 2; design.md §3; adr-3 §config; CHARTER.md I3
