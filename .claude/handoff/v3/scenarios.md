# Scenarios — Issue 0001 fix (tu-parse-fail)

run_id: tu-parse-fail-v3
stage: 2 of 8 — business-analyst
upstream: /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/requirements.md
downstream: architect reads this file

---

## Requirements summary

### In-scope
- Sanitise libclang args: strip compiler-driver token, `-c`/`-o` pairs, and source-file repeat from `compile_commands.json` arguments.
- Surface `failed` counter in Phase 1 summary, closing summary line, and `PipelineStats`.
- Map non-zero failed TUs to exit codes via `--fail-on-tu-error <ratio>`.
- Propagate `failed_tu_count` through `cxg-daemon` `GET /v1/jobs/{id}`.
- Integration smoke test on `gabime/spdlog` (opt-in, `#[ignore]`-gated).

### Out-of-scope
- Null/dry-run sink.
- Bundling or swapping libclang versions.
- Phase 5 cross-repo resolution rework.

### Assumptions
1. (assumed) Gherkin scenarios serve as traceability and QA-mapping artifacts. The project is Rust; QA executes these as `cargo test` / `rstest` tables, not as a pytest-bdd runner. Scenario IDs are the canonical link between requirements and tests.
2. (assumed) The driver-token whitelist in AC-1 (`*cc`, `*c++`, `*clang`, `*clang++`, `*gcc`, `*g++`) is matched against the leading token's basename (or path suffix), not against any token in the array.
3. (assumed) The rule "never strip tokens starting with `-`" (Risks table in requirements.md) covers flags not explicitly enumerated in AC-1 (e.g., `-O*`, `-m*`, `-g*`, `-pthread`, `--sysroot`). See Open question 4.

### Open questions
1. (needs-clarification) **`total_tu_count == 0`** — what exit code does `--fail-on-tu-error` produce when there are no TUs to parse? The ratio comparison `0/0` is undefined. Impacts AC-5 and AC-6 status mapping. Carry to architect.
2. (needs-clarification) **`tools/release/parse-summary.sh`** — does this file exist and does it parse the closing summary line? If yes, the new `failed:` token must not break it. Confirm during PR review; block merge if broken. (S2 / AC-4, from requirements open question 1.)
3. (needs-clarification) **`--fail-on-tu-error` parser shape** — the flag accepts both an `f64` ratio and the sentinel string `never`. Clap shape (custom `FromStr` enum vs separate flag) is an architect decision. (S3 / AC-5, from requirements open question 2.)
4. (needs-clarification) **Flag-prefix whitelist completeness** — AC-1 enumerates specific prefixes for pass-through (`-D`, `-I`, `-std=`, `-W`, `-f`, `-isystem`, `-include`, `-arch`, `-target`). Real compile_commands also carry `-O*`, `-m*`, `-g*`, `-pthread`, `--sysroot`, response files (`@file`). The Risks table says "never strip tokens starting with `-`", which implies a broader default-pass rule. Architect must reconcile and specify the exact strip predicate.
5. (needs-clarification) **Daemon `GET /v1/jobs/{id}` response struct location** — wiki confirms the route exists; architect to verify the struct definition before S4 planning. (S4 / AC-6, from requirements open question 3.)

### Edge cases
- Empty `arguments` array → `resolve_args()` returns empty `Vec` (no crash). (confirmed)
- `arguments` contains only the driver token, no flags. (confirmed)
- Source file path in `arguments` is relative; `entry.file` is absolute (or vice versa) — canonicalisation must match. (confirmed)
- `failed_tu_count / total_tu_count` exactly equals the threshold ratio (boundary: `>=` comparison). (confirmed)
- Daemon JSON record missing `failed_tu_count` field (legacy back-compat, serde default). (confirmed)
- `total_tu_count == 0` — ratio undefined. (needs-clarification)
- Leading token `foo.cc` (source file, not a driver) — must not be stripped. (assumed — architect must spec driver-detector shape)

### Stakeholders
- Operators running `cxg-index` against real C++ codebases (root user of S1–S3).
- CI pipelines relying on exit codes (root user of S3).
- Operators monitoring `cxg-daemon` job status (root user of S4).
- Developers maintaining the indexer regression suite (root user of S5).

---

## Gherkin

### Feature: S1 — Sanitise libclang args

```gherkin
Feature: Sanitise libclang compile_commands arguments before passing to libclang
  # Covers: AC-1, AC-2
  # References: requirements.md S1; docs/issues/0001-silent-tu-parse-failures.md §Root cause — Bug B

  @AC-1 @confirmed
  Scenario: Happy path — driver, -c/-o pair, source repeat, and flags present
    Given a compile_commands entry with arguments:
      | /usr/bin/clang++ | -c | foo.cpp | -o | foo.o | -std=c++17 | -DFOO=1 | -I/inc | foo.cpp |
    And entry.file is "/abs/path/foo.cpp"
    When resolve_args() is called with the entry
    Then the returned Vec contains exactly ["-std=c++17", "-DFOO=1", "-I/inc"]
    And the returned Vec does NOT contain "/usr/bin/clang++"
    And the returned Vec does NOT contain "-c"
    And the returned Vec does NOT contain "foo.cpp"
    And the returned Vec does NOT contain "-o"
    And the returned Vec does NOT contain "foo.o"

  @AC-1 @confirmed
  Scenario: All enumerated pass-through prefixes survive stripping
    Given a compile_commands entry with arguments:
      | gcc | -c | bar.cpp | -o | bar.o | -DNDEBUG | -I/usr/include | -std=c++14 | -Wall | -fPIC | -isystem/sys | -include pch.h | -arch arm64 | -target aarch64-apple-macos12 | bar.cpp |
    And entry.file is "/abs/bar.cpp"
    When resolve_args() is called with the entry
    Then the returned Vec contains "-DNDEBUG"
    And the returned Vec contains "-I/usr/include"
    And the returned Vec contains "-std=c++14"
    And the returned Vec contains "-Wall"
    And the returned Vec contains "-fPIC"
    And the returned Vec contains "-isystem/sys"
    And the returned Vec contains "-include"
    And the returned Vec contains "pch.h"
    And the returned Vec contains "-arch"
    And the returned Vec contains "arm64"
    And the returned Vec contains "-target"
    And the returned Vec contains "aarch64-apple-macos12"

  @AC-1 @confirmed
  Scenario: Empty arguments array returns empty Vec
    Given a compile_commands entry with an empty arguments array
    And entry.file is "/abs/empty.cpp"
    When resolve_args() is called with the entry
    Then the returned Vec is empty
    And no panic or error is produced

  @AC-1 @confirmed
  Scenario: Arguments contains only the driver token
    Given a compile_commands entry with arguments:
      | clang++ |
    And entry.file is "/abs/only.cpp"
    When resolve_args() is called with the entry
    Then the returned Vec is empty

  @AC-1 @confirmed
  Scenario: Source file path in arguments is relative, entry.file is absolute (canonicalisation)
    Given a compile_commands entry with arguments:
      | g++ | -c | src/rel.cpp | -o | rel.o | -DREL=1 | src/rel.cpp |
    And entry.file is "/project/src/rel.cpp"
    And the working directory for the entry is "/project"
    When resolve_args() is called with the entry
    Then the returned Vec contains "-DREL=1"
    And the returned Vec does NOT contain "src/rel.cpp"

  @AC-1 @assumed
  Scenario: Leading token is a source file ending in .cc, not a driver — must not be stripped
    Given a compile_commands entry with arguments:
      | foo.cc | -std=c++17 | -DFOO=1 |
    And entry.file is "/abs/foo.cc"
    When resolve_args() is called with the entry
    Then the returned Vec contains "-std=c++17"
    And the returned Vec contains "-DFOO=1"
    # Behaviour of leading "foo.cc" token (strip or pass-through) is needs-clarification:
    # architect must specify whether driver detection is basename-suffix match or full-path match
    # to distinguish "/usr/bin/gcc" from a source file whose name ends in a driver suffix.

  @AC-1 @confirmed
  Scenario Outline: Table-driven — driver shapes are all detected and stripped (AC-2)
    Given a compile_commands entry with arguments:
      | <driver> | -c | src.cpp | -o | src.o | -std=c++17 | src.cpp |
    And entry.file is "/abs/src.cpp"
    When resolve_args() is called with the entry
    Then the returned Vec contains exactly ["-std=c++17"]
    And the returned Vec does NOT contain "<driver>"

    @AC-2 @confirmed
    Examples:
      | driver               |
      | cc                   |
      | c++                  |
      | clang                |
      | clang++              |
      | gcc                  |
      | g++                  |
      | /usr/bin/clang++     |
      | /usr/local/bin/gcc   |
      | arm-linux-gnueabi-g++|
```

---

### Feature: S2 — Surface failed-TU counter in pipeline summary

```gherkin
Feature: Closing summary line includes failed TU count
  # Covers: AC-4
  # References: requirements.md S2; docs/issues/0001-silent-tu-parse-failures.md §Root cause — Bug A

  @AC-4 @confirmed
  Scenario: Summary line with failed > 0 follows the required format
    Given a PipelineStats value with total=10, partial=2, failed=3, nodes=500, edges=1200
    When the closing summary is formatted
    Then the output line matches exactly:
      "cxg-index: done — 10 TUs | 2 partial | 3 failed | 500 nodes | 1200 edges"

  @AC-4 @confirmed
  Scenario: Summary line with failed = 0 still includes the failed token
    Given a PipelineStats value with total=7, partial=0, failed=0, nodes=300, edges=700
    When the closing summary is formatted
    Then the output line matches exactly:
      "cxg-index: done — 7 TUs | 0 partial | 0 failed | 300 nodes | 700 edges"

  @AC-4 @confirmed
  Scenario: Token order is stable — failed appears between partial and nodes
    Given any PipelineStats value
    When the closing summary is formatted
    Then the token order in the line is: TUs, partial, failed, nodes, edges
    And no existing token is removed or renamed
```

---

### Feature: S3 — Exit-code policy via --fail-on-tu-error

```gherkin
Feature: cxg-index exits non-zero when failed TU ratio meets or exceeds threshold
  # Covers: AC-5
  # References: requirements.md S3; docs/issues/0001-silent-tu-parse-failures.md §AC-5

  @AC-5 @confirmed
  Scenario: Default ratio 1.0 — all TUs fail → exit 2
    Given cxg-index is invoked without --fail-on-tu-error (default ratio 1.0)
    And the pipeline processes 5 TUs, all 5 of which fail to parse
    When the process terminates
    Then the exit code is 2

  @AC-5 @confirmed
  Scenario: Default ratio 1.0 — partial failure → exit 0
    Given cxg-index is invoked without --fail-on-tu-error (default ratio 1.0)
    And the pipeline processes 5 TUs, 2 of which fail to parse
    When the process terminates
    Then the exit code is 0

  @AC-5 @confirmed
  Scenario: Ratio 0.0 — any failure → exit 2
    Given cxg-index is invoked with --fail-on-tu-error 0.0
    And the pipeline processes 5 TUs, 1 of which fails to parse
    When the process terminates
    Then the exit code is 2

  @AC-5 @confirmed
  Scenario: Sentinel "never" — all TUs fail → exit 0
    Given cxg-index is invoked with --fail-on-tu-error never
    And the pipeline processes 5 TUs, all 5 of which fail to parse
    When the process terminates
    Then the exit code is 0

  @AC-5 @confirmed
  Scenario: Threshold boundary — ratio exactly met (>=) → exit 2
    Given cxg-index is invoked with --fail-on-tu-error 0.5
    And the pipeline processes 4 TUs, exactly 2 of which fail to parse (ratio == 0.5)
    When the process terminates
    Then the exit code is 2

  @AC-5 @confirmed
  Scenario: Threshold boundary — ratio just below threshold → exit 0
    Given cxg-index is invoked with --fail-on-tu-error 0.5
    And the pipeline processes 4 TUs, 1 of which fails to parse (ratio == 0.25)
    When the process terminates
    Then the exit code is 0

  @AC-5 @confirmed
  Scenario: Invalid ratio value above 1.0 → CLI parse error, non-zero exit
    Given cxg-index is invoked with --fail-on-tu-error 1.5
    When the process terminates
    Then the exit code is non-zero (CLI argument parse error)
    And the error message references the invalid value

  @AC-5 @confirmed
  Scenario: Invalid ratio value below 0.0 → CLI parse error, non-zero exit
    Given cxg-index is invoked with --fail-on-tu-error -0.1
    When the process terminates
    Then the exit code is non-zero (CLI argument parse error)
    And the error message references the invalid value

  @AC-5 @confirmed
  Scenario: Non-numeric, non-sentinel string → CLI parse error, non-zero exit
    Given cxg-index is invoked with --fail-on-tu-error garbage
    When the process terminates
    Then the exit code is non-zero (CLI argument parse error)
    And the error message references the invalid value

  @AC-5 @needs-clarification
  Scenario: Zero TUs processed with --fail-on-tu-error 0.0
    Given cxg-index is invoked with --fail-on-tu-error 0.0
    And the compile_commands.json contains no entries
    When the process terminates
    Then the exit code is ??? (0/0 ratio undefined — needs architect decision)
    # See Open question 1
```

---

### Feature: S4 — Daemon job-status field and back-compat

```gherkin
Feature: GET /v1/jobs/{id} returns failed_tu_count and correct status transitions
  # Covers: AC-6, AC-7
  # References: requirements.md S4; docs/issues/0001-silent-tu-parse-failures.md §AC-6, §AC-7

  @AC-6 @confirmed
  Scenario: No failures — status is "completed", failed_tu_count is 0
    Given a completed indexing job with total_tu_count=7 and failed_tu_count=0
    When GET /v1/jobs/{id} is called
    Then the response body contains "status": "completed"
    And the response body contains "failed_tu_count": 0

  @AC-6 @confirmed
  Scenario: Partial failures — status is "completed_with_errors"
    Given a completed indexing job with total_tu_count=7 and failed_tu_count=2
    When GET /v1/jobs/{id} is called
    Then the response body contains "status": "completed_with_errors"
    And the response body contains "failed_tu_count": 2

  @AC-6 @confirmed
  Scenario: All TUs failed — status is "failed"
    Given a completed indexing job with total_tu_count=7 and failed_tu_count=7
    When GET /v1/jobs/{id} is called
    Then the response body contains "status": "failed"
    And the response body contains "failed_tu_count": 7

  @AC-6 @confirmed
  Scenario: failed_tu_count is an integer in all status transitions
    Given completed indexing jobs covering all three status transitions
    When GET /v1/jobs/{id} is called for each
    Then "failed_tu_count" is a JSON integer (not a string or float) in every response

  @AC-7 @confirmed
  Scenario: Legacy JSON record without failed_tu_count deserialises without error
    Given a persisted job JSON record that does not contain the "failed_tu_count" field
    When the daemon deserialises the record
    Then no error or panic is produced
    And the deserialised struct presents failed_tu_count as 0 (serde default)

  @AC-7 @confirmed
  Scenario: No existing client-visible field is removed or renamed
    Given the current GET /v1/jobs/{id} response shape (fields present before this fix)
    When GET /v1/jobs/{id} is called after the fix is applied
    Then every field that existed before the fix is still present with the same name and type
    And the "failed_tu_count" field is additive (new)

  @AC-6 @needs-clarification
  Scenario: Job completed with zero total TUs and zero failures
    Given a completed indexing job with total_tu_count=0 and failed_tu_count=0
    When GET /v1/jobs/{id} is called
    Then the response body contains "status": ??? (edge: 0/0 — needs architect decision)
    # See Open question 1
```

---

### Feature: S5 — Integration smoke test on spdlog

```gherkin
Feature: Opt-in integration test validates arg sanitisation against gabime/spdlog
  # Covers: AC-3
  # References: requirements.md S5; docs/issues/0001-silent-tu-parse-failures.md §Reproduction, §AC-3

  @AC-3 @confirmed
  Scenario: spdlog smoke test passes with ok_tu_count >= 6 on macOS arm64 and Linux x86_64
    Given the test is gated with #[ignore] and not(target_os = "windows")
    And gabime/spdlog at HEAD is cloned and compile_commands.json is generated via CMake
    When the indexer pipeline is run against spdlog's compile_commands.json
    Then ok_tu_count is at least 6 out of 7 TUs
    And the process exits with code 0

  @AC-3 @confirmed
  Scenario: spdlog smoke test is skipped on Windows
    Given the test is executed on Windows (target_os = "windows")
    When the test runner encounters the spdlog smoke test
    Then the test is skipped without failure

  @AC-3 @confirmed
  Scenario: CI matrix invokes the smoke test via cargo test --ignored spdlog_smoke
    Given CI is configured with a matrix covering macOS arm64 and Linux x86_64
    When the spdlog smoke stage runs
    Then it executes: cargo test --ignored spdlog_smoke
    And any non-zero exit fails the CI build
```

---

## References

- `/Users/husam/workspace/cpp-indexer/.claude/handoff/v3/requirements.md` (PM output, upstream)
- `/Users/husam/workspace/cpp-indexer/.claude/handoff/v3/CHARTER.md`
- `/Users/husam/workspace/cpp-indexer/docs/issues/0001-silent-tu-parse-failures.md`
- Wiki: `[[pages/code/cpp-indexer]]` — confirms `GET /v1/jobs/{id}` route, M1–M8 status
- Cognee tags: `task:tu-parse-fail`, `role:business-analyst`
