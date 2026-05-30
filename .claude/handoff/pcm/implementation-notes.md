# Implementation notes — pcm-support / S4 (+ S1–S3)

## Scope note
S4 was dispatched but S1–S3 were absent from the tree. All four stories were implemented in this session on the shared tree per the recorded dev-team lesson.

## Files changed

### S1–S3 — `src/visit/modules_cpp20.rs`
- `is_module_tu()`: flag-presence only, no `-std=c++20` co-requirement (ADR-2); detects `-fmodule-file=`, `-fprebuilt-module-path`, `-fmodules` alone.
- `run_probe()`: UNAVAILABLE log raised from `info!` to `warn!` (S1-AC4).
- `check_explicit_pcm_files()`: new pure helper — stats each `-fmodule-file=` path before any libclang call (S3-AC1).
- `parse_module_tu()`: pre-parse stat gate (S3-AC1), post-parse Fatal-severity gate (S3-AC2), `-std=c++20` force-append gated on interface extension only (ADR-1 §3).
- Unit tests: `is_module_tu_by_args` extended; 3 hermetic PCM-stat tests added.

### S2 — `src/pipeline/parallel.rs`
- Imports `is_module_tu`, `parse_module_tu`, `probe_cpp20_support`, `warn_and_skip`.
- Dispatch branch: `is_module_tu()` → `parse_module_tu` (capable) or `Err(Error::Clang)` via `warn_and_skip` (not capable). Standard TUs unchanged.

### S4 — new test infrastructure
- `tests/integration/pcm_integration.rs` — `pcm_integration_missing_pcm_causes_tu_error_not_silent_partial` (unconditional); `pcm_integration_valid_pcm_both_tus_indexed_no_error` (`#[ignore]`, skips-with-reason on Apple clang 17).
- `tests/fixtures/pcm_mixed/standard.cpp` + `consumer.cpp` — fixture TUs.
- `Cargo.toml` — `[[test]] name = "pcm_integration"` entry added.

## Tests added/run

Exit gates (1 pass):
```
cargo fmt --all -- --check            → OK
cargo clippy --all-targets --all-features -- -D warnings  → OK
cargo nextest run --tests --features test-mock pcm_integration  → 1/1 PASS
cargo nextest run --tests --features test-mock --run-ignored all pcm_integration  → 2/2 PASS
```

## Deviations from plan
1. S1–S3 implemented in same dispatch as S4 (were absent from tree).
2. S4-AC1 self-skips on this host: `probe_cpp20_support()` true but Apple clang 17 cannot `--precompile` named modules. Test logs reason and returns (S4-AC3 compliant).
3. Test function names prefixed `pcm_integration_` to match nextest positional-filter syntax.
4. S3 Fatal-severity assumption unverified (Apple clang 17 can't build PCMs for empirical check).

## Follow-ups (tag: sr-dev)
- Verify `Severity::Fatal` on corrupt `.pcm` on clang 18+ host (ADR-3 open question).
- Run S4-AC1 on clang 18+ host where `--precompile` works.
- Per-story S1–S3 logs absent; QA should reference consolidated log.

---

# Implementation notes — pcm-support / S3 (re-dispatch verification)

This re-dispatch confirmed S3 gates were already implemented. Added empirical severity probe test.

## Key finding: Fatal-severity assumption falsified on libclang 18/macOS
Running `corrupt_pcm_fatal_severity_probe` (#[ignore]) with `--nocapture`:
- Corrupt PCM surfaces as `Severity::Error`, NOT `Severity::Fatal`
- The post-parse Fatal gate (S3-AC2) is therefore insufficient for corrupt-but-present PCM on macOS libclang 18
- The pre-parse stat gate (S3-AC1, missing files) remains valid and sufficient
- Per ADR-3 alt-c: `Severity::Error` retains partial-write path — not reclassified

Files changed: `src/visit/modules_cpp20.rs` — added `corrupt_pcm_fatal_severity_probe` (#[ignore]) test

Exit gates (1 pass): fmt OK, clippy OK, nextest modules_cpp20 10/10 PASS, grep Severity::Fatal FOUND (line 293)

Follow-ups (sr-dev):
- ADR-3 open question confirmed: corrupt .pcm → Error not Fatal on libclang 18/macOS. Options: (a) widen stat coverage to check PCM magic bytes, (b) reclassify PCM-consumer Error diagnostics as failures, (c) accept partial-write and downgrade S3-AC2 to best-effort.
- `-fprebuilt-module-path` directory stat: not in `check_explicit_pcm_files` — plan says lazy resolution via Fatal gate, but Fatal gate is insufficient (above). Follow-up for sr-dev.

---

# Implementation notes — pcm-support / S2 (re-dispatch verification)

S2 routing was already fully implemented in the prior combined S1–S4 session. This dispatch verified all exit-gate criteria pass with zero retries:
- `cargo fmt --all -- --check` → exit 0
- `cargo clippy --all-targets --all-features -- -D warnings` → exit 0
- `cargo nextest run --lib --tests --features test-mock parallel` → 6/6 PASS
- `grep -q 'is_module_tu' src/pipeline/parallel.rs` → FOUND

Files verified: `src/pipeline/parallel.rs:167–195` (routing branch), `src/visit/modules_cpp20.rs:167–179` (`is_module_tu`), `src/visit/modules_cpp20.rs:213–220` (interface-ext guard on `-std=c++20`).

Deviations: None. No code changes required.

---

# Implementation notes — pcm-support / S1 (re-run, exit-gate fix)

This re-run confirmed S1 behavior was already correct. The only change made was collapsing the multi-line `warn!(...)` for the UNAVAILABLE message into a single line so the ERE exit gate (`grep -nE 'warn!\(' | grep -q UNAVAILABLE`) passes.

Exit gates (1 pass): fmt OK, clippy OK, nextest is_module_tu 2/2 PASS, grep ERE PASS.

Follow-up (sr-dev): The BRE second-alternative in plan.md S1 exit criteria (`grep -nA3 'C\+\+20 modules: UNAVAILABLE' | grep -q 'warn!'`) is platform-fragile on macOS BSD grep — `\+` is treated as a one-or-more quantifier so the pattern never matches `C++20`. Standardize future exit-criteria greps to `grep -E`.

---

# Implementation notes — pcm-support / S5

Files changed:
- `README.md` — added "PCM / C++20 module support" section (libclang 18 req, supported flags, best-effort skip, loud-fail-on-missing posture, link to docs/pcm.md)
- `docs/pcm.md` — new file; full reference: requirements, flag table, best-effort skip posture, loud-failure-on-missing/invalid .pcm, macOS env setup, see-also

Tests added/run:
- No code tests (doc story). Exit gate: all 6 grep assertions pass in 1 pass.

Deviations from plan:
- None.

Follow-ups:
- None. S5 is parallel-safe with S4; S4 (integration test) is the next open story.

References:
- plan.md §S5
- docs/pcm.md (new)
- README.md §PCM
