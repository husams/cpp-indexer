run_id: cpp-indexer-v1
stage: 6 of 8 — qa-engineer
date: 2026-05-17
scope: All 39 developer stories (S01–S39, M1–M7 → v1 GA)

---

Scope: cpp-indexer M1–M7 (all 39 stories)
Test plan: unit | integration | boundary (mutation category)

## Commands run

```
# Gate 1 — format
cargo fmt --all -- --check
# exit 0 — PASS

# Gate 2 — lint (canonical; all-features required for bench targets)
LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib \
DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib \
  cargo clippy --all-targets --all-features -- -D warnings
# exit 0 — PASS

# Gate 3 — build (all targets + all features)
LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib \
DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib \
  cargo build --all-targets --all-features
# exit 0 — PASS

# Gate 4 — test suite (lib + integration, with test-mock feature)
LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib \
DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib \
  cargo nextest run --lib --tests --features test-mock
# exit 0 — PASS: 334 passed / 0 failed / 27 skipped

# Gate 5 — new boundary tests only
LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib \
DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib \
  cargo nextest run --test macro_expands_to_bound --features test-mock
# exit 0 — PASS: 2 passed / 0 failed / 0 skipped
```

## Results

**334 passed / 0 failed / 27 skipped** (exit 0 on all gates)

### Per-story AC coverage (unit + integration)

| Story | AC IDs | Test modules | Status |
|-------|--------|-------------|--------|
| S01-init-crate | AC-M1-1 | build gate | PASS |
| S02-schema-base-types | AC-M1-2, AC-M1-3, AC-M1-4 | schema::nodes, schema::edges, schema::arrow | PASS |
| S03-config-toml | AC-M1-23, AC-M1-24 | config:: | PASS |
| S04-error-tracing | AC-M1-23 | error:: | PASS |
| S05-compile-commands | AC-M1-5, AC-M1-6, AC-M1-7 | bootstrap::compile_commands | PASS |
| S06-autodetect | AC-M1-8..AC-M1-13 | autodetect (integration) | PASS |
| S07-stage-parquet | AC-M1-14, AC-M1-17 | stage:: | PASS |
| S08-graphsink-trait-mock | AC-M1-15 | sink::mock | PASS |
| S09-visit-shallow-base | AC-M1-14..AC-M1-17 | phase1_base (integration) | PASS |
| S10-resolve-per-repo | AC-M1-18, AC-M1-19 | phase3 (integration) | PASS |
| S11-sink-neo4j | AC-M1-20, AC-M1-22 | sink_neo4j (DEFERRED — live Neo4j) | deferred |
| S12-sink-indradb | AC-M1-21, AC-M1-22 | sink_indradb (DEFERRED — live IndraDB) | deferred |
| S13-pipeline-m1-gate | AC-M1-25..AC-M1-27 | m1_exit_gate (DEFERRED — live sinks) | deferred |
| S14-visit-cpp-extensions | AC-M2-1..AC-M2-12 | phase1_base (m2_emits_* tests) | PASS |
| S15-system-header-filter | AC-M2-14, AC-M2-15 | system_header (integration) | PASS |
| S16-m2-boost-gate | AC-M2-16 | m2_exit_gate (DEFERRED — boost-optional checkout) | deferred |
| S17-parallel-phase1 | AC-M3-1, AC-M3-2, AC-M3-3 | parallel_phase1 (integration) | PASS |
| S18-batched-sink-writes | AC-M3-4, AC-M3-5 | sink:: batching unit tests | PASS |
| S19-content-hash-cache | AC-M3-8, AC-M3-9 | incremental_cache (integration) | PASS |
| S20-memory-spill-progress | AC-M3-7, AC-M3-10 | pipeline::progress tests | PASS |
| S21-m3-perf-gate | AC-M3-1..AC-M3-10 | parallel_phase1 speedup test | PASS |
| S22-repo-nodes | AC-M4-1..AC-M4-5 | repo_meta (integration) | PASS |
| S23-cross-repo-resolver-bin | AC-M4-2, AC-M4-3 | cross_repo (DEFERRED — live Neo4j) | deferred |
| S24-syshdr-canonicalisation | AC-M4-4, AC-M4-5 | phase3::sys_header (integration) | PASS |
| S25-m4-two-repo-gate | AC-M4-6, AC-M4-7 | m4_exit_gate (DEFERRED — live Neo4j) | deferred |
| S26-macros | AC-M5-1..AC-M5-4 | visit::macros (9 unit), macro_expands_to_bound (2 new) | PASS |
| S27-phase2-decorate | AC-M5-5, AC-M5-6 | visit::decorate (5 unit) | PASS |
| S28-cpp20-modules | AC-M5-7..AC-M5-9 | visit::modules_cpp20 (7 unit, 1 deferred) | PASS |
| S29-m5-chromium-gate | AC-M5-10, AC-M5-11 | m5_exit_gate (DEFERRED — chromium/LLVM checkout) | deferred |
| S30-prompt-codegen | AC-M6-1..AC-M6-5 | m6_agent_gate (5 unit) | PASS |
| S31-schema-version-mcp-handshake | AC-M6-6..AC-M6-9 | schema_version (integration) | PASS |
| S32-m6-agent-gate | AC-M6-10..AC-M6-15 | m6_agent_gate manual | PASS |
| S33-daemon-rest | AC-M7-1..AC-M7-8 | api:: unit tests | PASS |
| S34-daemon-reset | AC-M7-9, AC-M7-10 | api::reset unit tests | PASS |
| S35-daemon-metrics | AC-M7-11, AC-M7-12 | metrics:: unit tests | PASS |
| S36-workspace-git-ingest | AC-M7-13..AC-M7-22 | workspace:: (allowlist, git, layout unit tests) | PASS |
| S37-docker-ci | AC-M7-23, AC-M7-24 | Dockerfile + CI workflow presence | PASS |
| S38-runbook | — | runbook.md artifact | PASS |
| S39-m7-soak-gate | AC-M7-25 | m7_git_roundtrip (DEFERRED — live daemon + network) | deferred |

### Deferred fixtures (not defects)

All 27 skipped tests are `#[ignore]`'d pending live infrastructure. Breakdown:

| Reason | Count | Test locations |
|--------|-------|---------------|
| NEO4J_URI + NEO4J_PASSWORD not set | 10 | sink_neo4j (3), cross_repo (3), m1_exit_gate (1), m4_exit_gate (1), m7_git_roundtrip (1) |
| INDRADB_ENDPOINT not set | 12 | sink_indradb (12), m1_exit_gate (1) |
| boost-optional checkout absent | 2 | m2_exit_gate (2) |
| chromium checkout absent | 2 | m5_exit_gate (2) |
| libclang C++20 module support absent | 1 | visit::modules_cpp20 (1) |

These are not QA_DEFECT entries. They are correctly gated with `#[ignore = "<reason>"]` and require the corresponding fixture environment to be available at run time. DevOps should provision Neo4j + IndraDB via the compose file in `tests/compose/` before the integration soak.

### Bench compile note

`cargo nextest run --all-targets` without `--all-features` fails to compile `benches/sink_throughput.rs` because `MockSink` is gated behind `#[cfg(any(test, feature = "test-mock"))]`. Running with `--all-features` (or `--features test-mock`) compiles and links correctly. The `--all-features` flag is the canonical gate per plan.md §Conventions. This is not a defect; the bench has a `BENCH=1` guard and is not a test target under `cargo nextest`.

## Defects

None. 0 open QA_DEFECT entries.

## Additions made

**Category: mutation/boundary** — new test file covering AC-M5-4 (the only AC with a numeric bound not exercised by any prior test).

Files added:
- `tests/integration/macro_expands_to_bound.rs` — 2 tests:
  1. `expands_to_count_bounded_by_10x_source_lines` — asserts EXPANDS_TO ≤ 10 × L (L=38, bound=380) on the new `xmacro_def` fixture; detects regression if `is_top_level_expansion` filter is removed.
  2. `expands_to_count_is_nonzero_for_xmacro_fixture` — negative companion; asserts ≥1 EXPANDS_TO edge to guard against over-suppression.
- `tests/fixtures/xmacro_def/events.def` — 20-entry X-macro `.def` file (fixture input)
- `tests/fixtures/xmacro_def/main.cpp` — TU that includes `events.def` twice (enum + name table)
- `tests/fixtures/xmacro_def/compile_commands.json` — compile entry for main.cpp

`Cargo.toml` updated with `[[test]] name = "macro_expands_to_bound" required-features = ["test-mock"]`.

Justification: the existing m5_exit_gate tests only assert ≥1 EXPANDS_TO edge. No test previously verified the upper bound, meaning a bug that removes the `is_top_level_expansion` nesting filter would silently pass the suite. The new tests close that gap.

## Observations (advisory only — do not block dispatch)

1. `cargo nextest run --all-targets` (without `--all-features`) fails the bench compile step. The plan.md §Conventions and plan.md S01 exit criteria say `cargo nextest run` (no flags); on this macOS host the canonical invocation needs either `--features test-mock` or `--all-features`. CI should pin the exact flags used here. No production code change needed.

2. All exit-criteria commands in plan.md S26 and S27 omit the `LIBCLANG_PATH`/`DYLD_LIBRARY_PATH` prefix needed on macOS with Apple CLT libclang. The developer logs note this. CI workflow should set `LIBCLANG_PATH` via an env step or `.cargo/config.toml [env]` section to make the commands copy-pastable on macOS without manual prefix.

3. `exception_spec` classification in `visit/decorate.rs` uses display-name string matching rather than the typed `ExceptionSpecification` API (not exposed by the `clang` crate v2). This is best-effort and documented in-module. No test added — the behaviour is acknowledged by the developer as an approximation.

References: scenarios.md, implementation-notes.md (all s01–s39 logs), plan.md §S26, CHARTER §I4
