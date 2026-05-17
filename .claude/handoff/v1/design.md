# cpp-indexer — System Design (M1–M7 → v1 GA)

run_id: cpp-indexer-v1
stage: 3 of 8 — architect
version: 1.0
date: 2026-05-17
upstream: requirements.md v1.0, scenarios.md v1.0
downstream: senior-developer reads this + all adr-*.md to produce plan.md
references:
  - PRD v1.1 (~/workspace/wiki/pages/planning/codexgraph-cpp-prd-v1.md)
  - Engineering plan v1.1 (~/workspace/wiki/pages/planning/codexgraph-cpp-libclang-rust.md)
  - CHARTER (./CHARTER.md)

---

## 1. Goals and constraints (recap, non-litigable)

- Rust 2021 + libclang 18; both Neo4j and IndraDB sinks built into default binary, runtime-selected.
- USR (`clang_getCursorUSR`) is the global primary key; mangled name secondary.
- Parquet shards stage Phase 1/2 output; DB writes only in Phase 4 and Phase 5.
- Binaries: `cxg-index`, `cxg-resolve-cross-repo`, `cxg-daemon`.
- `compile_commands.json` auto-detected by upward walk + `build/`, `out/`, `cmake-build-*/` probe, stopping at `.git` or FS root; hard error on miss.
- Daemon REST control plane with bearer-token on writes; git-URL ingest via host allowlist.
- Public artifacts use **cpp-indexer** name; internal slug `codexgraph-cpp` retained only in wiki planning docs.

## 2. Top-level architecture

```
                       ┌─────────────────────────────────────┐
                       │           cxg-daemon                │
                       │ (axum REST + job queue + workers)   │
                       └──────────────┬──────────────────────┘
                                      │  spawns library calls (NOT subprocesses)
                                      ▼
   cxg-index ──────────►   cpp_indexer::pipeline::run(repo, sink, opts)
                                      │
                          ┌───────────┴───────────────────────────────────┐
                          ▼                                               ▼
   Phase 0  ──►  Phase 0.5 ──► Phase 1 ──► Phase 2 ──► Phase 3 ──► Phase 4
   (bootstrap)   (autodetect)  (libclang   (decorate   (in-mem     (GraphSink
                                visit,      opt)        USR map    batched
                                Parquet)                resolve)    write)
                                                                     │
                          (after every repo has reached Phase 4)     │
                                                                     ▼
                                                 cxg-resolve-cross-repo
                                                       (Phase 5: EXTERNAL_REF)
```

All phases share one in-process binary; phase boundaries are functions, not OS processes. `cxg-index` and `cxg-daemon` link the same `cpp_indexer` library crate.

## 3. Crate and module layout

Single library crate `cpp_indexer` + three thin bin crates. Per Rust convention (binary logic lives in `lib.rs`).

```
cpp-indexer/
├── Cargo.toml                         # workspace = false; single crate
├── src/
│   ├── lib.rs                         # re-exports public surface
│   ├── error.rs                       # crate-local `Error` enum (thiserror)
│   ├── config/
│   │   ├── mod.rs                     # cxg-index.toml + cxg-daemon.toml schemas
│   │   └── env.rs                     # `password_env`, `token_env`, `auth_token_env`
│   ├── schema/
│   │   ├── mod.rs
│   │   ├── nodes.rs                   # NodeKind enum + per-kind struct
│   │   ├── edges.rs                   # EdgeKind enum + Edge struct
│   │   ├── arrow.rs                   # Arrow schemas + enum<->record round-trip
│   │   └── version.rs                 # SCHEMA_VERSION const + SchemaVersion node
│   ├── bootstrap/
│   │   ├── compile_commands.rs        # Phase 0: parse + dedup by Blake3(file,args)
│   │   ├── autodetect.rs              # Phase 0.5: FR-AD-* upward walk
│   │   └── repo_meta.rs               # REPO node attrs (git rev-parse via git2)
│   ├── visit/
│   │   ├── mod.rs
│   │   ├── shallow.rs                 # Phase 1 visitor
│   │   ├── decorate.rs                # Phase 2 (opt, --skip-phase2)
│   │   ├── cursor_map.rs              # libclang CXCursorKind → NodeKind
│   │   ├── macros.rs                  # MACRO + EXPANDS_TO
│   │   └── modules_cpp20.rs           # .cppm/.pcm handling (see ADR-4)
│   ├── stage/
│   │   ├── writer.rs                  # Per-worker Parquet shard writer
│   │   ├── manifest.rs                # content-hash cache (Blake3)
│   │   └── schema.rs                  # Parquet schema constants (see ADR-3)
│   ├── resolve/
│   │   ├── per_repo.rs                # Phase 3: HashMap<USR,NodeMeta>
│   │   ├── spill.rs                   # RocksDB spill when map > 8 GB
│   │   └── cross_repo.rs              # Phase 5: EXTERNAL_REF materialisation
│   ├── sink/
│   │   ├── mod.rs                     # GraphSink trait (see ADR-2)
│   │   ├── neo4j.rs                   # Neo4jSink (neo4rs)
│   │   ├── indradb.rs                 # IndraDbSink (indradb + indradb-proto)
│   │   ├── factory.rs                 # config.sink.backend → Box<dyn GraphSink>
│   │   └── lock.rs                    # Phase 5 advisory lock
│   ├── pipeline/
│   │   ├── mod.rs                     # run(opts) — orchestrates all phases
│   │   ├── progress.rs                # stderr reporter (AC-M3-13)
│   │   └── parallel.rs                # rayon pool + thread-local clang::Index
│   ├── api/                           # cxg-daemon only
│   │   ├── mod.rs
│   │   ├── routes.rs                  # /v1/ingest, /v1/jobs, /v1/reset, /v1/repos, /v1/status, /metrics
│   │   ├── auth.rs                    # bearer-token middleware
│   │   ├── problem.rs                 # RFC-7807 problem+json (see ADR-5)
│   │   ├── jobs.rs                    # in-process queue + state machine
│   │   └── metrics.rs                 # Prometheus registry
│   ├── workspace/                     # cxg-daemon only
│   │   ├── mod.rs
│   │   ├── git.rs                     # git2 clone/fetch (see ADR-6)
│   │   ├── allowlist.rs               # host-suffix allowlist
│   │   └── layout.rs                  # <dir>/<repo-name>-<short-sha>/
│   └── prompt/
│       └── codegen.rs                 # build-script-invoked schema.txt regen (ADR-1)
├── src/bin/
│   ├── index.rs                       # cxg-index — clap CLI → pipeline::run
│   ├── resolve_cross_repo.rs          # cxg-resolve-cross-repo
│   └── daemon.rs                      # cxg-daemon — axum + jobs
├── build.rs                           # regenerates prompt/.../schema.txt at build
├── prompt/
│   └── graph_database/cpp/
│       ├── schema.txt                 # generated; CI verifies committed == generated
│       └── example.txt                # hand-authored idiom examples
└── tests/
    ├── fixtures/                      # 5-file (M1), Boost.Optional (M2), 2-repo (M4), Chromium subset (M5)
    └── integration/                   # full pipeline tests, both sinks
```

## 4. Phase contracts

### Phase 0 — Bootstrap (`bootstrap::compile_commands`)
- Input: a resolved `compile_commands.json` path (from Phase 0.5) + input scope (file/dir/repo).
- Output: `Vec<TuEntry { file: PathBuf, args: Vec<String>, hash: blake3::Hash }>`.
- Dedup key: `blake3(file_canonical || NUL || args.join(NUL))`. AC-M1-5, AC-M1-6.
- Scope filter: AC-M1-12 (dir → entries under dir), AC-M1-13 (file → exact match).

### Phase 0.5 — Auto-detect (`bootstrap::autodetect`)
- Strategy: from input path, walk upward; at each level probe `compile_commands.json` directly, then `build/`, `out/`, `cmake-build-*/` (glob). Stop at `.git` directory or FS root (AC-M1-10).
- Multiple candidates at same level: prefer one whose entry list contains the input file (AC-M1-11); else lexicographic.
- Failure: list every directory probed, exit non-zero (AC-M1-9). **No heuristic fallback.**

### Phase 1 — Shallow walk (`visit::shallow`)
- Parallel: `rayon::par_iter` over `TuEntry` with thread-local `clang::Index`. Each worker owns a `clang::Index` created via `Box::leak` for `'static` (see ADR-7 / pipeline::parallel).
- Per-cursor: emit node/edge events to per-worker Parquet shard via `stage::writer`.
- Edges with unresolved targets emit `target_usr` as the syntactic placeholder (USR string from `clang_getCursorReferenced` if any; else recorded as `unresolved=true`).
- Cache check before parse: if `manifest` has `(blake3(source), blake3(args), libclang_version, SCHEMA_VERSION)` → skip TU, count as done (AC-M3-7..10).
- Failure isolation: `panic::catch_unwind` wraps each TU parse; failure recorded as diagnostic, run continues (AC-M1-16, AC-M3-2).
- No DB I/O at all (AC-M1-15).

### Phase 2 — Decorate (`visit::decorate`, opt)
- Default: ON. `--skip-phase2` flag turns off (AC-M5-5/6).
- Adds: control-flow summary, exception specs, constexpr eval markers, macro-expansion provenance.

### Phase 3 — In-memory resolve (`resolve::per_repo`)
- Single-threaded per repo. Load all Parquet shards → `HashMap<Usr, NodeMeta>`.
- Walk edge shards; lookup target USR:
  - Hit → write to `final-edges.parquet` with `resolved=true`.
  - Miss → write with `resolved=false, cross_repo_candidate=true` (AC-M1-19, AC-M2-13, AC-M4-2).
- Memory: if map size > 8 GB, spill to RocksDB under `.cxg-cache/usr_map.rocks` (AC-M3-12, see ADR-8 for the threshold).

### Phase 4 — Bulk write (`sink::*`)
- Driven by `Box<dyn GraphSink>` (see ADR-2).
- Pre-create indexes via `ensure_indexes()` (Neo4j: `CREATE INDEX ... ON :Node(usr)`).
- Batched UNWIND in M3+; single-tx in M1 (AC-M1-20, AC-M3-4/5).
- Idempotency: USR-keyed MERGE on Neo4j; IndraDB uses `(usr, repo_name)` composite. AC-M3-6.
- Credentials: read from env var named in `password_env`/`token_env` at startup (AC-M1-23/24). Never logged.

### Phase 5 — Cross-repo (`resolve::cross_repo`, `cxg-resolve-cross-repo`)
- Builds global `Usr → (repo, node_id)` map by querying the configured DB.
- For each repo's `cross_repo_candidate` edges: resolve, canonicalise (see ADR-4 system headers), emit `EXTERNAL_REF { via: <orig_edge_kind> }` (AC-M4-4, AC-M4-10).
- Advisory lock prevents concurrent Phase 5 races (AC-M4-6). See ADR-2 §`lock` method.
- Schema version: if any source repo's `SchemaVersion` differs from current `SCHEMA_VERSION`, refuse with error naming both versions (AC-M4-2 implicit, AC-M6-7). See ADR-9.
- Mixed-backend detection (AC-M4-3): if any `REPO` node records `sink=neo4j` and another `sink=indradb`, refuse.

## 5. Cross-cutting concerns

### 5.1 Configuration
Two TOML files, single Rust schema (`config::Config`):
- `cxg-index.toml` — Phase 0..5 settings, `[repo]`, `[sink.*]`.
- `cxg-daemon.toml` — adds `[api]`, `[workspace]`.
Both serde-derived; missing required fields → startup error with field path.

### 5.2 Secrets
- Never read secret value from a TOML field. Always read from `*_env` indirection.
- `tracing` redaction: any field named `password`, `token`, `pat`, `credentials` is replaced with `***` in log output via a `tracing_subscriber` field formatter.

### 5.3 Error model
- Library code uses `thiserror`-derived `cpp_indexer::Error` with variants:
  `Io`, `Clang`, `CompileCommands`, `Autodetect{searched:Vec<PathBuf>}`, `Sink{backend:&'static str, source:Box<dyn StdError>}`, `Schema`, `Cache`, `Workspace`, `Api`.
- Bin crates use `anyhow::Result` in `main`, mapping `Error` → exit code 1 with single-line message + stderr trace.
- Library never `unwrap()` or `expect()`. Tests are exempt.

### 5.4 Observability
- `tracing` with `tracing-subscriber` env filter (`RUST_LOG`); default INFO.
- Prometheus registry exposed by daemon at `GET /metrics` (no auth — AC-M7-18).
- Counters: `cxg_nodes_total`, `cxg_edges_total`, `cxg_libclang_errors_total`, `cxg_cache_hit_ratio` (gauge), `cxg_queue_depth` (gauge), `cxg_nodes_per_second` (gauge), `cxg_edges_per_second` (gauge). AC-M7-17.
- Progress to stderr ≥ once/5 s (AC-M3-13).

### 5.5 Testing strategy
- Unit: per-module `#[cfg(test)] mod tests`. Schema enum ↔ Arrow round-trip (AC-M1-4).
- Integration: `tests/integration/`; each milestone exit gate is a `#[test]`:
  - M1 fixture (5-file) → both sinks → isomorphic graph (AC-M1-25/26).
  - M2 Boost.Optional → zero `cross_repo_candidate=true` (AC-M2-16).
  - M3 LLVM perf benchmark (run conditionally under `BENCH=1`).
  - M4 two-repo `EXTERNAL_REF` (AC-M4-9/10).
  - M5 Chromium subset.
  - M7 daemon REST tests using `tower::ServiceExt::oneshot` (no real port bind in unit tests; integration uses ephemeral port).
- Backends in integration: docker-compose under `tests/compose/` (Neo4j 5 + IndraDB gRPC). Tests skip with `#[ignore]` + diagnostic when endpoints unreachable.
- `cargo nextest run` preferred; CI uses `cargo test --all-targets` as fallback.
- 7-day soak (AC-M7-25) is a manual checklist in `runbook.md`, not an automated test.

### 5.6 CI and build
- Linux + macOS matrix (AC-M1-1, AC-M7-21).
- Steps: `cargo fmt --check`, `cargo clippy --all-targets -- -D warnings`, `cargo nextest run`, `cargo build --release`, `cargo bench --bench llvm_index` (gated to weekly + main).
- Regression gate: >20 % wall-time slowdown on LLVM bench fails CI (AC-M7-22).
- Schema drift gate: `cargo build` regenerates `prompt/.../schema.txt`; `git diff --exit-code` fails CI if drift (AC-M6-3).

### 5.7 Docker
- Single multi-stage `Dockerfile`. Base `debian:bookworm-slim` with `libclang-18-dev`, `clang-18`. Produces image carrying all three binaries (AC-M7-20).
- Image entrypoint: `/usr/local/bin/cxg-daemon`; `cxg-index` / `cxg-resolve-cross-repo` available on PATH.
- Built on pve01/pve02 (CHARTER pre-flight resource note) when invoked through CI; never on a memory-constrained VM.

## 6. Traceability — design element → AC

| Design element | AC coverage |
|---|---|
| `schema/` enums + Arrow | AC-M1-2/3/4, AC-M2-1..12, AC-M5-1..3 |
| `bootstrap/compile_commands` | AC-M1-5/6/7 |
| `bootstrap/autodetect` | AC-M1-8..13 |
| `visit/shallow` + `stage/writer` | AC-M1-14..17, AC-M2-1..12, AC-M5-1..3 |
| `resolve/per_repo` | AC-M1-18/19, AC-M2-13 |
| `sink/` GraphSink + Neo4j + IndraDB | AC-M1-20..24, AC-M3-4..6 |
| `pipeline/parallel` | AC-M3-1..3 |
| `stage/manifest` | AC-M3-7..10 |
| `resolve/spill` | AC-M3-11/12 |
| `pipeline/progress` | AC-M3-13/14 |
| `resolve/cross_repo` + `bin/resolve_cross_repo` | AC-M4-1..10 |
| `visit/macros` | AC-M5-1..4 |
| `visit/decorate` | AC-M5-5/6 |
| `visit/modules_cpp20` | AC-M5-7..9 |
| `prompt/codegen` + `build.rs` | AC-M6-1..3 |
| `prompt/.../example.txt` | AC-M6-4/5 |
| `schema/version` + Phase 5 check | AC-M6-6/7 |
| `api/routes` + `api/auth` + `api/jobs` | AC-M7-1..8 |
| `api/routes` reset path | AC-M7-9..11 |
| `workspace/` + `api/routes` ingest path | AC-M7-12..16 |
| `api/metrics` | AC-M7-17..19 |
| `Dockerfile` + CI | AC-M7-20..22 |
| `runbook.md` (DevOps stage) | AC-M7-23/24 |
| Soak harness (manual) | AC-M7-25..27 |

## 7. ADR index

| ADR | Decision | Resolves |
|---|---|---|
| adr-1 | cpp-mcp boundary: schema prompt + drift detection live in cpp-indexer; cpp-mcp consumes via published schema.txt + version check | PRD Q5 (hard blocker), AC-M6-1..9 |
| adr-2 | `GraphSink` trait shape: async, batch-oriented, idempotent, with `ensure_indexes` and `acquire_phase5_lock` | AC-M1-20..24, AC-M3-4..6, AC-M4-6 |
| adr-3 | Parquet staging schema: one shard set per worker; columnar `Node` + `Edge` schemas with `resolved` and `cross_repo_candidate` flags | AC-M1-14..17, AC-M1-18/19, M3 perf |
| adr-4 | USR canonicalisation for system headers via path-rule prefix matcher with override list; vendored copies get `repo:vendored:<pkg>` | AC-M4-7/8 |
| adr-5 | cxg-daemon REST contract: axum + RFC-7807 + bearer-token middleware on writes only; OpenAPI generated from route enum | AC-M7-1..8, AC-M7-9..11 |
| adr-6 | git2 workspace clone manager with host-suffix allowlist, PAT-via-env, shallow-by-default, `<dir>/<repo>-<short-sha>/` layout | AC-M7-12..16 |
| adr-7 | Parallel ingestion model: rayon `par_iter` over TUs with `thread_local!` `clang::Index` leaked to `'static`; per-worker shard, no cross-worker locks | AC-M3-1..3 |
| adr-8 | C++20 modules: implement on libclang 18 best-effort, with runtime capability probe + skip-with-warning fallback | PRD Q3, AC-M5-7..9 |
| adr-9 | Cross-repo schema versioning: refuse mismatch; single monotonically-increasing integer `SCHEMA_VERSION` baked at build | PRD Q4, AC-M4-2 implicit, AC-M6-6/7 |
| adr-10 | Build-config handling (one graph per `(build_profile, defines_hash)` tuple) and multi-tenant daemon scope (single-tenant for v1) | PRD Q2 + Q7 |

## 8. Risks and follow-ups

- **libclang 18 thread-safety**: `clang::Index` is not `Send`; pipeline uses `thread_local!` per rayon worker (see ADR-7). Tested under TSAN where feasible.
- **Neo4j throughput floor 50 k rows/s** is plausible on a tuned 5.x cluster but unverified on the team's hardware. If missed, fall back to `apoc.periodic.iterate` or csv bulk import. Tracked as a risk, not a re-design.
- **IndraDB ecosystem maturity** is lower than Neo4j; if the proto client stalls, the GraphSink trait isolation lets us swap to direct gRPC without touching pipeline code.
- **Docker image size** with libclang 18 + Rust binaries will exceed 1 GiB; acceptable for a backend service. No distroless variant in v1.
- **Phase 5 single-writer assumption**: advisory lock is single-DB. Multi-DB topologies (separate Neo4j + IndraDB clusters) are out of scope for v1.
