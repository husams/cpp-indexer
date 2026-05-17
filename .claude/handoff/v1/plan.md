# cpp-indexer — Implementation Plan (M1–M7 → v1 GA)

run_id: cpp-indexer-v1
stage: 4 of 8 — senior-developer
version: 1.0
date: 2026-05-17
upstream: requirements.md v1.0, design.md v1.0, adr-{1..10}.md (all `Status: accepted`)
downstream: developer reads this story-by-story; coordinator dispatches per wave

## Conventions (apply to ALL stories)

- Language: Rust 2021. Crate: single library `cpp_indexer` + three bin targets (`cxg-index`, `cxg-resolve-cross-repo`, `cxg-daemon`). See design.md §3 for the canonical module layout — every story's `files-to-touch` references paths from that tree.
- Toolchain (per rust-conventions): formatter `cargo fmt`, lint `cargo clippy --all-targets --all-features -- -D warnings`, tests `cargo nextest run` (fallback `cargo test --all-targets`), build `cargo build`.
- Error model: library uses `thiserror`-derived `cpp_indexer::Error`; bin crates use `anyhow::Result` in `main`. No `unwrap()`/`expect()` outside tests and `main` init.
- Async runtime: `tokio` (multi-thread) for daemon + sinks; `rayon` for Phase 1 CPU pool (per ADR-7).
- Worktree convention: every parallel-safe story is implemented under `/Users/husam/workspace/cpp-indexer/.worktrees/<story-slug>/` on branch `story/<story-slug>` cut from `main`. Coordinator merges between waves.
- Exit-criteria commands are mandatory and run inside the story's worktree. The four canonical gates are:
  1. `cargo fmt --all -- --check`
  2. `cargo clippy --all-targets --all-features -- -D warnings`
  3. `cargo build --all-targets`
  4. story-specific `cargo nextest run -p cpp_indexer --test <name>` (or unit test pattern)
- Traceability: every story references AC IDs from requirements.md and the ADRs that govern it. Developer MUST surface `MISSING_EXIT_CRITERIA`, `LINT_FAIL`, `TEST_FAIL`, or `BUILD_FAIL` per CHARTER §Failure taxonomy.

## Wave plan (dependency-ordered)

| Wave | Stories | Parallel-safe? |
|---|---|---|
| W0 | S01-init-crate | no (foundation; everything depends on it) |
| W1 | S02-schema, S03-config, S04-error-tracing | yes (disjoint modules) |
| W2 | S05-compile-commands, S06-autodetect | yes |
| W3 | S07-stage-parquet, S08-graphsink-trait-mock | yes |
| W4 | S09-visit-shallow-base | no (consumes W2+W3) |
| W5 | S10-resolve-per-repo, S11-sink-neo4j, S12-sink-indradb | yes |
| W6 | S13-pipeline-orchestration-m1-gate | no (M1 exit) |
| W7 | S14-visit-cpp-extensions, S15-system-header-filter | yes |
| W8 | S16-m2-boost-optional-gate | no (M2 exit) |
| W9 | S17-parallel-phase1, S18-batched-sink-writes, S19-content-hash-cache | partial (S17 & S19 share `pipeline/`; S18 disjoint) |
| W10 | S20-memory-spill-progress, S21-m3-perf-gate | sequential |
| W11 | S22-repo-nodes, S23-cross-repo-resolver-bin | sequential (S23 depends on S22) |
| W12 | S24-syshdr-canonicalisation, S25-m4-two-repo-gate | sequential |
| W13 | S26-macros, S27-phase2-decorate, S28-cpp20-modules | yes |
| W14 | S29-m5-chromium-gate | no |
| W15 | S30-prompt-codegen, S31-schema-version-mcp-handshake | yes |
| W16 | S32-m6-agent-gate | no (manual + integration) |
| W17 | S33-daemon-rest, S34-daemon-reset, S35-daemon-metrics | yes |
| W18 | S36-workspace-git-ingest | no (depends on S33) |
| W19 | S37-docker-ci, S38-runbook | yes |
| W20 | S39-m7-soak-gate | no |

Total: **39 stories** (count: 21 marked `parallel-safe: true`).

---

## Story specs

### S01-init-crate

- Title: Initialise Rust crate skeleton, dependency manifest, CI scaffold.
- AC covered: AC-M1-1 (compiles on Linux + macOS with libclang 18 linked).
- Files to touch:
  - new `Cargo.toml` (single-crate, edition `2021`, three `[[bin]]` targets pointing at `src/bin/*.rs`)
  - new `rust-toolchain.toml` (channel `stable`, components `rustfmt`, `clippy`)
  - new `src/lib.rs` (empty `pub mod` stubs for `error`, `config`, `schema`, `bootstrap`, `visit`, `stage`, `resolve`, `sink`, `pipeline`, `api`, `workspace`, `prompt`)
  - new `src/bin/{index,resolve_cross_repo,daemon}.rs` (skeleton `fn main() -> anyhow::Result<()> { Ok(()) }`)
  - new `build.rs` (no-op placeholder; S30 fills in)
  - new `.github/workflows/ci.yml` (matrix linux/macos: fmt, clippy, build, test)
  - new `.gitignore` additions (`target/`, `.cxg-cache/`, `.worktrees/`)
  - new `tests/integration/mod.rs` (placeholder)
- Cargo deps (pinned major versions): `clang = "2"` (libclang 18 wrapper), `rayon = "1"`, `tokio = { version = "1", features = ["full"] }`, `axum = "0.7"`, `tower = "0.5"`, `tower-http = "0.5"`, `git2 = "0.18"`, `neo4rs = "0.7"`, `indradb-proto = "5"`, `indradb-lib = "5"`, `arrow = "53"`, `parquet = "53"`, `serde = { version = "1", features = ["derive"] }`, `serde_json = "1"`, `toml = "0.8"`, `thiserror = "1"`, `anyhow = "1"`, `blake3 = "1"`, `tracing = "0.1"`, `tracing-subscriber = { version = "0.3", features = ["env-filter"] }`, `clap = { version = "4", features = ["derive"] }`, `async-trait = "0.1"`, `prometheus = "0.13"`, `rocksdb = "0.22"`, `num_cpus = "1"`, `glob = "0.3"`. Dev: `tempfile = "3"`, `assert_cmd = "2"`, `predicates = "3"`.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo build --all-targets` (Linux + macOS; CI matrix verifies)
  - `cargo nextest run` (zero tests OK; gate is non-zero exit)
  - all three binaries link: `cargo build --release --bin cxg-index --bin cxg-resolve-cross-repo --bin cxg-daemon`
- References: design.md §3 module layout; CHARTER project context; ADR-2 (sink deps), ADR-5 (axum), ADR-6 (git2), ADR-7 (rayon/clang).
- Parallel-safe: **no** (every other story branches from this).

---

### S02-schema-base-types

- Title: Base CodexGraph node/edge enums + Arrow round-trip.
- AC covered: AC-M1-2, AC-M1-3, AC-M1-4.
- Files to touch:
  - `src/schema/mod.rs`, `src/schema/nodes.rs`, `src/schema/edges.rs`, `src/schema/arrow.rs`, `src/schema/version.rs` (define `SCHEMA_VERSION: &str = "1"` per ADR-9)
- Tests: unit tests in `src/schema/arrow.rs` — for each `NodeKind`/`EdgeKind` variant, build a sample record, write+read via `arrow::record_batch::RecordBatch`, assert round-trip equality.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer schema::`
- References: design.md §3 schema/, ADR-3 (column layout), ADR-9 (version const).
- Parallel-safe: **yes** (disjoint from S03/S04).

---

### S03-config-toml

- Title: `cxg-index.toml` + `cxg-daemon.toml` serde schemas with env-indirected secrets.
- AC covered: AC-M1-23, AC-M1-24 (partial — env-var resolution + startup validation).
- Files to touch: `src/config/mod.rs`, `src/config/env.rs`
- Tests: unit tests — (a) parse golden TOMLs from `tests/fixtures/config/`; (b) missing required field → typed error naming the field path; (c) `password_env`/`token_env` unset at resolve time → `Error::Sink` with env-var name; (d) any `password`/`token`/`pat` field in TOML rejected at parse with clear error.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer config::`
- References: design.md §5.1, §5.2; ADR-2 (sink credentials); ADR-5 (`[api].auth_token_env`); ADR-6 (`[workspace].git_credentials_env`).
- Parallel-safe: **yes**.

---

### S04-error-tracing

- Title: `cpp_indexer::Error` enum + `tracing` setup with secret redaction.
- AC covered: cross-cutting infrastructure for AC-M1-7, AC-M1-9, AC-M1-24, AC-M3-2.
- Files to touch: `src/error.rs`, `src/lib.rs` (re-export), new `src/observability.rs` (subscriber init helper used by all three bins)
- Tests: unit — verify a `tracing` event carrying `password=secret123` is rendered with `***`; verify `Error::Autodetect{searched}` `Display` lists every path.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer error::`
- References: design.md §5.2, §5.3.
- Parallel-safe: **yes**.

---

### S05-compile-commands-parser

- Title: Phase 0 — parse + dedup `compile_commands.json`.
- AC covered: AC-M1-5, AC-M1-6, AC-M1-7.
- Files to touch: `src/bootstrap/compile_commands.rs`, `src/bootstrap/mod.rs`
- Tests: unit — (a) parse valid file → expected `Vec<TuEntry>`; (b) duplicate `(file, args)` entries collapse to one; (c) malformed JSON → `Error::CompileCommands` with file path; (d) Blake3 hash stable across runs.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer bootstrap::compile_commands`
- References: design.md §Phase 0; requirements.md M1-S2.
- Parallel-safe: **yes** (disjoint from S06).

---

### S06-autodetect

- Title: Phase 0.5 — upward-walk auto-detection of `compile_commands.json`.
- AC covered: AC-M1-8, AC-M1-9, AC-M1-10, AC-M1-11, AC-M1-12, AC-M1-13.
- Files to touch: `src/bootstrap/autodetect.rs`
- Tests: integration in `tests/integration/autodetect.rs` using `tempfile::tempdir()` to synthesise repo layouts — (a) file under `<repo>/src/`, cc.json at `<repo>/build/` → resolved + INFO log captured via `tracing-test`; (b) no cc.json → exit-error listing every dir probed; (c) walk stops at `.git`; (d) two candidates `build/` + `out/`, file in `build/` entry list → `build/` chosen; (e) dir input filters entries; (f) file input → exact-match entry only.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer --test autodetect`
- References: design.md §Phase 0.5; requirements.md M1-S3.
- Parallel-safe: **yes**.

---

### S07-stage-parquet

- Title: Per-worker Parquet shard writer + manifest skeleton.
- AC covered: groundwork for AC-M1-14, AC-M1-17, AC-M3-7 (manifest schema only).
- Files to touch: `src/stage/writer.rs`, `src/stage/manifest.rs`, `src/stage/schema.rs`, `src/stage/mod.rs`
- Tests: unit — write N node + M edge records to a tempdir, re-read via `parquet::arrow::async_reader`, assert row count + column types match ADR-3; manifest JSON round-trip.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer stage::`
- References: ADR-3 (Parquet schemas + disk layout); design.md §3 stage/.
- Parallel-safe: **yes** (disjoint from S08).

---

### S08-graphsink-trait-mock

- Title: `GraphSink` async trait + `MockSink` for unit tests + factory.
- AC covered: AC-M1-22 (trait shape so both sinks satisfy a shared test fixture).
- Files to touch: `src/sink/mod.rs` (trait + `NodeRecord`/`EdgeRecord`/`WriteStats`/`Phase5LockGuard`), `src/sink/factory.rs`, `src/sink/lock.rs`, `src/sink/mock.rs` (under `#[cfg(any(test, feature = "test-mock"))]`)
- Tests: unit — `MockSink` records call sequence; `factory::create` dispatches on `[sink].backend`; trait-object compiles behind `Arc<dyn GraphSink>`.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer sink::`
- References: ADR-2.
- Parallel-safe: **yes**.

---

### S09-visit-shallow-base

- Title: Phase 1 base libclang visitor → Parquet (single-threaded).
- AC covered: AC-M1-14, AC-M1-15, AC-M1-16, AC-M1-17.
- Files to touch: `src/visit/shallow.rs`, `src/visit/cursor_map.rs`, `src/visit/mod.rs`
- Tests: integration `tests/integration/phase1_base.rs` — run visitor against `tests/fixtures/m1_5file/` (committed in this story), assert Parquet contains expected nodes for each `MODULE/CLASS/FUNCTION/METHOD/FIELD/GLOBAL_VARIABLE`; assert every node has non-empty `usr`; assert a TU with intentional parse error produces diagnostic + run continues; assert zero sink calls during Phase 1 (use `MockSink`).
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer --test phase1_base`
- References: design.md §Phase 1; ADR-7 (single-thread for M1, parallelised in S17).
- Parallel-safe: **no** (consumes S05/S07; foundation for W5).

---

### S10-resolve-per-repo

- Title: Phase 3 in-memory USR-map resolution → `final-edges.parquet`.
- AC covered: AC-M1-18, AC-M1-19, AC-M2-13.
- Files to touch: `src/resolve/per_repo.rs`, `src/resolve/mod.rs`
- Tests: integration `tests/integration/phase3.rs` — synthesise shards with N within-repo edges + K unresolved edges; assert final-edges Parquet has all within-repo `resolved=true`; unresolved edges retain `cross_repo_candidate=true`.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer --test phase3`
- References: design.md §Phase 3.
- Parallel-safe: **yes** (disjoint from S11/S12).

---

### S11-sink-neo4j

- Title: `Neo4jSink` implementation via `neo4rs`.
- AC covered: AC-M1-20, AC-M1-23 (Neo4j half), AC-M3-6 (idempotent MERGE).
- Files to touch: `src/sink/neo4j.rs`, `tests/compose/neo4j.yml` (Docker compose for integration)
- Tests: integration `tests/integration/sink_neo4j.rs` gated `#[ignore]` if `NEO4J_URI` env unset — `preflight` → `ensure_indexes` → `write_nodes` → `write_edges` → MERGE idempotency by re-running the same batch. Unit tests for Cypher query construction (no DB needed).
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer sink::neo4j`
  - integration: `NEO4J_URI=bolt://localhost:7687 NEO4J_PASSWORD_ENV=N4J_PW N4J_PW=test cargo nextest run --test sink_neo4j -- --ignored`
- References: ADR-2; design.md §Phase 4.
- Parallel-safe: **yes**.

---

### S12-sink-indradb

- Title: `IndraDbSink` implementation via `indradb-proto`.
- AC covered: AC-M1-21, AC-M1-23 (IndraDB half), AC-M3-6.
- Files to touch: `src/sink/indradb.rs`, `tests/compose/indradb.yml`
- Tests: mirror of S11 against IndraDB gRPC endpoint; integration gated `#[ignore]` if `INDRADB_ENDPOINT` unset.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer sink::indradb`
  - integration: `INDRADB_ENDPOINT=http://localhost:27615 cargo nextest run --test sink_indradb -- --ignored`
- References: ADR-2.
- Parallel-safe: **yes**.

---

### S13-pipeline-orchestration-m1-gate

- Title: `pipeline::run()` orchestration + `cxg-index` CLI + M1 isomorphic-graph gate.
- AC covered: AC-M1-25, AC-M1-26, AC-M1-27 (M1 exit gate).
- Files to touch: `src/pipeline/mod.rs`, `src/pipeline/progress.rs` (stub), `src/bin/index.rs`, `tests/integration/m1_exit_gate.rs`, `tests/fixtures/m1_5file/golden_graph.json`
- Tests: integration — run full pipeline on 5-file fixture against (a) Neo4j and (b) IndraDB; assert node count, edge count, and a subset of USR-keyed nodes match golden snapshot. Diff output identifies missing/extra.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer --test m1_exit_gate -- --ignored` (requires both sinks up)
- References: requirements.md M1-S7; design.md §Phase 4.
- Parallel-safe: **no** (M1 milestone gate).

---

### S14-visit-cpp-extensions

- Title: C++ extension nodes + edges (NAMESPACE, TEMPLATE_DECL, SPECIALIZATION, TYPEDEF, ENUM, HEADER + INCLUDES/OVERRIDES/INSTANTIATES/SPECIALIZES/FRIEND_OF/ADL_CANDIDATE).
- AC covered: AC-M2-1..AC-M2-12.
- Files to touch: `src/schema/nodes.rs` (extend), `src/schema/edges.rs` (extend), `src/visit/shallow.rs` (extend), `src/visit/cursor_map.rs` (extend)
- Tests: extend `tests/integration/phase1_base.rs` with a fixture containing each construct; assert one node/edge per case; verify `vtable_slot` populated from `clang_getOverriddenCursors`.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer --test phase1_base`
- References: requirements.md M2-S1, M2-S2.
- Parallel-safe: **yes** (disjoint from S15).

---

### S15-system-header-filter

- Title: `skip_system_headers` config flag + unresolved-ref handling.
- AC covered: AC-M2-13, AC-M2-14, AC-M2-15.
- Files to touch: `src/config/mod.rs` (add field), `src/visit/shallow.rs` (filter), `src/resolve/per_repo.rs` (ensure `resolved=false` for unresolved)
- Tests: integration — fixture including `/usr/include/<header>` use; with `skip_system_headers: true` no sys nodes; with `false` they appear; unresolved ref produces `resolved=false` edge.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer system_header`
- References: requirements.md M2-S3.
- Parallel-safe: **yes**.

---

### S16-m2-boost-optional-gate

- Title: M2 exit gate — Boost.Optional fixture.
- AC covered: AC-M2-16, AC-M2-17.
- Files to touch: `tests/fixtures/boost_optional/` (vendored header + cc.json), `tests/integration/m2_exit_gate.rs`
- Tests: index fixture; assert zero `cross_repo_candidate=true` edges; assert template specialisation nodes + `SPECIALIZES` edges for `boost::optional<T>` exist.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer --test m2_exit_gate -- --ignored`
- References: requirements.md M2-S4.
- Parallel-safe: **no**.

---

### S17-parallel-phase1

- Title: Rayon-parallel Phase 1 with thread-local `clang::Index` per ADR-7.
- AC covered: AC-M3-1, AC-M3-2, AC-M3-3.
- Files to touch: `src/pipeline/parallel.rs`, `src/visit/shallow.rs` (use `with_thread_index`)
- Tests: integration — measurable wall-time speedup vs sequential on a 50-TU synthetic fixture; `panic::catch_unwind` test simulating libclang panic with stub leaves siblings running and increments `cxg_libclang_errors_total`.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer pipeline::parallel`
  - benchmark gate (manual): `BENCH=1 cargo bench --bench llvm_index` ≤15 min on 32-core target host.
- References: ADR-7.
- Parallel-safe: **partial** (touches `pipeline/` and `visit/shallow.rs`; conflicts with S19 → run sequentially within W9 or pair-merge).

---

### S18-batched-sink-writes

- Title: Batched UNWIND/insert writes — Neo4j ≥50k/s, IndraDB ≥100k/s.
- AC covered: AC-M3-4, AC-M3-5, AC-M3-6.
- Files to touch: `src/sink/neo4j.rs` (batch loop, configurable `batch_size`/`sessions`), `src/sink/indradb.rs` (batch + retry), `src/config/mod.rs` (defaults `batch_size=10000`, `sessions=16`)
- Tests: throughput micro-bench in `benches/sink_throughput.rs` gated `BENCH=1`; idempotency: write same batch twice, assert one row per `usr`.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer sink::`
  - bench: `BENCH=1 cargo bench --bench sink_throughput`
- References: ADR-2 §Concurrency.
- Parallel-safe: **yes**.

---

### S19-content-hash-cache

- Title: Content-hash incremental cache (TU skip on hit).
- AC covered: AC-M3-7, AC-M3-8, AC-M3-9, AC-M3-10.
- Files to touch: `src/stage/manifest.rs` (extend with cache entry: `source_hash`, `args_hash`, `libclang_version`, `schema_version`, `output_shards`), `src/pipeline/mod.rs` (pre-pass filter)
- Tests: integration — (a) second run with no changes → 0 TUs parsed + exit ≤30 s; (b) edit one source → only affected TU re-parsed; (c) libclang/schema version bump → full invalidation.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer --test incremental_cache`
- References: requirements.md M3-S3; ADR-3 §`manifest.json`.
- Parallel-safe: **partial** (touches `pipeline/mod.rs` — coordinate with S17).

---

### S20-memory-spill-progress

- Title: USR-map RocksDB spill + stderr progress reporter.
- AC covered: AC-M3-11, AC-M3-12, AC-M3-13, AC-M3-14.
- Files to touch: `src/resolve/spill.rs`, `src/resolve/per_repo.rs` (delegate above threshold), `src/pipeline/progress.rs` (full impl)
- Tests: unit — spill threshold triggered with synthetic 8GB+ map (mock the size check); progress reporter emits ≥1 line/5s; cache-hit TUs counted immediately.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer resolve::spill pipeline::progress`
- References: ADR-7 §memory; ADR-8 (spill threshold).
- Parallel-safe: **no** (touches `pipeline/` after S17/S19).

---

### S21-m3-perf-gate

- Title: M3 perf benchmark harness + LLVM gate.
- AC covered: AC-M3-3 (LLVM ≤15 min), AC-M3-4/5 (throughput), AC-M3-10 (incremental ≤1 min), AC-M3-11 (≤16 GB RSS).
- Files to touch: `benches/llvm_index.rs`, `tests/fixtures/llvm_checkout.md` (acquisition checklist — not the source itself)
- Tests: manual gate on tuned hardware; results recorded in `test-report.md` per CHARTER.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `BENCH=1 cargo bench --bench llvm_index` (gate fails if any AC breached; numbers persisted)
- References: requirements.md M3 exit criterion.
- Parallel-safe: **no**.

---

### S22-repo-nodes

- Title: `REPO` node + `BELONGS_TO_REPO` edges + `sink` attribution.
- AC covered: AC-M4-1, AC-M4-2, AC-M4-3.
- Files to touch: `src/bootstrap/repo_meta.rs` (git2 `rev-parse` for sha + commit date), `src/schema/nodes.rs` (add `REPO`), `src/schema/edges.rs` (add `BELONGS_TO_REPO`), `src/pipeline/mod.rs` (emit REPO + tag every node)
- Tests: integration — run pipeline against git repo fixture; assert `REPO` node carries `name`, `root_path`, `commit_sha`, `commit_date`, `sink`; every other node has `BELONGS_TO_REPO` edge.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer repo_meta`
- References: requirements.md M4-S1.
- Parallel-safe: **no** (foundation for S23/S24).

---

### S23-cross-repo-resolver-bin

- Title: `cxg-resolve-cross-repo` binary + Phase 5 `EXTERNAL_REF` materialisation + advisory lock + schema-version refuse + mixed-backend refuse.
- AC covered: AC-M4-4, AC-M4-5, AC-M4-6 + AC-M4-3 enforcement + AC-M6-7 refuse-on-version-mismatch.
- Files to touch: `src/resolve/cross_repo.rs`, `src/bin/resolve_cross_repo.rs`, `src/sink/lock.rs` (Phase5LockGuard impls for both backends)
- Tests: integration — two-repo fixture in shared DB; run binary; assert `EXTERNAL_REF` edges with `via:CALLS`; unresolved edges retain `resolved=false`; second concurrent invocation blocks on lock; mismatched `SchemaVersion` → refuse; mixed sink attribution → refuse.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer --test cross_repo -- --ignored`
- References: ADR-2 §lock, ADR-9 (schema version refuse), design.md §Phase 5.
- Parallel-safe: **no**.

---

### S24-syshdr-canonicalisation

- Title: System-header USR canonicalisation + vendored-pkg pinning.
- AC covered: AC-M4-7, AC-M4-8.
- Files to touch: `src/resolve/cross_repo.rs` (extend with canonicaliser), new `src/resolve/canonical.rs` (path-rule matcher + override list per ADR-4)
- Tests: unit — USR under `/usr/include/**` → `repo:system:libstdc++`; under compiler-internal path → `repo:system:libc`; under `third_party/<pkg>/` → `repo:vendored:<pkg>`; override-list entry takes precedence.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer resolve::canonical`
- References: ADR-4.
- Parallel-safe: **no** (touches `resolve/cross_repo.rs` shared with S23).

---

### S25-m4-two-repo-gate

- Title: M4 exit gate — two-repo `EXTERNAL_REF` Cypher test.
- AC covered: AC-M4-9, AC-M4-10.
- Files to touch: `tests/fixtures/two_repo/{lib-a,lib-b}/`, `tests/integration/m4_exit_gate.rs`
- Tests: index both fixtures + run `cxg-resolve-cross-repo`; execute Cypher `MATCH p=()-[:EXTERNAL_REF]->() RETURN p LIMIT 1`; assert one path; assert `via=CALLS`.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer --test m4_exit_gate -- --ignored`
- References: requirements.md M4-S4.
- Parallel-safe: **no**.

---

### S26-macros

- Title: `MACRO` nodes + `EXPANDS_TO` edges (top-level only).
- AC covered: AC-M5-1, AC-M5-2, AC-M5-3, AC-M5-4.
- Files to touch: `src/visit/macros.rs`, `src/schema/nodes.rs` + `edges.rs` (add MACRO + EXPANDS_TO), `src/visit/shallow.rs` (hook)
- Tests: integration with `.def`-style fixture; assert edge-count bound `≤10× source-lines`; nested expansions not duplicated.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer visit::macros`
- References: requirements.md M5-S1.
- Parallel-safe: **yes** (disjoint from S27/S28).

---

### S27-phase2-decorate

- Title: Phase 2 optional deep decoration behind `--skip-phase2`.
- AC covered: AC-M5-5, AC-M5-6.
- Files to touch: `src/visit/decorate.rs`, `src/bin/index.rs` (clap flag), `src/pipeline/mod.rs` (conditionally invoke)
- Tests: unit — decorate phase emits control-flow + exception-spec annotations into Parquet `attrs_json`; `--skip-phase2` short-circuits.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer visit::decorate`
- References: requirements.md M5-S2.
- Parallel-safe: **yes**.

---

### S28-cpp20-modules

- Title: C++20 modules with runtime capability probe (per ADR-8).
- AC covered: AC-M5-7, AC-M5-8, AC-M5-9.
- Files to touch: `src/visit/modules_cpp20.rs`, `src/bin/index.rs` (`--version` output emits capability note)
- Tests: unit — `probe_cpp20_support()` returns bool; when false, `.cppm` TU logs warning + skips; when true, emits nodes for exported decls.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer visit::modules_cpp20`
- References: ADR-8.
- Parallel-safe: **yes**.

---

### S29-m5-chromium-gate

- Title: M5 exit gate — Chromium `base/` + `net/` subset.
- AC covered: AC-M5-10, AC-M5-11.
- Files to touch: `tests/fixtures/chromium_subset.md` (acquisition checklist), `tests/integration/m5_exit_gate.rs`
- Tests: index Chromium subset; assert exit 0, no segfault; ≥1 MACRO + ≥1 EXPANDS_TO in Parquet.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer --test m5_exit_gate -- --ignored`
- References: requirements.md M5-S4.
- Parallel-safe: **no**.

---

### S30-prompt-codegen

- Title: `build.rs`-driven `prompt/graph_database/cpp/schema.txt` regeneration + CI drift gate + idiom examples file.
- AC covered: AC-M6-1, AC-M6-2, AC-M6-3, AC-M6-4, AC-M6-5.
- Files to touch: `build.rs` (full impl invoking `src/prompt/codegen.rs`), `src/prompt/codegen.rs`, `prompt/graph_database/cpp/schema.txt` (initial committed output), `prompt/graph_database/cpp/example.txt` (hand-authored idioms: template, override, namespace, include), `.github/workflows/ci.yml` (add `git diff --exit-code prompt/` step)
- Tests: unit — codegen output stable across runs; CI step `git diff --exit-code prompt/graph_database/cpp/schema.txt` fails on drift.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo build` (regenerates schema.txt) then `git diff --exit-code prompt/`
  - `cargo nextest run -p cpp_indexer prompt::`
- References: ADR-1; requirements.md M6-S1, M6-S2.
- Parallel-safe: **yes** (disjoint from S31).

---

### S31-schema-version-mcp-handshake

- Title: `SchemaVersion` node write at every index + Phase 5 / cpp-mcp version-mismatch refuse.
- AC covered: AC-M6-6, AC-M6-7 (producer side only; cpp-mcp consumer is out of repo per ADR-1).
- Files to touch: `src/schema/version.rs` (extend with helper), `src/pipeline/mod.rs` (write `SchemaVersion` node on each run), `src/resolve/cross_repo.rs` (refuse on mismatch — overlap with S23 already implemented; this story formalises test coverage)
- Tests: integration — graph contains `SchemaVersion` node post-index; Phase 5 refuses when fixture repo carries an older version.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer schema_version`
- References: ADR-9.
- Parallel-safe: **yes**.

---

### S32-m6-agent-gate

- Title: M6 exit gate — Streamlit agent inheritance query (manual + scripted).
- AC covered: AC-M6-8, AC-M6-9.
- Files to touch: `tests/integration/m6_agent_gate.md` (manual procedure pointing at external CodexGraph Streamlit agent), `tests/integration/m6_nl_eval.json` (10 NL questions + expected answer fragments)
- Tests: manual run (agent not in this repo); results recorded in `test-report.md`.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - manual gate: ≥8/10 NL answers graded correct; documented in test-report.md.
- References: requirements.md M6-S4; ADR-1 (cpp-mcp boundary).
- Parallel-safe: **no**.

---

### S33-daemon-rest

- Title: `cxg-daemon` binary + axum routes + bearer-token middleware + RFC-7807 + jobs state machine.
- AC covered: AC-M7-1..AC-M7-8.
- Files to touch: `src/api/mod.rs`, `src/api/routes.rs` (`POST /v1/ingest`, `GET /v1/jobs[/{id}]`, `GET /v1/status`, `GET /v1/repos`), `src/api/auth.rs`, `src/api/problem.rs`, `src/api/jobs.rs`, `src/bin/daemon.rs`
- Tests: integration via `tower::ServiceExt::oneshot` (no port bind) — (a) `POST /v1/ingest` returns 202 + job_id p99 ≤50 ms; (b) `GET /v1/jobs/{id}` returns state/phase/progress p99 ≤20 ms; (c) writes without bearer → 401; (d) errors are `application/problem+json`; (e) `GET /v1/repos` returns expected fields; (f) `[api].listen` honoured.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer api::`
- References: ADR-5.
- Parallel-safe: **yes** (api/ disjoint from workspace/ + metrics).

---

### S34-daemon-reset

- Title: `POST /v1/reset` with sha256 confirmation token.
- AC covered: AC-M7-9, AC-M7-10, AC-M7-11.
- Files to touch: `src/api/routes.rs` (add reset handler), `src/sink/mod.rs` (`ResetTarget` enum already in S08 trait; ensure impls invoke)
- Tests: integration — correct token + target=repo deletes only that repo's nodes + clears its staging cache; wrong/missing token → 400; target=all wipes everything.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer api::reset`
- References: requirements.md M7-S2; ADR-5.
- Parallel-safe: **yes** (touches routes.rs — coordinate with S35 if same wave; otherwise serialise).

---

### S35-daemon-metrics

- Title: Prometheus `GET /metrics` + 429 on queue-full.
- AC covered: AC-M7-17, AC-M7-18, AC-M7-19.
- Files to touch: `src/api/metrics.rs` (registry + counters/gauges), `src/api/routes.rs` (add `/metrics` route, no auth), `src/api/jobs.rs` (queue-depth gauge, 429 on overflow)
- Tests: integration — `GET /metrics` returns expected Prom names; no auth required; `cxg_queue_depth > job_queue_max` → 429.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer api::metrics`
- References: requirements.md M7-S4; design.md §5.4.
- Parallel-safe: **yes**.

---

### S36-workspace-git-ingest

- Title: `workspace::` git2 clone manager + git_url ingest path.
- AC covered: AC-M7-12, AC-M7-13, AC-M7-14, AC-M7-15, AC-M7-16.
- Files to touch: `src/workspace/mod.rs`, `src/workspace/git.rs`, `src/workspace/allowlist.rs`, `src/workspace/layout.rs`, `src/api/routes.rs` (extend `POST /v1/ingest` to dispatch on body `source` variant)
- Tests: integration — (a) allowed host + git_url → clones to `<dir>/<repo>-<short-sha>/` + starts index; (b) repeat → `git fetch` not re-clone; (c) disallowed host → 403; (d) PAT from env, never logged (assert via captured tracing output); (e) `default_clone_depth=1` → `--depth=1`.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer workspace::`
- References: ADR-6; requirements.md M7-S3.
- Parallel-safe: **no** (extends `api/routes.rs` after S33–S35 merged).

---

### S37-docker-ci

- Title: Multi-stage `Dockerfile` (libclang 18) + CI matrix + benchmark regression gate.
- AC covered: AC-M7-20, AC-M7-21, AC-M7-22.
- Files to touch: new `Dockerfile`, `.dockerignore`, extend `.github/workflows/ci.yml` (matrix linux/macos; benchmark step gated to `main` with 20% regression check)
- Tests: `docker build .` produces image containing all 3 binaries; CI runs `cargo nextest run --all-targets` on both OS; benchmark step compares to baseline json artifact.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `docker build -t cpp-indexer:ci .` (smoke: `docker run --rm cpp-indexer:ci cxg-index --version` returns 0)
- References: requirements.md M7-S5; design.md §5.6, §5.7.
- Parallel-safe: **yes**.

---

### S38-runbook

- Title: Operator runbook for corrupted staging recovery + reset token derivation.
- AC covered: AC-M7-23, AC-M7-24.
- Files to touch: `/Users/husam/workspace/cpp-indexer/.claude/handoff/v1/runbook.md` (will be authored by devops stage; senior-developer story scope is the markdown content draft committed to `docs/runbooks/staging-recovery.md`)
- Tests: doc review — every step is executable and reproducible by a clean operator.
- Exit criteria:
  - `cargo fmt --all -- --check` (no Rust changes; gate still required)
  - markdown link-check: `npx --yes markdown-link-check docs/runbooks/staging-recovery.md`
- References: requirements.md M7-S6.
- Parallel-safe: **yes**.

---

### S39-m7-soak-gate

- Title: M7 / v1 GA exit gate — 7-day daemon soak + git-URL round-trip.
- AC covered: AC-M7-25, AC-M7-26, AC-M7-27.
- Files to touch: `tests/integration/m7_soak_checklist.md`, `tests/integration/m7_git_roundtrip.rs`
- Tests: (a) automated git-URL round-trip against a small public repo; assert `state=done` + nodes tagged correctly; (b) manual 7-day soak on hermes-agent — `GET /v1/status` 200 throughout, `cxg_libclang_errors_total / cxg_nodes_total < 0.01` at end.
- Exit criteria:
  - `cargo fmt --all -- --check`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `cargo nextest run -p cpp_indexer --test m7_git_roundtrip -- --ignored`
  - manual soak signed off in test-report.md per CHARTER I4.
- References: requirements.md M7-S7.
- Parallel-safe: **no**.

---

## Risks and out-of-scope

- **libclang 18 segfault on Chromium fixtures** — mitigated by `catch_unwind` per ADR-7; risk acknowledged in design.md §8.
- **Neo4j 50k/s throughput unverified on team hardware** — risk, not redesign trigger (design.md §8).
- **Bazel `compile_commands.json` probing** — explicitly out of scope (Q8 / requirements.md).
- **SSH git auth** — deferred to v2 (Q6).
- **C++20 modules** — best-effort only, runtime probe (ADR-8); falls back to skip-with-warning.
- **Multi-tenant daemon** — single-tenant for v1 (ADR-10).
- **7-day soak** — manual gate, not automated; coordinator + devops own scheduling.

## References

- requirements.md (AC IDs)
- design.md §3 (module layout), §4 (phase contracts), §5 (cross-cutting)
- adr-1.md … adr-10.md (all accepted 2026-05-17)
- CHARTER.md (worktree convention, failure taxonomy, traceability)
- Cognee tags: `task:cpp-indexer role:senior-developer`
