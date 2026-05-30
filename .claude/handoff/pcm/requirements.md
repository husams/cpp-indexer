# PCM Support — Requirements

Source spec: [[pages/planning/cpp-indexer-compact-ingest-path]] § "Added requirement (2026-05-30): PCM / precompiled-module support"
Charter: .claude/handoff/pcm/CHARTER.md

---

## Scope

Wire the existing (unlinked) C++20 module parser into the main pipeline dispatch so that TUs consuming Clang precompiled modules (`.pcm` / `-fmodules` / `-fmodule-file=` / `-fprebuilt-module-path`) are correctly indexed.

No schema changes required — `MODULE` node and `module_interface` attr already exist.

## Out of scope

- Reducing ingest-peak RSS or staged-Parquet size (PCM is CPU-bound, not RAM-bound; does not advance compact-ingest-path memory goals).
- Schema or backend redesign.
- Adding system-header nodes.
- gRPC protocol work (M9+).

---

## Stories

### S1 — Detect PCM TUs at the pipeline dispatch point

Story: As the indexer pipeline, I want to identify TUs that carry PCM flags (`-fprebuilt-module-path`, `-fmodule-file=`, `-fmodules`) so that I can route them to the correct parser.

Acceptance criteria:
  - Given a compile command containing `-fprebuilt-module-path=<dir>`, when `is_module_tu()` is called, then it returns `true`.
  - Given a compile command containing `-fmodule-file=<name>=<path>`, when `is_module_tu()` is called, then it returns `true`.
  - Given a compile command with neither C++20 module nor PCM flags, when `is_module_tu()` is called, then it returns `false`.
  - Given libclang < 18 at runtime, when `probe_cpp20_support()` is called, then it returns a capability-absent result and logs a warning; no panic or silent exit-0.

Priority: P0 — prerequisite for S2 and S3; gate on dispatch correctness.
Dependencies: none
Open questions: none
References: src/visit/modules_cpp20.rs:168, src/visit/modules_cpp20.rs:66; [[pages/planning/cpp-indexer-compact-ingest-path]]

---

### S2 — Route detected PCM/module TUs to parse_module_tu()

Story: As the indexer pipeline, I want module and PCM TUs dispatched to `parse_module_tu()` at `src/pipeline/parallel.rs:176` so that their AST is parsed with the correct libclang module-aware codepath.

Acceptance criteria:
  - Given a TU where `is_module_tu()` returns `true`, when the parallel pipeline processes it, then `parse_module_tu()` is called instead of `visit_tu_inner()`.
  - Given a standard (non-module) TU, when the parallel pipeline processes it, then `visit_tu_inner()` is called (no regression).
  - Given a mixed `compile_commands.json` containing both standard and module TUs, when the indexer runs to completion, then both kinds produce graph output and the process exits with code 0.
  - Given a TU with PCM flags but where libclang reports PCM support is absent, when the pipeline processes it, then the TU is skipped with a LOUD warning log (not a silent partial parse) and the overall exit code is non-zero OR a summary diagnostic reports the skip count.

Priority: P0 — core deliverable.
Dependencies: S1
Open questions: none
References: src/pipeline/parallel.rs:176; src/visit/modules_cpp20.rs:203

---

### S3 — Loud diagnostic on missing or invalid .pcm file

Story: As an operator indexing a codebase with PCM files, I want a loud, unambiguous error when a `.pcm` file is missing or invalid, so that I never receive a silent partial parse (Issue 0001 family).

Acceptance criteria:
  - Given a compile command referencing a `.pcm` file that does not exist on disk, when the indexer processes that TU, then it emits an ERROR-level log line naming the missing file path and the TU source file, and does NOT silently emit a partial graph for that TU.
  - Given a corrupt/truncated `.pcm` file, when the indexer processes the referencing TU, then it emits an ERROR-level log line and does NOT produce graph output for that TU.
  - Given either of the above failure cases, when the indexer run finishes, then the process exits with a non-zero exit code OR emits a machine-readable summary with `failed_tus > 0`.
  - Given all PCM files present and valid, when the indexer runs, then no spurious error diagnostics are emitted.

Priority: P0 — safety requirement; directly closes the Issue 0001 failure family for PCM TUs.
Dependencies: S2
Open questions:
  - Does libclang surface a distinct error code for module-load failure vs. parse failure? Confirm during implementation; the diagnostic must be attributable to the PCM load step specifically if the API allows it.
References: Issue 0001 (silent total TU parse failure); src/visit/modules_cpp20.rs:203; [[pages/planning/cpp-indexer-compact-ingest-path]] § Constraints/risks

---

### S4 — Integration test: mixed compile_commands with PCM and standard TUs

Story: As a developer, I want an automated test that exercises the full parse path with a mixed `compile_commands.json` containing both standard TUs and PCM-consuming TUs, so that regressions in dispatch logic are caught by CI.

Acceptance criteria:
  - Given a fixture with at least one standard TU and one PCM-consuming TU (using a prebuilt `.pcm` from the test harness), when the indexer runs against the fixture, then both TUs appear in the graph output and the exit code is 0.
  - Given the same fixture but with the `.pcm` file removed, when the indexer runs, then the test asserts a non-zero exit code or a non-zero `failed_tus` count.
  - The test runs in CI without requiring a libclang 18+ system library when libclang < 18 is detected (skip with reason logged, not fail).

Priority: P1 — required for safe merge; can follow S1–S3 if developed in parallel.
Dependencies: S1, S2, S3
Open questions:
  - What is the standard fixture pattern for libclang-dependent tests in this repo? Confirm with existing `symbol_id_integration.rs` pattern.
References: src/visit/modules_cpp20.rs; benches/sink_throughput.rs (pattern reference)

---

### S5 — Document PCM support and limitations

Story: As a user of the cpp-indexer, I want documentation on PCM support so that I know what flags are supported, what libclang version is required, and what happens when PCM files are missing.

Acceptance criteria:
  - The README or a linked doc page states that PCM support requires libclang 18+.
  - The doc lists the supported compiler flags: `-fmodules`, `-fmodule-file=`, `-fprebuilt-module-path`.
  - The doc describes the best-effort posture: when libclang < 18 is present, PCM TUs are skipped with a warning.
  - The doc states that missing/invalid `.pcm` files produce an error and non-zero exit, not a silent partial parse.

Priority: P2 — required before the feature is considered GA; can be written last.
Dependencies: S1–S4
Open questions: none
References: CHARTER.md; [[pages/planning/cpp-indexer-compact-ingest-path]]

---

## Dependency order

S1 → S2 → S3 → S4 (S5 can be written in parallel with S4)

---

## Risks carried forward

- libclang PCM API surface varies between 18.x patch releases — implementation must probe at runtime, not at compile time.
- Issue 0001 failure mode (silent partial parse on clean compile_commands) must be regression-tested in S4; S3 AC is the minimum bar.
