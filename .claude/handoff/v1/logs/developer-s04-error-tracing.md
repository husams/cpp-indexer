## Developer log — S04-error-tracing
date: 2026-05-17
worktree: /Users/husam/workspace/cpp-indexer/.worktrees/s04-error-tracing
branch: story/s04-error-tracing

### Skills loaded
- rust-conventions (loaded; project has Cargo.toml + *.rs)

### Skills considered but not loaded
- cpp-conventions — not applicable (Rust project)
- python-conventions — not applicable
- typescript-conventions — not applicable
- implement-story — task dispatched directly with plan.md; skill not needed
- simplify — not loaded; this is new code, not a refactor

### Orientation reads
- CHARTER.md — invariants, path conventions
- plan.md lines 112-125 — S04 story, exit criteria, files to touch
- design.md §5.2 §5.3 §5.4 — error variants, secret redaction spec, observability init
- Cargo.toml — existing dependencies (thiserror, tracing, tracing-subscriber with env-filter only)
- src/lib.rs — confirmed inline empty module stubs (no separate files yet)

### Advisor call (pre-implementation)
Called advisor before writing. Key flags received:
1. Exit-gate filter `error::` will skip the observability redaction test — noted as deviation.
2. Sink source field needs `Send + Sync + 'static` for thread safety — applied.
3. Redaction must override all Visit methods (str/debug/i64/u64/f64/bool/error) — applied.
4. Use `with_default` not `set_global_default` in tests — applied.
5. Assert both absence of raw secret AND presence of `***` — applied.
6. `lib.rs` inline stubs must change to `pub mod error;` file references — applied.

### Commands run

| Command | Exit | Notes |
|---|---|---|
| `cargo fmt --all` | 0 | Applied import reorder on error.rs and observability.rs |
| `cargo fmt --all -- --check` | 0 | Clean after apply |
| `cargo clippy --all-targets --all-features -- -D warnings` | 0 | First run had 5 errors (unused imports, FormattedFields not Write); fixed by rewriting observability.rs to use `Writer<'writer>` not `&mut dyn Write` |
| `cargo nextest run -p cpp_indexer error::` | 0 | 1 passed, 1 skipped (observability test filtered out) |
| `cargo test` | 0 | 2 passed (both tests run) |

### Issues encountered and resolved
- **Clippy pass 1**: `FormattedFields<RedactingFields>` does not implement `std::fmt::Write`. Root cause: `add_fields` tried to pass `current` as `&mut dyn Write` but `FormattedFields` only exposes `as_writer()` returning `Writer<'_>`. Fix: rewrote `RedactingVisitor` to hold `Writer<'writer>` (tracing-subscriber's wrapper type) and implemented `MakeVisitor<Writer<'writer>>`.
- **Clippy pass 1**: Several unused imports (`FmtContext`, `FormatEvent`, `LookupSpan`, `Layer`, `Subscriber`) from initial draft. Removed.
- **Formatter**: Import order differed between error.rs and observability.rs across two runs — `cargo fmt --all` applied both times.

### Deviations from plan.md
1. Exit gate filter mismatch (see implementation-notes.md follow-ups).
2. Sink variant source widened to `Box<dyn StdError + Send + Sync + 'static>`.
3. `tracing-subscriber` `fmt` feature added to Cargo.toml.

### Unresolved signals
None. All three exit gates pass at exit.
