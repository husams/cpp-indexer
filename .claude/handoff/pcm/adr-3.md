# ADR-3: Loud-diagnostic strategy for missing/invalid .pcm + failure-signaling contract

Status: accepted

Context:
This is the safety-critical decision; it directly closes the Issue-0001 failure family for PCM
TUs (S3). Two facts force the design:

1. **`parse_module_tu` is currently itself an Issue-0001 trap.** libclang's `parse()` rarely
   returns `Err` for a module-load failure — it returns `Ok(tu)` carrying a **Fatal**
   diagnostic. The current code (`modules_cpp20.rs:245-247`) lumps `Error | Fatal` together as
   `has_errors`, logs a `warn!`, then **writes the partial nodes** (lines 481-482) and returns
   `Ok(true)`. In the pipeline that counts as `tu_partial`, NOT `tu_error`, and the process
   exits 0 — exactly what S3-AC1/AC2 forbid.

2. **The failure-signaling "OR" (scenarios open-question #3) is not a real disjunction here.**
   `failed_tu_count` already drives BOTH the machine-readable summary (`mod.rs:230` →
   `closing_summary()`, `mod.rs:724`) AND the exit code (`index.rs:301` →
   `FailOnTuError::exit_code()`, `index.rs:64`). Feeding `failed_tu_count` satisfies both
   branches at once. `warn_and_skip` (`modules_cpp20.rs:157`), however, increments nothing.

Decision:

A. **Detect-before-write, two gates, both return `Err(Error::Clang)` before any `writer.write_*`:**
   - **Pre-parse stat (deterministic, attributable).** For each `-fmodule-file=` arg, extract
     the path and `stat` it. **Both Clang forms must be handled:** `-fmodule-file=name=path`
     (named) AND `-fmodule-file=path` (no name) — i.e. split on the first `=` after the flag;
     if the remainder contains another `=`, the substring after it is the path, otherwise the
     whole remainder is the path. Missing → emit an ERROR-level log naming **both** the `.pcm`
     path and the TU source file, return `Err(Error::Clang(...))`, write nothing. This
     satisfies S3-AC1 deterministically and is attributable to the PCM-load step without
     depending on libclang surfacing a distinct error code. For `-fprebuilt-module-path=<dir>`,
     stat the directory; the concrete `.pcm` filename is module-derived and resolved lazily by
     libclang, so a missing module under a present dir is caught by the post-parse gate.
   - **Post-parse Fatal gate.** Split the severity lump: scan diagnostics and treat
     **`Severity::Fatal`** as a load failure (corrupt/truncated/out-of-date `.pcm`) → ERROR log
     naming the TU, return `Err(Error::Clang(...))`, write nothing. **`Severity::Error`** retains
     the existing partial-write behavior → `Ok(true)` (counted as `tu_partial`), since
     ordinary semantic errors are a legitimate partial parse, not a module-load failure.

B. **Probe-absent skip is counted, not silent (S2-AC4).** When `probe_cpp20_support()` returns
   `false` for a TU carrying PCM flags, the dispatch (ADR-1) emits the `warn_and_skip` log AND
   routes the TU to the `tu_error` count. Mechanism: return `Err(Error::Clang(...))` from the
   module-dispatch branch for the skip case (cheapest — reuses the existing `match` arm at
   `parallel.rs:189`). `warn_and_skip` keeps its log; the count comes from the `Err` return.

C. **Failure-signaling contract (resolves open-question #3):** all three PCM failure modes
   (probe-absent skip, missing `.pcm`, Fatal `.pcm`) increment `failed_tu_count`. The
   `failed N` summary is always emitted; the non-zero exit fires per the operator's
   `--fail-on-tu-error` policy. **`FailOnTuError::default()` is `Ratio(1.0)`** (`index.rs:54`):
   exit 2 only when *all* TUs fail. Tests with a mixed fixture therefore assert on
   `failed_tu_count > 0` or pass `--fail-on-tu-error 0.0` (see §tests).

Tests (S4):
- Follow the libclang-fixture pattern of `tests/integration/symbol_id_integration.rs` and the
  full-pipeline pattern of `tests/integration/parallel_phase1.rs`; the exit-code assertion
  pattern is in `tests/integration/cli_fail_on_tu_error.rs` (resolves scenarios
  open-question #2). Mark the real-parse test `#[ignore]` and skip-with-reason when
  `probe_cpp20_support()` is false, so CI on libclang < 18 does not fail (S4-AC3).
- Mixed fixture (1 standard + 1 PCM consumer with a prebuilt `.pcm`): assert both TUs in graph
  output and `failed_tu_count == 0` (S4-AC1).
- Remove the `.pcm`: assert `failed_tu_count > 0` (the always-on summary branch — robust to the
  default `Ratio(1.0)`), OR run with `--fail-on-tu-error 0.0` and assert exit code 2 (S4-AC2).
- Unit-test the pre-parse stat path with a synthetic `-fmodule-file=` pointing at a missing
  path — hermetic, no libclang needed.

Alternatives considered:
- a) Scrape libclang diagnostic *text* to distinguish module-load vs parse failure — rejected as
  primary: brittle across 18.x patch releases; the pre-parse `stat` gives attribution for free
  and the Fatal-severity split is stable.
- b) Add a dedicated `skipped_tu_count` / `module_load_failures` field to `ParallelStats` and a
  new exit branch — viable but rejected for v1: reuses no existing contract, adds a second
  signaling path to keep consistent. `tu_error` already feeds summary + exit; folding PCM
  failures into it is the smallest correct change. (A struct field is not a graph schema bump,
  so this remains available later without breaking `schemaBump:false`.)
- c) Keep the current `Error | Fatal` lump and just flip the write to be skipped on any error —
  rejected: would reclassify legitimate soft-`Error` partials as hard failures, regressing the
  partial-TU semantics other parts of the pipeline rely on.

Consequences:
- Positive: closes the Issue-0001 family for PCM TUs; one signaling path (`failed_tu_count`)
  satisfies both OR-branches; attribution (names `.pcm` + TU) without fragile text scraping.
- Negative: relies on the empirical claim that missing/corrupt `.pcm` surfaces as `Fatal` (or is
  caught by the pre-parse stat). Developer MUST verify this empirically on libclang 18+ during
  implementation; if a real load failure surfaces only as `Error` severity, the pre-parse stat
  still catches the explicit-file case, and the developer must widen the gate accordingly and
  note it. This is the one residual implementation risk and is called out in the open question.
- Follow-up: no graph schema change. `schemaBump: false` holds (graph schema v5/v6 unaffected;
  any future `ParallelStats` field is an in-process counter).

References:
src/visit/modules_cpp20.rs:157,203,215-217,245-247,481-482; src/pipeline/parallel.rs:181-211;
src/pipeline/mod.rs:230,724; src/bin/index.rs:54,64,301; Issue 0001 (silent total TU parse
failure); tests/integration/symbol_id_integration.rs, tests/integration/parallel_phase1.rs,
tests/integration/cli_fail_on_tu_error.rs (fixture + exit-code patterns); scenarios.md
open-questions #1,#2,#3;
.claude/handoff/pcm/design.md §3,§7; cognee task:pcm-support role:architect
