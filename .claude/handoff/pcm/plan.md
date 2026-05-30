# PCM Support — Implementation Plan

Task: `pcm-support` · Stage 4 (senior-developer) · Charter: `.claude/handoff/pcm/CHARTER.md`
Upstream: `requirements.md`, `scenarios.md`, `design.md`, `adr-1.md`, `adr-2.md`, `adr-3.md` (all `Status: accepted`)
Downstream: developer implements each story; QA verifies.

## Goal
Wire the already-written-but-unlinked C++20/PCM module parser into the parallel Phase-1
dispatch so PCM-consuming TUs are indexed correctly, and so that missing/invalid `.pcm`
inputs produce a loud, counted failure (`failed_tu_count`) instead of a silent partial
parse (Issue 0001 family).

## Toolchain (rust-conventions) — exit-criteria env is baked into every command
libclang 18 required. On this macOS host the validated env (from AGENTS.md §3) is:
```
LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib
DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib
```
Formatter `cargo fmt --all`; linter `cargo clippy --all-targets --all-features -- -D warnings`;
tests `cargo nextest run --lib --tests --features test-mock`. No `.unwrap()`/`.expect()` in
new library paths — use `?` + `Error::Clang`.

## Settled facts (do not re-litigate)
- **Single dispatch site.** `src/pipeline/mod.rs:200` is the incremental cache-check loop
  (hashes only, no parse). The only live Phase-1 parse dispatch is
  `parallel::run_phase1_parallel` (`mod.rs:221`) → per-TU closure at `parallel.rs:163-179`.
  Routing at `parallel.rs:176` is therefore complete; there is **no** second live
  single-threaded parse path to also route. (Verified — see Risks.)
- **Failure-signaling OR is one path.** `failed_tu_count` (= `ParallelStats.tu_error`) drives
  BOTH `closing_summary()` (`mod.rs:230`) AND `FailOnTuError::exit_code()` (`index.rs:301`).
  All PCM load failures must land in `tu_error` via `Err(Error::Clang)`. (ADR-3 §C.)
- **Default `FailOnTuError` is `Ratio(1.0)`** (`index.rs:54`): a mixed fixture (1 ok + 1 broken)
  exits 0 by default. Tests assert on `failed_tu_count > 0`, NOT exit code. (ADR-3 §tests.)
- **No graph schema change.** `schemaBump:false`. `prompt/graph_database/cpp/schema.txt` must
  stay untouched (a story exit-criterion checks this).

---

## S1 — PCM detection + WARNING-level capability probe (ADR-2, ADR-1 §2)
Goal: `is_module_tu()` returns `true` on flag presence independent of language standard, and
the process-wide "modules UNAVAILABLE" log is `warn!` (S1-AC4 requires WARNING level).

Files to touch:
- `src/visit/modules_cpp20.rs:168-179` — rewrite `is_module_tu()` per ADR-2: `true` when ext is
  `cppm|ixx|mxx` OR any arg `== "-fmodules"` OR any arg `starts_with("-fmodule-file=")` OR any
  arg `starts_with("-fprebuilt-module-path")`. Drop the `-std=c++20` co-requirement. Detect once
  (no double-count when both flags present).
- `src/visit/modules_cpp20.rs:139-144` — raise the `info!("C++20 modules: UNAVAILABLE …")` line
  to `warn!` (S1-AC4 / Gherkin line 77). Leave the two earlier diagnostic `info!` lines
  (119-124, 127-132) as-is; only the final capability-absent summary must be WARNING.
- `src/visit/modules_cpp20.rs:544-561` — extend the existing unit tests
  `is_module_tu_by_extension` / `is_module_tu_by_args`: add cases for `-fprebuilt-module-path=/d`
  alone → true (S1-AC1), `-fmodule-file=Foo=/p.pcm` alone → true (S1-AC2), `-fmodules` alone →
  true (Gherkin 60), no-flags `.cpp` → false (S1-AC3), and both PCM flags present → still true.
  Update/replace the now-incorrect `args_partial` assertion (the old test asserted `-std=c++20`
  alone is false — keep a no-flag false case).

New files: none.
Tests: unit tests in `modules_cpp20.rs` (above). Probe-bool / idempotency tests already exist.

Exit-criteria:
```
LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo fmt --all -- --check
LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo clippy --all-targets --all-features -- -D warnings
LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo nextest run --lib --features test-mock is_module_tu
grep -nE 'warn!\(' src/visit/modules_cpp20.rs | grep -q UNAVAILABLE || grep -nA3 'C\+\+20 modules: UNAVAILABLE' src/visit/modules_cpp20.rs | grep -q 'warn!'
```
parallel-safe: false (edits `modules_cpp20.rs`, shared with S2/S3; S2/S3 depend on it).

---

## S2 — Route detected PCM/module TUs to parse_module_tu() (ADR-1)
Goal: branch at `parallel.rs:176` on `is_module_tu()`; route to `parse_module_tu()` (capable)
or a counted skip (not capable); preserve original `-std` for flag-only `.cpp` PCM consumers.
Depends on S1.

Files to touch:
- `src/pipeline/parallel.rs:163-179` — inside the existing `catch_unwind` + `with_thread_index`
  + `THREAD_WRITER` closure, after `filtered_args` is computed, branch on
  `is_module_tu(&entry.file, &filtered_args)`:
  - `false` → existing `visit_tu_with_index(index, &opts, writer)` (no regression, S2-AC2).
  - `true` → probe gate: if `probe_cpp20_support()` is `false`, call `warn_and_skip(&entry.file)`
    AND return `Err(Error::Clang(...))` so it counts as `tu_error` (S2-AC4, ADR-3 §B — the
    existing `match` arm at `parallel.rs:189` maps `Err(Error::Clang)` → `error2`); if `true`,
    call `parse_module_tu(index, &entry.file, &filtered_args, repo_name, *entry.hash.as_bytes(),
    writer, allocator.as_ref().clone())`. Add the `modules_cpp20` import.
- `src/visit/modules_cpp20.rs:213-220` — gate the `-std=c++20` force-append on a
  module-interface *extension* (`cppm|ixx|mxx`). For flag-only PCM-consuming `.cpp` TUs, pass the
  TU's args through unchanged (ADR-1 §3 — forcing c++20 on a C++17 consumer changes semantics and
  can trigger spurious "module out of date" Fatals, breaking S2-AC3). Keep the `-fmodules`
  append behaviour or scope it the same way per ADR-1 §3 (interface units only).

New files: none.
Tests: extend `tests/integration/parallel_phase1.rs` or add a unit assertion that a non-module
TU still routes to `visit_tu_inner` (S2-AC2). The full mixed-fixture run is S4 (real libclang).

Exit-criteria:
```
LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo fmt --all -- --check
LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo clippy --all-targets --all-features -- -D warnings
LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo nextest run --lib --tests --features test-mock parallel
grep -q 'is_module_tu' src/pipeline/parallel.rs
```
parallel-safe: false (edits `parallel.rs` + `modules_cpp20.rs`; depends on S1, blocks S3).

---

## S3 — Loud diagnostic on missing/invalid .pcm + counted failure (ADR-3 §A)
Goal: detect-before-write, two gates, both return `Err(Error::Clang)` before any
`writer.write_*`, so failures land in `tu_error`. Closes Issue 0001 family for PCM TUs.
Depends on S2.

Files to touch:
- `src/visit/modules_cpp20.rs:203-243` — add a **pre-parse stat gate** at the top of
  `parse_module_tu` (before `index.parser(...).parse()`): for each arg `starts_with("-fmodule-file=")`,
  strip the prefix and parse both Clang forms — `name=path` (path = substring after the *second*
  `=`) and bare `path` (whole remainder). `stat` the resolved `.pcm` path; if missing, emit an
  **ERROR**-level log naming BOTH the `.pcm` path and the TU source file, return
  `Err(Error::Clang(...))`, write nothing (S3-AC1). For each `-fprebuilt-module-path=<dir>` arg,
  stat the directory; a missing module *under* a present dir is left to the post-parse gate
  (ADR-3 §A — concrete `.pcm` name is module-derived, resolved lazily).
- `src/visit/modules_cpp20.rs:245-255` — split the `Error | Fatal` severity lump: scan
  diagnostics for `Severity::Fatal` → **post-parse Fatal gate**: emit ERROR log naming the TU,
  return `Err(Error::Clang(...))`, write nothing (corrupt/truncated/out-of-date `.pcm`, S3-AC2).
  `Severity::Error` (no Fatal) retains the existing partial-write path → `Ok(true)` (legitimate
  soft partial; do NOT reclassify, ADR-3 alt-c). Note: the developer MUST empirically confirm on
  libclang 18+ that a corrupt `.pcm` surfaces as `Fatal`; if it surfaces only as `Error`, widen
  the explicit-file stat coverage and record the finding (ADR-3 Consequences / open question).
- `src/visit/modules_cpp20.rs` — add a hermetic unit test: `parse_module_tu` (or a factored-out
  pure helper) with a synthetic `-fmodule-file=Foo=/nonexistent/Foo.pcm` returns `Err` and writes
  nothing, with no libclang dependency (ADR-3 §tests bullet 4). Prefer factoring the stat check
  into a small pure `fn` so the test needs no `Index`.

New files: none.
Tests: hermetic missing-`.pcm` unit test (above). Real corrupt/missing end-to-end is S4.

Exit-criteria:
```
LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo fmt --all -- --check
LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo clippy --all-targets --all-features -- -D warnings
LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo nextest run --lib --features test-mock modules_cpp20
grep -nE 'Severity::Fatal' src/visit/modules_cpp20.rs
```
parallel-safe: false (edits `modules_cpp20.rs`; depends on S2).

---

## S4 — Integration test: mixed compile_commands with PCM + standard TUs (ADR-3 §tests)
Goal: CI-grade integration test covering the full parse path on a mixed fixture; `#[ignore]` /
skip-with-reason when the probe is absent so libclang < 18 CI does not fail (S4-AC3).
Depends on S1, S2, S3. Parallel-safe with S5 (separate files).

Files to touch:
- `tests/integration/pcm_integration.rs` (new) — follow the libclang-fixture pattern of
  `tests/integration/symbol_id_integration.rs`, the full-pipeline pattern of
  `tests/integration/parallel_phase1.rs`, and the exit/`failed_tu_count` assertion pattern of
  `tests/integration/cli_fail_on_tu_error.rs`. Build a fixture `compile_commands.json` with one
  standard `.cpp` TU and one PCM-consuming TU plus a prebuilt `.pcm` produced by the test harness.
  - S4-AC1: both TUs in graph output AND `failed_tu_count == 0` (mark `#[ignore]`, skip-with-reason
    when `probe_cpp20_support()` is false).
  - S4-AC2: remove the `.pcm`, assert `failed_tu_count > 0` (the always-on summary branch — robust
    to default `Ratio(1.0)`; do NOT assert exit code unless run with `--fail-on-tu-error 0.0`).
  - S4-AC3: when probe absent, the test logs a reason and is skipped, not failed.
- `tests/integration/mod.rs` — register the new test module if the harness uses an aggregator
  (check existing entries; add `mod pcm_integration;` if needed).

New files: `tests/integration/pcm_integration.rs` (+ any fixture under `tests/fixtures/`).
Tests: this story IS the test. Hermetic missing-`.pcm` assertion lives in S3; this is the
real-libclang end-to-end.

Exit-criteria:
```
LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo fmt --all -- --check
LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo clippy --all-targets --all-features -- -D warnings
LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo nextest run --tests --features test-mock pcm_integration
LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo nextest run --tests --features test-mock --run-ignored all pcm_integration
```
parallel-safe: true with S5 (new test file + docs touch disjoint paths); but ordered AFTER
S1-S3 (needs the implemented behaviour). Not parallel with S1-S3.

---

## S5 — Document PCM support and limitations (requirements S5)
Goal: README/doc states libclang 18+ requirement, supported flags, best-effort skip posture,
and loud-fail-on-missing/invalid posture. Non-behavioral; AC verified by doc presence.
Parallel-safe with S4.

Files to touch:
- `README.md` (or `docs/pcm.md` linked from README) — add a "PCM / C++20 module support" section
  covering: requires libclang 18+; supported flags `-fmodules`, `-fmodule-file=`,
  `-fprebuilt-module-path`; best-effort posture (libclang < 18 → PCM TUs skipped with a warning);
  missing/invalid `.pcm` → error + counted failure (`failed_tus > 0`), not a silent partial parse.

New files: optionally `docs/pcm.md`.
Tests: none (doc story).

Exit-criteria (checkable string presence — one grep per required claim):
```
grep -qE 'libclang 18' README.md docs/pcm.md 2>/dev/null
grep -q -- '-fmodules' README.md docs/pcm.md 2>/dev/null
grep -q -- '-fmodule-file=' README.md docs/pcm.md 2>/dev/null
grep -q -- '-fprebuilt-module-path' README.md docs/pcm.md 2>/dev/null
grep -qiE 'skip|best-effort' README.md docs/pcm.md 2>/dev/null
grep -qiE 'silent|partial|failed_tus|non-zero' README.md docs/pcm.md 2>/dev/null
```
parallel-safe: true (only docs; disjoint from all code stories and from S4's test file).

---

## Dependency order
S1 → S2 → S3 → S4. S5 parallel with S4 (both after S1-S3). Recommended execution: sequential
shared-tree for S1-S3 (they share `modules_cpp20.rs` and are dependency-chained — per recorded
dev-team lesson, no per-story worktree isolation for chained stories); S4 + S5 may run in
parallel after S3 merges.

## Risks / Out of scope
- **Probe is a proxy** (tests interface-unit parsing, not `.pcm` consumption). Acceptable: the
  ADR-3 per-TU stat + Fatal gate is the real safety net (design §6).
- **Empirical Fatal-severity assumption** (S3): corrupt `.pcm` is assumed to surface as
  `Severity::Fatal`. Developer must verify on libclang 18+; if it surfaces as `Error`, widen the
  explicit-file stat coverage and record it (ADR-3 Consequences).
- **`-fprebuilt-module-path` is a directory**, not a file — pre-parse stat checks the dir exists;
  the lazily-resolved `.pcm` name is caught by the post-parse Fatal gate (ADR-3 §A edge).
- **Single dispatch site confirmed**: `mod.rs:200` is the cache-hash loop, not a parse path. The
  only live parse dispatch is `run_phase1_parallel` → `parallel.rs:176`. No second site to route.
- Out of scope: RSS/Parquet-size reduction; schema/backend redesign; system-header nodes; gRPC
  (M9+); new edge kinds (module imports reuse `EdgeKind::Includes`, ADR-8 scope-limit holds).
- **Schema untouched** — `prompt/graph_database/cpp/schema.txt` must not change (`schemaBump:false`).

## References
- `design.md` §3 (the trap), §5 (dispatch flow), §7 (failure contract), §9 (hazards)
- `adr-1.md` (dispatch+routing), `adr-2.md` (flag-presence detection), `adr-3.md` (loud fail)
- Source: `src/visit/modules_cpp20.rs:66,139-144,168-179,203-255,544-561`;
  `src/pipeline/parallel.rs:163-179,189`; `src/pipeline/mod.rs:200,221,230`; `src/bin/index.rs:54,301`
- Tests pattern: `tests/integration/{symbol_id_integration,parallel_phase1,cli_fail_on_tu_error}.rs`
- Issue 0001 (silent total TU parse failure); `[[pages/planning/cpp-indexer-compact-ingest-path]]`
- Cognee tags: `task:pcm-support role:senior-developer`
