# Developer Log — Story 1: SymbolAllocator

## Skills loaded
- `rust-conventions` (loaded before any code written)

## Skills considered but not loaded
- `implement-story` — task dispatch already provided full plan.md; not needed.
- `graph-symbol-ids` — this IS the current task slug; already in context.

## Commands run

| Command | Outcome |
|---------|---------|
| `cargo fmt --all -- --check` (pass 1) | FAIL — rustfmt diffs in symbol_map.rs |
| `cargo fmt --all` | Fixed formatting |
| `cargo fmt --all -- --check` (pass 2) | PASS |
| `cargo clippy --all-targets --all-features -- -D warnings` (pass 1) | FAIL — 4x `clippy::useless_conversion` on `.try_into().ok().and_then(NonZeroUsize::new)` |
| Fixed: replaced `cache_size.try_into().ok().and_then(NonZeroUsize::new)` with `NonZeroUsize::new(cache_size)` | — |
| `cargo clippy --all-targets --all-features -- -D warnings` (pass 2) | PASS |
| `DYLD_LIBRARY_PATH=... cargo test --lib resolve::symbol_map` | PASS (14/14) |

## Deviations from plan.md
- `src/error.rs` added as 4th file-to-touch (additive `Sqlite` variant). Recorded in implementation-notes.md.
- `lru = "0.12"` crate added (plan allowed this; justified in implementation-notes.md).
- Exit gate requires `DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib` on macOS — pre-existing environment requirement.

## Tool failures / retries
- Pass 1 formatter: rustfmt reformatted import order and closure layout. Fixed by running `cargo fmt --all`.
- Pass 1 clippy: 4 `useless_conversion` errors. Fixed by removing `.try_into().ok()` chain.
- Total passes: 2 (within 3-pass budget).

---

# Re-run Session (2026-05-29) — Story 1 from clean working tree

## Skills loaded
- rust-conventions

## Skills considered but not loaded
- implement-story: dispatch already provided full plan.md

## Commands run + outcomes

| Step | Command | Outcome |
|------|---------|---------|
| Orientation | Read plan.md, design.md, Cargo.toml, src/resolve/mod.rs, src/error.rs | confirmed symbol_map.rs absent |
| advisor() | shape guidance received | — |
| Edit Cargo.toml | add rusqlite + lru | success |
| Edit src/error.rs | add Sqlite variant | success |
| Write src/resolve/symbol_map.rs | 628-line implementation | created |
| Edit src/resolve/mod.rs | register pub mod symbol_map | success |
| cargo fmt -- --check (pass 1) | FAIL: formatting diffs | — |
| cargo fmt --all | applied | 2 files reformatted |
| cargo clippy (pass 1) | FAIL: 2 map_or lints + dead_code | — |
| Fix map_or → == / is_some_and | edit symbol_map.rs | success |
| Fix dead_code: pub(crate) → pub | edit mod.rs | success |
| cargo clippy (pass 2) | FAIL: unused Arc import (non-test) | — |
| Gate Arc under #[cfg(test)] | edit symbol_map.rs | success |
| cargo fmt -- --check (pass 3) | exit 0 | PASS |
| cargo clippy (pass 3) | exit 0 | PASS |
| cargo test --lib resolve::symbol_map | 15/15 ok | PASS |

## Deviations from plan.md
- src/error.rs not in files-to-touch (Sqlite variant required); tagged sr-dev
- lru crate added (ADR-3 allows; justified)
- Module pub not pub(crate) (dead_code lint; no caller yet in Story 1)
- Arc import gated under #[cfg(test)]

## Final status: all signals clear (3 passes used)
