# Developer log — S5 spdlog smoke test (s5-spdlog-smoke)

run_id: tu-parse-fail-v3
story-slug: s5-spdlog-smoke
role: developer

## Skills loaded

- `rust-conventions` — loaded before writing any code

## Skills considered but not loaded

- `implement-story` — not loaded; task scope is a single new test file with no src/ changes; standard developer role flow was sufficient
- `cpp-conventions` — not applicable; project is Rust
- `simplify` — not applicable; no refactor scope

## Orientation steps

1. Read CHARTER.md → confirmed paths and toolchain
2. Read plan.md §S5 → established file placement, ignore attribute, skip-cleanly requirements, AC-3 assertion
3. Read adr-1.md → context on sanitiser (what the smoke test validates)
4. Inspected `Cargo.toml` → confirmed `tempfile` and `assert_cmd` already in dev-deps; `[[test]]` must be added manually (no autobins for tests)
5. Inspected `tests/integration/` listing → integration tests use individual `[[test]]` entries, not a mod.rs aggregation
6. Read `tests/integration/mod.rs` → placeholder comment only; no submodule declarations
7. Read `src/pipeline/mod.rs` lines 50–90 → confirmed `RunOptions` fields; lines 640–720 → confirmed `PipelineStats` fields including `failed_tu_count`
8. Read `tests/integration/m1_exit_gate.rs` lines 1–135 → confirmed `#[tokio::test]`, `Arc<dyn GraphSink>`, `MockSink::default()`, `RunOptions` shape
9. Read `src/sink/mock.rs` top lines → confirmed `MockSink` gated on `any(test, feature = "test-mock")`

## Called advisor

Before writing: advisor confirmed `[[test]]` entry is mandatory, `required-features = ["test-mock"]` needed, `#[tokio::test]` pattern, additional skip cases for network/toolchain failure.

## Commands run and outcomes

| Command | Outcome |
|---------|---------|
| `cargo fmt --all` | Fixed one `println!` line-break; clean |
| `cargo fmt --all -- --check` | exit 0 |
| `cargo clippy --workspace --all-targets -- -D warnings` | exit 0 (5.45s compile) |
| `cargo test --workspace` | PASS — spdlog_smoke absent from output (not compiled without test-mock feature) |
| `cargo test --features test-mock --test spdlog_smoke` | 1 test found, shown as `ignored`; exit 0 |

## Deviations from plan

- `required-features = ["test-mock"]` added to `[[test]]` entry. Plan says "No src/ changes" (respected) but does not mention this Cargo.toml-level detail. Necessary for the test to compile when `MockSink` is used in-process.

## Tool failures or retries

- Formatter (pass 1): `cargo fmt --all -- --check` exited 1 — a multi-line `println!` did not match rustfmt's preferred single-line layout. Fixed by running `cargo fmt --all`. Pass 2: exit 0.
- No other retries needed.

## Pre-existing known failures

- `schema_drift::schema_txt_contains_all_promoted_fields` — out of scope per dispatch; present on main before S5.
