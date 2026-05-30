# Plan: graph-symbol-ids

Goal: Store compact per-repo integer IDs in the CodexGraph (both sinks), keep the USR↔id and path↔id maps in a per-repo SQLite database fronted by a configurable write-through LRU cache, and resolve IDs back to strings on every read path.

Toolchain (rust-conventions, canonical): `cargo fmt --all` / `cargo fmt --all -- --check`; `cargo clippy --all-targets --all-features -- -D warnings`; `cargo test` (scoped per story). Errors: `thiserror` typed enum in lib (`src/error.rs`), `anyhow` in bins.

ID field names (OQ-2, design §3.3 — use verbatim): node `symbol_id`, `file_id`; edge `src_id`, `dst_id` (`Option<i64>`), `dst_repo_name`. All integer type `i64` (SQLite rowid alias). Default DB path = `<stage_dir>/cxg-symbols.db` (OQ-1). Default cache size = 100_000; `0` disables the cache.

## Pre-existing-failure note (developer MUST observe)
- `tests/schema_drift.rs` static-name check (`schema_txt_contains_all_promoted_fields`, `schema_md_contains_all_promoted_fields`, `drift_parser_detects_mutation`) is a STATIC test that must stay green; Story 3 changes promoted fields, so `tests/schema-baseline.txt` (and the `schema.md` it references) MUST be updated in the same story — included in filesToTouch.
- `tests/schema_version_bump.rs` must STAY green: it gates structural `nodes.rs`/`edges.rs` changes on a same-commit `SCHEMA_VERSION` bump. Story 3 satisfies it via the v5→v6 bump.
- The only allowed pre-existing red test is the unrelated `schema_drift` *live-Neo4j* case (`schema_drift_live_neo4j`, requires a running Neo4j; S7-SC-15 exemption). NEVER scope a story's exit-criteria to a bare `cargo test` — always scope `cargo test <module>` to feature modules so the developer does not loop on this.
- S4-SC-06 (≥30% size reduction) is NOT a `cargo test` gate (no "before" in a single run). It is a one-time measurement the developer records in `implementation-notes.md`. The CI proxy is S6-SC-03 ("no `usr`/`src_usr`/`dst_usr` properties on v6 nodes/edges"), asserted in Story 4.

## Cross-story API contract (subtlest part — ADR-1 pts 4-5)
Story 1's `SymbolAllocator` MUST expose, in addition to per-repo get-or-insert:
- a **read-only resolver handle** that can open an *arbitrary* repo's `cxg-symbols.db` by path and resolve `usr → id` and the reverse `id → usr` / `id → path`. Cross-repo EXTERNAL_REF emission (Story 3) resolves `dst_usr → dst_id` against the *destination* repo's db; the read path (Story 4) resolves an EXTERNAL_REF endpoint's `dst_id` against `dst_repo_name`'s db. Without this handle Story 3 and Story 4 cannot compile. State the signature in Story 1's exit and consume it in Stories 3/4.

---

## Story 1 — SymbolAllocator: SQLite store + thread-safe get-or-insert + write-through LRU
Satisfies AC: S1-SC-01..04, S2-SC-01..10, S7-SC-01..11 (unit).
ADRs: adr-1 (pts 1-3), adr-3. Constraints C3, C4, C5, C6.

Files to change:
- `Cargo.toml` — add `rusqlite = { version = "0.31", features = ["bundled"] }` (bundled SQLite; no system lib). LRU may be hand-rolled (HashMap + recency) or `lru` crate (adr-3 alt-c leaves crate choice open) — if a crate is added, justify in implementation-notes.

New files:
- `src/resolve/symbol_map.rs` — `SymbolAllocator`: single write `Connection` behind `std::sync::Mutex`, `PRAGMA journal_mode=WAL; synchronous=NORMAL`; tables `symbols(id INTEGER PRIMARY KEY, usr TEXT NOT NULL UNIQUE)` + `files(id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE)`. API: `open(path, cache_size) -> Result<Self>`, `get_or_insert_symbol(&self, usr) -> Result<i64>`, `get_or_insert_file(&self, path) -> Result<i64>`, reverse `resolve_symbol(&self, id) -> Result<Option<String>>`, `resolve_file(&self, id) -> Result<Option<String>>`, plus a read-only resolver handle (`open_readonly(path, cache_size)` or equiv) for cross-repo/read resolution. Get-or-insert = `INSERT ... ON CONFLICT(usr) DO NOTHING; SELECT id WHERE usr=?` inside the lock (exactly-once under concurrency). Bidirectional write-through LRU (usr↔id, path↔id): SQLite first then cache; `cache_size==0` → `Option<Cache>` is `None`, every lookup hits SQLite; `cache_size==1` evicts on second distinct insert. Cache type private to module. Errors via `crate::error::Error` (`thiserror`); no `.unwrap()`/`.expect()` outside `#[cfg(test)]`.
- Register `pub(crate) mod symbol_map;` in `src/resolve/mod.rs`.

Tests (in `#[cfg(test)] mod tests` at bottom of `symbol_map.rs`, temp-dir / in-memory SQLite):
- S7-SC-01 insert-new-usr → new id + row; S7-SC-02 dup-usr → same id, no new row; S7-SC-03/04 same for path; S7-SC-05 persistence across reopen.
- S7-SC-06 cache hit (no SQLite query — assert via a query-counter or in-memory probe); S7-SC-07 miss → query + populate; S7-SC-08 LRU eviction; S7-SC-09 size-0 disabled; S7-SC-10/11 both-direction (id→usr, id→path).
- S2-SC-08 concurrent same-usr → exactly one id, both callers equal, no dup rows; S2-SC-09 concurrent distinct usrs → distinct ids; S2-SC-10 N threads (≥8) allocate concurrently, no deadlock/panic (use `std::thread::scope` or rayon).

Exit criteria:
- `cargo fmt --all -- --check`
- `cargo clippy --all-targets --all-features -- -D warnings`
- `cargo test --lib resolve::symbol_map`

Parallel-safe: yes (new file + Cargo.toml dep add; no edits to existing logic).

---

## Story 2 — Config surface: cache size + SQLite DB path (CLI + env + struct)
Satisfies AC: S3-SC-01..06. ADR: adr-3 §config. Constraints C5, C6.

Files to change:
- `src/config/mod.rs` — add `symbol_cache_size: usize` (default 100_000) and `symbol_db_path: Option<PathBuf>` (default `None` → resolved to `<stage_dir>/cxg-symbols.db` at use site) to the relevant config struct (`IndexConfig` or a small new `[symbols]` section). Follow existing raw-struct + `deny_unknown_fields` + `From`/validate pattern; non-secret, so no `*_env` indirection.
- `src/config/env.rs` — read `CXG_SYMBOL_CACHE_SIZE` and `CXG_SYMBOL_DB_PATH`, overlaying config-file values (match existing env-overlay precedence).
- `src/bin/index.rs` — add clap derive args `--symbol-cache-size <N>` and `--symbol-db-path <PATH>`; wire into config (CLI > env > file > default, matching the existing precedence in this file).

Tests:
- S3-SC-01/03 CLI sets cache size / db path (parse args → assert config).
- S3-SC-02/04 env var sets cache size / db path.
- S3-SC-05 `cache_size=0` via any surface lands as 0. S3-SC-06 defaults when nothing set (assert default cache size + `None` db path that resolves to `<stage_dir>/cxg-symbols.db`).
- Unit tests in `#[cfg(test)]` of `config/mod.rs` / `config/env.rs`; use `std::env::set_var` guarded by a serial-test pattern already used in the tree if present.

Exit criteria:
- `cargo fmt --all -- --check`
- `cargo clippy --all-targets --all-features -- -D warnings`
- `cargo test --lib config`

Parallel-safe: yes (config-only; does not touch the allocator internals or sinks). Depends conceptually on Story 1's `cache_size`/path knobs existing but can be developed in parallel — the values are plumbed, not the allocator type.

---

## Story 3 — Write path: both sinks emit integer IDs + SCHEMA_VERSION v5→v6 (ATOMIC)
Satisfies AC: S4-SC-01..07, S6-SC-03, S7-SC-12 (integration). ADRs: adr-1 (pts 3-5), adr-2, adr-3 (allocator placement). Constraints C2, C4.

WHY ATOMIC (do NOT split): `tests/schema_version_bump.rs` gates any structural `nodes.rs`/`edges.rs` change on a same-commit `SCHEMA_VERSION` bump (ADR-2 pt 1), and ADR-2/ADR-4 forbid dual-write. A story that adds record fields without the bump, or bumps to v6 while sinks still write `usr`, is a lying intermediate that fails its own gate or yields a v6 graph full of USR strings (S6-SC-03 violation).

Files to change:
- `src/schema/nodes.rs` — add `pub symbol_id: i64`, `pub file_id: i64` to `NodeRecord` (retain `usr`/`file_path` for Phase 5 staging reads; sink drops the strings).
- `src/schema/edges.rs` — add `pub src_id: i64`, `pub dst_id: Option<i64>` (mirrors existing `dst_usr: None` skip rule), `pub dst_repo_name: String` (== `repo_name` for intra-repo) to `EdgeRecord`.
- `src/schema/arrow.rs` — append `Int64` columns for the new fields; `record_batch_to_nodes/edges` keep reading the `usr` fields Phase 5 needs.
- `src/schema/version.rs` — `SCHEMA_VERSION = 6`, `SCHEMA_VERSION_TAG = "cxg-schema-v6"`, `PARQUET_MAGIC = "cxg_parquet_v6"`, in lock-step; add the v6 changelog doc-comment line (adr-2 pts 1-3). In-tree consistency tests (`schema_version_tag_matches_integer`, `parquet_magic_contains_version`) enforce lock-step.
- `src/pipeline/parallel.rs` — construct one `Arc<SymbolAllocator>` per repo (path from config, default `<stage_dir>/cxg-symbols.db`); pass into `run_phase1_parallel` next to the existing `Arc` atomic counters; thread through `VisitOptions`.
- `src/visit/shallow.rs` (and emit callers) — on each `NodeRecord`/`EdgeRecord` emit, call `get_or_insert_symbol(usr)` / `get_or_insert_file(file_path)` and attach `symbol_id`/`file_id`/`src_id`/`dst_id`.
- `src/sink/neo4j.rs` — `CQL_MERGE_NODES` keys `(symbol_id, repo_name)` not `(usr, repo_name)`; `CQL_MERGE_EDGES` keys source on `(src_id, repo_name)`, destination on `(dst_id, dst_repo_name)`; drop `usr`/`src_usr`/`dst_usr`/`file_path` string SET clauses; add `symbol_id`/`file_id`/`src_id`/`dst_id`/`dst_repo_name`; replace `CQL_ENSURE_NODE_USR_INDEX` with an `IF NOT EXISTS` index on `:Node(symbol_id)`; `node_to_bolt`/`edge_to_bolt` emit integer keys + `dst_repo_name`. Keep `repo_name`.
- `src/sink/indradb.rs` — change `usr_to_uuid((repo_name, usr))` keying to `(repo_name, symbol_id)`; same id-only node/edge representation (D2, S4-SC-04/05); drop USR/path string properties (keep `PROP_*` only for integer fields + `repo_name` + `dst_repo_name`).
- `src/resolve/cross_repo.rs` — **DO NOT change** `build_global_usr_map`, `check_schema_version`, or the homogeneity gate (USR matching stays, D1). DO change `materialise_external_refs`: after USR match yields destination `repo_name`, open that repo's `cxg-symbols.db` (read-only handle, Story 1 API; stage dirs available via `Phase5Options::stage_dirs`), resolve `dst_usr → dst_id`; resolve `src_usr → src_id` via the source repo's map; emit `EdgeRecord { src_id, dst_id: Some, repo_name: src_repo, dst_repo_name: dst_repo, .. }`.
- `tests/schema-baseline.txt` (and the `schema.md` it pins, if any) — update promoted-field baseline so `schema_drift.rs` static name-checks stay green with the new fields and removed USR fields.

Tests:
- S4-SC-01/02/03 Neo4j node/edge carry integer ids, no `usr`/`src_usr`/`dst_usr`/path strings (assert on generated Cypher / `node_to_bolt` output via existing mock or unit harness).
- S4-SC-04/05 IndraDB same (extend `tests/sink_indradb.rs`).
- S4-SC-07 `SCHEMA_VERSION == 6` + tag/magic lock-step (existing version.rs unit tests + `schema_version_bump.rs`).
- S6-SC-03 (CI proxy for S4-SC-06): no `usr`/`src_usr`/`dst_usr` string property emitted by either sink for a fresh v6 write.
- S7-SC-12 integration (`tests/integration/`, target `integration`): index a small fixture repo twice via the allocator + the `src/sink/mock.rs` sink (NOT live Neo4j/IndraDB); assert each USR gets the same `symbol_id` on the second run (re-index ID stability, C3). Mock-backed so it is a hard CI gate.
- Record the one-time ≥30% size measurement (S4-SC-06) in `implementation-notes.md` — NOT a gate.

Test-gating note: live-sink tests (`tests/sink_indradb.rs`, `tests/integration/sink_neo4j.rs`) are `#[ignore]` by default (need `INDRADB_ENDPOINT` / live Neo4j). Story 3's sink assertions MUST run against the mock sink or generated-Cypher/`node_to_bolt` output, NOT a live service, so they are hard gates.

Exit criteria (libtest accepts multiple positional filters; cargo 1.95 confirmed):
- `cargo fmt --all -- --check`
- `cargo clippy --all-targets --all-features -- -D warnings`
- `cargo test --lib schema sink::neo4j sink::indradb pipeline resolve::cross_repo`
- `cargo test --test schema_version_bump --test schema_drift`
- `cargo test --test integration` (runs the mock-backed S7-SC-12 re-index-stability test; `#[ignore]`d live cases stay skipped)

Parallel-safe: NO. Highest-shared-surface hub (schema records, both sinks, pipeline, cross_repo, version). Must merge after Story 1 (consumes `SymbolAllocator` + read-only handle). Run solo.

---

## Story 4 — Read/resolve path: integer ID → USR/path resolution (daemon + resolver)
Satisfies AC: S5-SC-01..04 (in-repo scope), S7-SC-13/14 (integration). ADRs: adr-1 (consequence), adr-4 (pt follow-up ii). Constraints C1, C7.

Scope note: cpp-mcp tools (`get_definition`, `get_references`, `get_ast`, `query_graphdb`) live in a SEPARATE repo (`cpp-mcp`) — no path under this repo's `src/`. This story delivers the in-repo daemon resolution + the reusable resolver; cpp-mcp wiring is an explicit cross-repo FOLLOW-UP recorded in `implementation-notes.md` (design §5).

Files to change:
- New `src/resolve/id_resolver.rs` (or add to `symbol_map.rs`) — a repo-scoped reverse resolver wrapping `SymbolAllocator` read-only handles, cache-fronted: `resolve_node(repo, symbol_id, file_id) -> Result<(usr, path)>`, and for an EXTERNAL_REF edge resolve `src_id` via `repo_name`'s db and `dst_id` via `dst_repo_name`'s db. On missing/unknown ID or unavailable SQLite → explicit `Error` (C7, S5-SC-03), never fabricate/blank.
- `src/api/routes.rs` — any daemon endpoint that surfaces symbol/filename data (`GET /v1/repos` and any node/edge-bearing response) resolves integer ids to strings before the HTTP response; preserve existing string-based response contract (S5-SC-04). If `/v1/repos` surfaces only repo metadata (no per-symbol ids), assert that explicitly in the test and record that no symbol-bearing daemon endpoint currently exists beyond it (deferred consumers listed in implementation-notes).
- Register the new module in `src/resolve/mod.rs` if added separately.

Tests:
- S5-SC-01 resolver maps ids→usr/path (unit). S5-SC-03 missing id / unavailable db → explicit error (negative unit test). S5-SC-04 round-trip preserves the original strings.
- S5-SC-02 daemon endpoint returns no raw integer ids (extend `routes.rs` `#[cfg(test)]` axum test, or assert N/A + deferred).
- S7-SC-13/14 round-trip: write nodes/edges with integer ids → resolve through SQLite → recover original USR/path. The CI-gated version uses the `src/sink/mock.rs` sink (hard gate, in `tests/integration/`, target `integration`). The live-Neo4j/IndraDB variants (`tests/integration/sink_neo4j.rs`, `tests/sink_indradb.rs`) are `#[ignore]`d env-gated extensions (NOT exit-gates).

Exit criteria:
- `cargo fmt --all -- --check`
- `cargo clippy --all-targets --all-features -- -D warnings`
- `cargo test --lib resolve::id_resolver resolve::symbol_map api::routes`
- `cargo test --test integration` (runs the mock-backed S7-SC-13/14 round-trip; `#[ignore]`d live sink cases stay skipped)

Parallel-safe: NO with Story 3 (consumes the integer-id graph + Story 1 read-only handle; integration tests require Story 3's sink writes). Merge after Story 3.

---

## Story 5 — Migration handshake: refuse v5 graph on the write path without reset
Satisfies AC: S6-SC-01, S6-SC-02, S6-SC-04 (runbook recipe shape). ADR: adr-4 (decision pts 1-2, follow-up i). Constraint C7.

Files to change:
- `src/resolve/cross_repo.rs` — `check_schema_version` already returns `Error::Schema` naming expected/actual tag on read; extend the handshake so the WRITE path (`cxg-index` startup, before appending v6 data) also refuses a v5 graph with an explicit error instructing reset/re-index (adr-4 follow-up i). Reuse the existing error shape; do NOT auto-migrate.
- `src/bin/index.rs` (and/or `src/pipeline/mod.rs`) — invoke the write-path version gate before Phase 4/6 write; surface the explicit error. Reuse existing `reset(ResetTarget)` plumbing for the operator-driven path (no new reset logic).
- Note: the runbook prose (S6-SC-04: stop writers → `reset` → re-index v6) is owned downstream (devops/doc-writer) per adr-4 pt 4; this story fixes the recipe SHAPE and the hard-error behavior, and records the recipe in `implementation-notes.md` for the doc-writer.

Tests:
- S6-SC-01 v6 binary reading a v5-tagged `SchemaVersion` node → explicit `Error::Schema` naming both tags (unit; reuse the mock sink / existing `check_schema_version` test harness).
- S6-SC-02 destructive reset is gated on explicit operator action (assert the write path errors rather than silently overwriting when no `--reset`).

Exit criteria:
- `cargo fmt --all -- --check`
- `cargo clippy --all-targets --all-features -- -D warnings`
- `cargo test --lib resolve::cross_repo`
- `cargo test --test cross_repo`

Parallel-safe: NO with Story 3 (touches `cross_repo.rs` + the v6 tag from version.rs). Merge after Story 3.

---

## Story 6 — Verification: size measurement + full regression sweep
Satisfies AC: S4-SC-06 (measurement), S7-SC-15 (regression). ADRs: all. Success criterion (size goal).

Files to change:
- New `tests/symbol_id_size.rs` (or extend `tests/integration/`) — index a representative fixture, compute node+edge storage bytes pre/post conceptually; since a single run has no "before", the developer records the measured v5-vs-v6 reduction in `implementation-notes.md` and the test asserts the structural proxy (S6-SC-03: zero USR-string properties on v6 nodes/edges, and integer fields present). This converts the unverifiable single-run 30% into a durable structural assertion plus a recorded measurement.

Tests:
- S4-SC-06 measurement recorded in implementation-notes (one-time; not a gate).
- S7-SC-15 regression: **no NEW failures vs the pre-feature baseline.** The repo has known carry-over defects beyond `schema_drift` (per project status: schema_drift fail + 5 carry-over defects). The developer MUST capture the baseline by running the full suite at the PRE-feature commit (`cargo test --lib && cargo test --tests` at HEAD before any story lands) and record the failing-test set in `implementation-notes.md`. The gate is: the post-feature failing set ⊆ the pre-feature failing set (no regressions introduced). Do NOT fix pre-existing carry-over defects (out of scope; CLAUDE.md scope discipline). Live-service tests (`#[ignore]`d) are excluded from both baselines absent their env vars.

Exit criteria:
- `cargo fmt --all -- --check`
- `cargo clippy --all-targets --all-features -- -D warnings`
- `cargo test --lib`
- `cargo test --tests` (compare the failing set against the recorded pre-feature baseline in implementation-notes.md; pass iff no NEW failures vs baseline — `#[ignore]`d live-service cases excluded)

Parallel-safe: NO (validates the integrated result of Stories 1-5; runs last).

---

## Risks / Out of scope
- Out of scope: gRPC / M9-M10; new node/edge KINDS; cross-repo integer-ID unification (D1); libclang visit semantics beyond id emission.
- Risk: cpp-mcp read-path resolution is a separate repo — delivered only as a documented cross-repo follow-up here (design §5, Story 4 note). Coordinator must track it.
- Risk: `schema_drift.rs` baseline drift — Story 3 MUST update `tests/schema-baseline.txt`; verify by reading `schema_drift.rs` before editing.
- Risk: write-Mutex serialises allocations under heavy parallel visit; adr-3 deems negligible, batching is a follow-up not a requirement.
- Risk: S4-SC-06 30% goal is a recorded measurement, not a CI gate; the enforceable proxy is S6-SC-03.
- Risk: known pre-existing reds (schema_drift + 5 carry-over defects) — S7-SC-15 gate is "no NEW failures vs a captured pre-feature baseline," NOT "single known red." QA must diff against the baseline so CHARTER I4 (no open QA_DEFECT) is not falsely tripped by carry-over defects. Carry-over defects are OUT OF SCOPE for this feature.

## References
- design.md (§3 components, §3.3 field names, §3.6 cross-repo, §5 read path, §7 files-to-touch); requirements.md (S1-S7, D1, D2, OQ-1..4); scenarios.md (all SC ids cited above)
- adr-1 (per-repo IDs + cross-repo emission), adr-2 (v5→v6 bump), adr-3 (SQLite + allocator + LRU), adr-4 (reset-not-migrate)
- CHARTER.md (invariants I2/I3, traceability chain, failure taxonomy)
- Code: `src/resolve/{symbol_map(new),cross_repo,mod}.rs`, `src/schema/{nodes,edges,arrow,version}.rs`, `src/sink/{neo4j,indradb}.rs`, `src/pipeline/parallel.rs`, `src/visit/shallow.rs`, `src/config/{mod,env}.rs`, `src/bin/index.rs`, `src/api/routes.rs`, `tests/{schema-baseline.txt,schema_drift,schema_version_bump,sink_indradb,cross_repo}`
- Wiki: [[pages/code/cpp-indexer]], [[pages/code/cpp-mcp]]
- cognee tags: `task:graph-symbol-ids`, `role:senior-developer`
