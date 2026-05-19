# ADR-3: `--fail-on-tu-error` Clap shape, ratio semantics, and exit-code mapping

Status: accepted
Date: 2026-05-19
Stage: architect (tu-parse-fail v3)
Covers AC: AC-5 (S3); addresses scenarios Open questions 1 and 3

## Context
S3 / AC-5 introduces a new `cxg-index` flag:
- Accepts `f64` in `[0.0, 1.0]` (ratio) OR the literal sentinel `never`.
- Default: `1.0` (back-compat: only all-fail → exit 2; partial fail → exit 0).
- Comparison: `failed / total >= ratio` ⇒ exit `2`; else exit `0`.
- Invalid values (`1.5`, `-0.1`, `garbage`) → clap parse error (non-zero exit).
- Edge case (scenarios Open question 1): `total == 0` ratio undefined.

`cxg-index` today returns `anyhow::Result<()>`; `main` returns `Ok(())` on
success, and clap parse errors / pipeline errors return non-zero via `anyhow`
auto-format (exit `1`). We need to introduce an additional exit code (`2`)
that is **only** triggered by the failed-TU threshold, distinct from clap (`2`
by default in clap when used with `process::exit`) and pipeline error (`1`).

Forces:
- Clap derive macros do not directly support "f64 OR sentinel string" on one
  flag → need a wrapper enum with `FromStr`.
- `--fail-on-tu-error never` and `--fail-on-tu-error 0.5` must both be accepted
  on the same flag (operators don't want two flags).
- The exit code must be reachable without `process::exit`, because
  `tracing-subscriber` etc. on the existing `#[tokio::main]` body relies on
  Drop. Use `std::process::ExitCode` from `main`.
- `total == 0` case: per scenarios Open question 1, must be decided here.
  Operator intent: a run with no TUs is *not* a failure — `compile_commands.json`
  with zero entries is operator error caught elsewhere (the existing
  `filter_entries_to_input_scope` already errors when no entries match the
  input scope, see src/pipeline/mod.rs:753). So `0/0 → exit 0`.

## Decision

### 1. CLI type
New module-private enum in `src/bin/index.rs`:

```rust
#[derive(Debug, Clone, Copy)]
enum FailOnTuError {
    Never,
    Ratio(f64), // invariant: 0.0 <= r <= 1.0
}

impl std::str::FromStr for FailOnTuError {
    type Err = String;
    fn from_str(s: &str) -> Result<Self, Self::Err> {
        if s.eq_ignore_ascii_case("never") {
            return Ok(Self::Never);
        }
        let r: f64 = s.parse().map_err(|_|
            format!("expected a number in [0.0, 1.0] or `never`, got `{s}`"))?;
        if !(0.0..=1.0).contains(&r) || r.is_nan() {
            return Err(format!("ratio must be in [0.0, 1.0], got `{r}`"));
        }
        Ok(Self::Ratio(r))
    }
}

impl Default for FailOnTuError {
    fn default() -> Self { Self::Ratio(1.0) }
}
```

Clap field:
```rust
#[arg(long = "fail-on-tu-error",
      value_name = "RATIO|never",
      default_value = "1.0")]
fail_on_tu_error: FailOnTuError,
```

`default_value = "1.0"` round-trips through `FromStr` (clap accepts that on
custom types with `ValueParser::new(...)`-style derivation). Verified by the
clap derive book §value_parser auto-derivation for `FromStr` types.

### 2. Threshold check + exit mapping

Add a method on `FailOnTuError`:

```rust
impl FailOnTuError {
    fn exit_code(&self, failed: usize, total: usize) -> u8 {
        match self {
            FailOnTuError::Never => 0,
            FailOnTuError::Ratio(r) => {
                if total == 0 { return 0; }              // 0/0 → success
                let ratio = failed as f64 / total as f64;
                if ratio >= *r { 2 } else { 0 }
            }
        }
    }
}
```

- `total == 0` short-circuits to 0 (Open question 1 resolution).
- `failed = 0, total > 0, ratio = 0.0` → `0.0 >= 0.0` is true → exit 2.
  This matches scenarios "Ratio 0.0 — any failure" but **also** triggers when
  zero failures occur with ratio 0.0. That is acceptable: ratio 0.0 means
  "fail on any non-trivial run regardless of failures"; operators who don't
  want that use the default `1.0` or `never`. Documented in CLI help text.

  Wait — re-check: with `failed=0, total=5, ratio=0.0`, `0.0 >= 0.0` is true,
  exit 2. That's user-hostile. Adjust:

  **Correction**: the comparison is `failed > 0 && ratio_satisfies`. Re-state:

  ```rust
  if failed == 0 { return 0; }                          // no failures → success
  let ratio = failed as f64 / total as f64;
  if ratio >= *r { 2 } else { 0 }
  ```

  Now: ratio 0.0 + any-fail → exit 2 (matches scenario); ratio 0.0 + no-fail
  → exit 0 (correct). Boundary case "ratio exactly met" (failed=2 of 4,
  ratio=0.5 → 0.5 >= 0.5 → exit 2) remains correct. `total == 0` already
  excluded via `failed == 0` short-circuit.

### 3. `main` return type
Change signature from `anyhow::Result<()>` to
`anyhow::Result<std::process::ExitCode>`:

```rust
async fn main() -> anyhow::Result<std::process::ExitCode> {
    // ... existing setup ...
    let stats = run(Arc::clone(&sink), opts).await.context("pipeline failed")?;
    eprintln!("{}", stats.closing_summary());
    let code = cli.fail_on_tu_error.exit_code(stats.failed_tu_count, stats.tu_count);
    Ok(std::process::ExitCode::from(code))
}
```

Pipeline error (clap parse error or `anyhow` bubble) continues to exit `1` —
that path is untouched. New threshold-driven exit is **only** code `2`.

### 4. Help text
```
--fail-on-tu-error <RATIO|never>
    Exit with code 2 when the fraction of failed translation units meets or
    exceeds RATIO. `never` disables the check. Default: 1.0 (exit 2 only when
    every TU fails).
```

## Alternatives considered

| Option | Trade-off | Verdict |
| ------ | --------- | ------- |
| **A. Two separate flags** (`--fail-on-tu-error <ratio>` and `--no-fail-on-tu-error`) | More flags to document; can't be combined sensibly; operators expect one flag with one value. | rejected — UX |
| **B. Custom `FromStr` enum** (chosen) | One flag, both forms; clap derives `value_parser` from `FromStr`; small surface. | **accepted** |
| **C. Use `clap::ValueEnum`** | Only handles closed token sets — can't accept arbitrary `f64`. | rejected — wrong tool |
| **D. Three flags** (`--fail-ratio`, `--never-fail`, default) | Worse UX, more state to validate. | rejected |
| **E. `0/0 → exit 2`** | Operator-hostile: a run with no TUs (e.g. empty compile_commands.json after filtering) would always exit 2 regardless of ratio. | rejected (resolves Open question 1 → exit 0) |
| **F. `failed > 0 && ratio_satisfies`** (chosen for threshold) | Avoids the "0 failed + ratio 0.0 → exit 2" footgun while preserving "any-fail with ratio 0.0 → exit 2". | **accepted** |

## Consequences

Positive:
- Single new flag; clap parse errors handled by clap's existing machinery
  (exit code 2 from clap on parse error, exit 2 from threshold check — both
  mean "user-visible failure" so the overlap is acceptable; clap also writes a
  diagnostic to stderr).
- `main` signature change is isolated; no other consumer of the binary.

Negative / follow-ups:
- Clap's default parse-error exit is `2` — same as our threshold exit. CI
  scripts cannot distinguish "bad flag" from "too many failed TUs" by exit
  code alone. Acceptable: stderr message makes the cause obvious, and both
  cases are "non-success".
- `failed_tu_count > tu_count` would yield ratio > 1.0 — impossible by
  construction (failed is a subset of total in `ParallelStats`), but if
  invariants drift the comparison still works (clamped semantics: any ratio
  >= threshold triggers).

## References
- requirements.md §S3, AC-5
- scenarios.md Feature S3 (10 scenarios), Open questions 1 and 3
- /Users/husam/workspace/cpp-indexer/src/bin/index.rs:86 (current main signature)
- ADR-2 (provides `stats.failed_tu_count` and `closing_summary()`)
- Cognee tag: task:tu-parse-fail, role:architect
