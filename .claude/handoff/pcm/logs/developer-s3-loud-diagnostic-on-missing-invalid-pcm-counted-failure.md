# Developer Log — S3: Loud diagnostic on missing/invalid .pcm + counted failure

## Skills loaded
- rust-conventions (loaded before any file edits)

## Skills considered but not loaded
- cpp-conventions — not applicable (no CMakeLists.txt changed)
- implement-story — task was dispatched with explicit plan; used plan.md directly

## Pre-flight
- Implementation was already present in `src/visit/modules_cpp20.rs` from prior development
- Both S3 gates (pre-parse stat + post-parse Fatal) were in place

## Commands run

### Exit gate pass 1
- `cargo fmt --all -- --check` — FAILED (formatting diff in probe test)
- `cargo fmt --all` — formatted
- `cargo fmt --all -- --check` — PASS
- `cargo clippy --all-targets --all-features -- -D warnings` — PASS
- `cargo nextest run --lib --features test-mock modules_cpp20` — PASS (10/10)
- `grep -nE 'Severity::Fatal' src/visit/modules_cpp20.rs` — PASS (line 293, 733)

## Deviations from plan
1. **Fatal-severity assumption empirically falsified (ADR-3 open question):** libclang 18 on macOS arm64 emits `Severity::Error` (not `Fatal`) for a corrupt `.pcm` referenced via `-fmodule-file=`. The `import Foo;` line fails with "unknown type name 'import'" as an Error diagnostic. The Fatal gate (lines 291-305) therefore does NOT catch corrupt-PCM (only missing-PCM is caught by the pre-parse stat). Per ADR-3 alt-c, `Severity::Error` retains the partial-write path — this is a documented deliberate design choice, not a code bug.
   - **Empirical test added:** `corrupt_pcm_fatal_severity_probe` (`#[ignore]`) records the finding; running it with `--nocapture` shows `has_fatal=false has_error=true`.
   - **Consequence:** S3-AC2 ("corrupt .pcm → no graph output") is NOT fully satisfied by the current Fatal gate on macOS libclang 18. The explicit-file stat gate (pre-parse) covers missing files. Corrupt-but-present files fall through to partial-write.
   - **Open item tagged sr-dev:** widen explicit-file stat coverage (e.g., try parsing the PCM header magic bytes) OR accept the partial-write posture for corrupt PCM and downgrade S3-AC2 to best-effort.

2. **`-fprebuilt-module-path` directory stat:** `check_explicit_pcm_files` only handles `-fmodule-file=` (explicit file path). It does not stat `-fprebuilt-module-path=<dir>`. Per plan lines 119-121, this is the intended design (directory stat; lazy resolution of concrete `.pcm` name). The current code matches the plan — directory stat is the plan's intent but the function name only claims "explicit PCM files". No deviation.

## Findings to surface
- Fatal-severity assumption is false on libclang 18/macOS — record as open item for sr-dev.
- The pre-parse stat gate is the reliable safety net; the post-parse Fatal gate is defense-in-depth for other libclang builds that may surface Fatal.
