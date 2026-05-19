# Requirements — Issue 0001 fix (tu-parse-fail)

run_id: tu-parse-fail-v3
stage: 1 of 8 — product-manager

---

## Scope

**In:**
- Strip non-flag tokens from `compile_commands.json` `arguments` before handing them to libclang.
- Add `failed` counter to the Phase 1 summary, the closing line, and `PipelineStats`.
- Map non-zero failed TUs to exit codes via a new `--fail-on-tu-error <ratio>` flag.
- Propagate the same counter through `cxg-daemon`'s job-status API (`GET /v1/jobs/{id}`).

**Out:**
- Adding a null / dry-run sink.
- Bundling a libclang build or swapping libclang versions.
- Any rework of Phase 5 cross-repo resolution.

---

## Stories

### S1 — Sanitise libclang args

Story: As an operator running `cxg-index` against a real C++ codebase, I want the compiler-driver token, `-c`/`-o` flag pairs, and the source-file repeat stripped from `compile_commands.json` arguments before they are handed to libclang, so that TUs that compile cleanly with the system compiler are also parsed successfully by the indexer.

Acceptance criteria:
  - AC-1: Given a CMake-style `arguments` array containing a leading compiler-driver token (path ending in `cc`, `c++`, `clang`, `clang++`, `gcc`, or `g++`), any `-c <src>` pair, any `-o <obj>` pair, and the trailing source-file token matching `entry.file` (canonicalised), when `resolve_args()` is called, then the returned `Vec<String>` MUST NOT contain any of those tokens; tokens starting with `-D`, `-I`, `-std=`, `-W`, `-f`, `-isystem`, `-include`, `-arch`, `-target` MUST pass through unchanged.
  - AC-2: A unit test using a table of CMake-style compile-command entries (driver + `-c` + `-o` + source + flags) MUST assert that `resolve_args()` returns only the flag set for every row in the table.

Priority: P0 — root cause of Bug B; all other stories depend on this fix.
Dependencies: none
Open questions: none
References:
  - `/Users/husam/workspace/cpp-indexer/docs/issues/0001-silent-tu-parse-failures.md` §Root cause — Bug B
  - `/Users/husam/workspace/cpp-indexer/.claude/handoff/v3/requirements-raw.md` §Suggested split S1

---

### S2 — Surface failed-TU counter in pipeline summary

Story: As an operator reviewing indexer output, I want the closing summary line to include a `failed: <N>` count, so that a run with parse failures is visually distinguishable from a fully-successful run without inspecting WARN-level logs.

Acceptance criteria:
  - AC-4: The closing summary line MUST follow the format `cxg-index: done — <T> TUs | <P> partial | <F> failed | <N> nodes | <E> edges`. A snapshot unit test MUST assert this format for a synthetic `PipelineStats` value with `failed > 0`. The token order MUST remain stable; the new `failed:` field MUST appear between `partial` and `nodes`.

Priority: P0 — without this field, Bug A (silent failure) remains partially visible even after S1.
Dependencies: S1
Open questions:
  - AC-7 note: confirm whether `tools/release/parse-summary.sh` parses the closing summary line; if it exists, the new `failed:` token MUST not break it (verify during PR, not before).
References:
  - `/Users/husam/workspace/cpp-indexer/docs/issues/0001-silent-tu-parse-failures.md` §Root cause — Bug A
  - `/Users/husam/workspace/cpp-indexer/.claire/handoff/v3/requirements-raw.md` §Suggested split S2

---

### S3 — Exit-code policy via `--fail-on-tu-error`

Story: As an operator or CI pipeline invoking `cxg-index`, I want the process to exit non-zero when failed TUs exceed a configurable ratio threshold, so that broken indexing runs are caught automatically rather than silently passing.

Acceptance criteria:
  - AC-5: A new CLI flag `--fail-on-tu-error <ratio>` (default `1.0`) MUST be accepted by `cxg-index`. When `failed_tu_count / total_tu_count >= ratio`, the process MUST exit with code `2`. The flag MUST accept any `f64` in `[0.0, 1.0]` and the sentinel value `never` (always exit 0). Default behaviour (ratio `1.0`): exit `2` only when all TUs fail; existing partial-success runs continue to exit `0`. CLI integration tests MUST cover: ratio `1.0` with all-fail input → exit 2; ratio `0.0` with any-fail input → exit 2; ratio `1.0` with partial-fail input → exit 0; `never` with all-fail input → exit 0.

Priority: P0 — operators and CI cannot rely on process exit code without this fix.
Dependencies: S2
Open questions:
  - The flag accepts both a floating-point ratio and the string `never` — the Clap parser shape (enum vs custom `FromStr`) is an architect decision; surface for ADR.
References:
  - `/Users/husam/workspace/cpp-indexer/docs/issues/0001-silent-tu-parse-failures.md` §Acceptance criteria AC-5
  - `/Users/husam/workspace/cpp-indexer/.claude/handoff/v3/requirements-raw.md` §Suggested split S3

---

### S4 — Daemon job-status field and back-compat

Story: As an operator monitoring `cxg-daemon`, I want `GET /v1/jobs/{id}` to include `failed_tu_count` and a `status` value of `completed_with_errors` or `failed` when TU parse failures occurred, so that long-running background indexing jobs surface the same signal that the CLI would print.

Acceptance criteria:
  - AC-6: `GET /v1/jobs/{id}` MUST return `failed_tu_count` (integer) in the response body. `status` MUST be `"completed_with_errors"` when `failed > 0 && failed < total`, `"failed"` when `failed == total`, and remain `"completed"` when `failed == 0`. An API integration test MUST cover all three status transitions.
  - AC-7: Existing JSON job records that lack the `failed_tu_count` field MUST deserialise without error (serde `#[serde(default)]` or equivalent). The new field MUST be additive; no existing client-visible fields may be removed or renamed.

Priority: P1 — daemon parity is important but does not block the CLI fix (S1–S3).
Dependencies: S2
Open questions:
  - Confirm that `GET /v1/jobs/{id}` is already implemented in the daemon (wiki `[[pages/code/cpp-indexer]]` confirms the route exists; implementation detail is for architect review).
References:
  - `/Users/husam/workspace/cpp-indexer/docs/issues/0001-silent-tu-parse-failures.md` §Acceptance criteria AC-6, AC-7
  - `/Users/husam/workspace/cpp-indexer/.claude/handoff/v3/requirements-raw.md` §Suggested split S4
  - Wiki: `[[pages/code/cpp-indexer]]` — confirms `GET /v1/jobs/{id}` route

---

### S5 — Integration smoke test on spdlog + docs update

Story: As a developer maintaining the indexer, I want an opt-in integration test that clones spdlog and asserts successful TU parse after the sanitisation fix, so that regressions in libclang arg handling are caught in CI on macOS arm64 and Linux x86_64 before merge.

Acceptance criteria:
  - AC-3: An integration test gated behind `#[ignore]` MUST clone `gabime/spdlog` at HEAD, generate `compile_commands.json` via CMake, run the indexer pipeline, and assert `ok_tu_count >= 6` out of 7 TUs (allowing 1 partial for header-only edge cases). The test MUST be skipped on Windows (`#[cfg(not(target_os = "windows"))]` or equivalent). CI MUST run it via `cargo test --ignored spdlog_smoke` in a matrix covering macOS arm64 and Linux x86_64.

Priority: P1 — validates the end-to-end fix; gated `--ignored` so it does not block the fast test suite.
Dependencies: S1, S2, S3, S4
Open questions: none
References:
  - `/Users/husam/workspace/cpp-indexer/docs/issues/0001-silent-tu-parse-failures.md` §Reproduction, §Acceptance criteria AC-3
  - `/Users/husam/workspace/cpp-indexer/.claude/handoff/v3/requirements-raw.md` §Suggested split S5

---

## Non-functional requirements

- **No new dependencies.** Use existing `shlex`, `clap`, `serde` machinery.
- **Sanitisation cost ≤ 5 µs per TU** (10k-TU repo budget = 50 ms). Prefer linear scan over regex; measure with `bencher` if regex is introduced.
- **No change to Parquet shard schema or `SCHEMA_VERSION`.** This fix is read-side only.
- **No change to wire schema** beyond the additive job-status field (back-compat covered by AC-7).

---

## Risks and mitigations

| Risk | Mitigation |
| ---- | ---------- |
| Over-aggressive arg stripping removes a genuine flag | Whitelist driver-token shapes (`*cc`, `*c++`, `*clang`, `*clang++`, `*gcc`, `*g++`); never strip tokens starting with `-`. |
| CMake exports list source file with relative path, breaking match | Canonicalise both `entry.file` and the trailing token before equality check; fall through if they don't match. |
| Existing CI pipeline parses the closing summary line | Audit `tools/release/` for any consumer; add `failed:` between known tokens; keep token order stable (see AC-7 open question). |
| Exit-code change breaks `cxg-daemon` health check or operator scripts | Default `--fail-on-tu-error` is `1.0` (only all-fail → exit 2). Today's partial-failure runs still exit 0. Documented in AC-5. |

---

## Definition of done

- All ACs (AC-1 through AC-7) covered by tests; `cargo test --workspace` green.
- `cargo test --ignored spdlog_smoke` green on macOS arm64 and Linux x86_64 in CI (matrix).
- Manual smoke run on `gabime/spdlog` produces `ok_tu_count == 7, failed == 0` and exit 0.
- Manual run with `--fail-on-tu-error 0.0` against a deliberately-broken `compile_commands.json` produces exit 2 and a `failed: N` line.
- Wiki page `[[pages/code/cpp-indexer]]` updated with the fix and the new CLI flag.
- Issue 0001 closed with PR link.

---

## Open questions (consolidated)

1. **`tools/release/parse-summary.sh`** — does this file exist and does it parse the closing summary line? If yes, the new `failed:` token must not break it. Confirm during PR; block merge if broken (S2 / AC-7).
2. **`--fail-on-tu-error` type shape** — the flag accepts both an `f64` ratio and the sentinel string `never`. Clap parser shape (custom `FromStr` enum vs separate `--never-fail-on-tu-error` bool flag) is an architect decision (S3 / AC-5).
3. **Daemon `GET /v1/jobs/{id}` implementation** — wiki confirms the route exists; architect to verify the response struct location before S4 planning (S4 / AC-6).
