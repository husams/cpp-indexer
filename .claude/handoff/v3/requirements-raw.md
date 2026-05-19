# Requirements — Issue 0001 fix

Companion to [`0001-silent-tu-parse-failures.md`](./0001-silent-tu-parse-failures.md). Brief, dev-team-pipeline-ready.

## User story

> As an operator running `cxg-index` (or `cxg-daemon`) against a real C++ codebase,
> I want the indexer to **actually parse the TUs** and to **fail loudly when it can't**,
> so that a broken run cannot pass silently as a successful one.

## Scope

In:

- Strip non-flag tokens from `compile_commands.json` `arguments` before handing them to libclang.
- Add `failed` to the Phase 1 summary, the closing line, and `PipelineStats`.
- Map non-zero failed TUs to exit codes via a new `--fail-on-tu-error <ratio>` flag.
- Propagate the same counter through `cxg-daemon`'s job-status API.

Out:

- Adding a null / dry-run sink.
- Bundling a libclang build; swapping libclang versions.
- Any rework of Phase 5 cross-repo resolution.

## Acceptance criteria

Carried over verbatim from the issue (AC-1 … AC-7). Each must have a test:

| ID    | Subject                            | Test type                  |
| ----- | ---------------------------------- | -------------------------- |
| AC-1  | `resolve_args` sanitisation        | unit                       |
| AC-2  | regression for AC-1                | unit (table)               |
| AC-3  | spdlog parses end-to-end           | integration (`#[ignore]`)  |
| AC-4  | `failed:` in closing summary       | unit (snapshot)            |
| AC-5  | `--fail-on-tu-error` exit codes    | CLI integration            |
| AC-6  | daemon job status field            | API integration            |
| AC-7  | back-compat for existing JSON      | serde round-trip unit      |

## Non-functional requirements

- **No new dependencies.** Use existing `shlex`, `clap`, `serde` machinery.
- **Sanitisation cost ≤ 5 µs per TU** (10k-TU repo budget = 50 ms). Measure with `bencher` if any regex is introduced; prefer linear scan.
- **No change to Parquet shard schema or `SCHEMA_VERSION`.** This fix is read-side only.
- **No change to wire schema** beyond the additive job-status field (back-compat covered by AC-7).

## Risks and mitigations

| Risk                                                                  | Mitigation                                                                                                                  |
| --------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------- |
| Over-aggressive arg stripping removes a genuine flag                  | Whitelist driver-token shapes (`*cc`, `*c++`, `*clang`, `*clang++`, `*gcc`, `*g++`); never strip tokens starting with `-`.  |
| Some CMake exports list source file with relative path, breaking match | Canonicalise both `entry.file` and the trailing token before equality check; fall through if they don't match.              |
| Existing CI pipeline parses the closing summary line                  | Audit `tools/release/` for any consumer; add `failed:` between known tokens; keep token order stable.                       |
| Exit-code change breaks `cxg-daemon` health check or operator scripts | Default `--fail-on-tu-error` is `1.0` (only all-fail → exit 2). Today's partial-failure runs still exit 0. Documented.      |

## Definition of done

- All ACs covered by tests; `cargo test --workspace` green.
- `cargo test --ignored spdlog_smoke` green on macOS arm64 and Linux x86_64 in CI (matrix).
- Manual smoke run on `gabime/spdlog` produces `ok_tu_count == 7, failed == 0` and exit 0.
- Manual run with `--fail-on-tu-error 0.0` against a deliberately-broken `compile_commands.json` produces exit 2 and a `failed: N` line.
- Wiki page `[[pages/code/cpp-indexer]]` updated with the fix and the new CLI flag.
- Issue 0001 closed with PR link.

## Suggested split (for `/dev-team-auto`)

| Stage | Title                                                              | Files-to-touch (preview)                                                | Depends on |
| ----- | ------------------------------------------------------------------ | ----------------------------------------------------------------------- | ---------- |
| S1    | `sanitize_libclang_args` + unit tests (AC-1, AC-2)                 | `src/bootstrap/compile_commands.rs`                                     | —          |
| S2    | Plumb `failed_tu_count` through `PipelineStats` + summary (AC-4)   | `src/pipeline/mod.rs`, `src/bin/index.rs`                               | S1         |
| S3    | `--fail-on-tu-error` flag + exit-code mapping (AC-5)               | `src/bin/index.rs`, `src/pipeline/mod.rs`                               | S2         |
| S4    | Daemon job-status field + back-compat (AC-6, AC-7)                 | `src/api/jobs.rs`, `src/bin/daemon.rs`                                  | S2         |
| S5    | Integration smoke test on spdlog (AC-3) + docs                     | `tests/integration/spdlog_smoke.rs`, `docs/runbooks/libclang-setup.md`  | S1–S4      |

Five small stories, all parallel-unsafe (sequential) because they share `pipeline/mod.rs` and `bin/index.rs` touch points except S4 which can run alongside S3.
