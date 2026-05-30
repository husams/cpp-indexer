# PCM Support — Scenarios

## Requirements

### In-scope
- Detection of PCM TUs at dispatch (`is_module_tu`, `probe_cpp20_support`)
- Routing PCM/module TUs to `parse_module_tu()`
- Loud diagnostics on missing or invalid `.pcm` files
- Integration test covering mixed `compile_commands.json`
- Documentation (S5, non-behavioral — AC verified by doc presence, not a pipeline scenario)

### Out-of-scope
- RSS / compact-ingest-path memory goals
- Schema or backend redesign
- System-header nodes
- gRPC (M9+)

### Assumptions
- `assumed` — `-fmodules` flag routes to the module parser (C++20 header-modules path); architect must confirm whether `-fmodules` alone is sufficient for `is_module_tu()=true` or requires a C++20 language-mode flag co-present.
- `confirmed` — `MODULE` node and `module_interface` attr already exist; no schema changes needed.
- `confirmed` — libclang < 18 is a runtime probe; compile-time gating is not sufficient.
- `confirmed` — missing-`.pcm` and corrupt-`.pcm` scenarios apply only to commands carrying `-fmodule-file=` or `-fprebuilt-module-path`; `-fmodules`-only has no addressable `.pcm` path.

### Open questions
1. **needs-clarification** (S3) — Does libclang surface a distinct error code for module-load failure vs. general parse failure? Implementation must verify; the diagnostic should be attributable to the PCM-load step specifically if the API allows it.
2. **needs-clarification** (S4) — What is the standard libclang-dependent fixture pattern in this repo? Confirm against existing `symbol_id_integration.rs` pattern before writing the test harness.
3. **needs-clarification** (S2-AC4, S3-AC3, S4-AC2) — Failure signaling contract is defined as "non-zero exit code OR machine-readable summary with `failed_tus > 0`". The `OR` is undecided. Architect/developer must settle the contract before implementation; these scenarios reflect the disjunction.

### Edge cases
- `confirmed` — `-fprebuilt-module-path=<dir>` with no `-fmodule-file=` entry (directory given, no explicit file): detection must fire.
- `confirmed` — TU with both `-fmodule-file=name=path` and `-fprebuilt-module-path=dir` in same command: still a single PCM TU, not double-counted.
- `confirmed` — libclang < 18 present but PCM flags present in compile_commands: skip must be LOUD (warn), not silent.
- `assumed` — corrupt-`.pcm` (truncated file, wrong magic) is distinguishable from missing-`.pcm` in the log message; if libclang does not distinguish them, both are treated as fatal and logged as "PCM load failure".

### Stakeholders
- Indexer pipeline (S1, S2, S3)
- Operators indexing C++20/PCM codebases (S3)
- CI / developers (S4)
- End users / integrators (S5)

---

## Gherkin

```gherkin
Feature: PCM TU detection at dispatch (S1)

  Scenario: fprebuilt-module-path flag detected as module TU
    # S1-AC1 | confirmed
    Given a compile command containing "-fprebuilt-module-path=/build/modules"
    When is_module_tu() is called on that compile command
    Then it returns true

  Scenario: fmodule-file flag detected as module TU
    # S1-AC2 | confirmed
    Given a compile command containing "-fmodule-file=Foo=/build/Foo.pcm"
    When is_module_tu() is called on that compile command
    Then it returns true

  Scenario: fmodules-only flag detected as module TU
    # S1 implied by dispatch requirement | assumed (see open assumption on fmodules routing)
    Given a compile command containing "-fmodules" and no other PCM or C++20 module flags
    When is_module_tu() is called on that compile command
    Then it returns true

  Scenario: standard TU without module flags not detected as module TU
    # S1-AC3 | confirmed
    Given a compile command with no "-fmodules", "-fmodule-file=", or "-fprebuilt-module-path" flags
    When is_module_tu() is called on that compile command
    Then it returns false

  Scenario: libclang older than version 18 — capability probe returns absent, no crash
    # S1-AC4 | confirmed
    Given the runtime libclang version is below 18
    When probe_cpp20_support() is called
    Then it returns a capability-absent result
    And a WARNING-level log line is emitted indicating PCM support is unavailable
    And the process does not panic and does not exit with code 0 due to the probe alone


Feature: PCM TU routing in parallel pipeline (S2)

  Scenario: module TU routed to parse_module_tu
    # S2-AC1 | confirmed
    Given a TU where is_module_tu() returns true
    When the parallel pipeline processes that TU
    Then parse_module_tu() is invoked for that TU
    And visit_tu_inner() is NOT invoked for that TU

  Scenario: standard TU routed to visit_tu_inner — no regression
    # S2-AC2 | confirmed
    Given a TU where is_module_tu() returns false
    When the parallel pipeline processes that TU
    Then visit_tu_inner() is invoked for that TU
    And parse_module_tu() is NOT invoked for that TU

  Scenario: mixed compile_commands — both TU kinds produce graph output
    # S2-AC3 | confirmed
    Given a compile_commands.json containing at least one standard TU and at least one PCM-consuming TU
    And all referenced .pcm files exist on disk and are valid
    And libclang >= 18 is present at runtime
    When the indexer runs to completion against that compile_commands.json
    Then graph output is produced for each standard TU
    And graph output is produced for each PCM-consuming TU
    And the process exits with code 0

  Scenario: PCM flags present but libclang reports PCM support absent — loud skip
    # S2-AC4 | needs-clarification (exit-code vs failed_tus contract undecided)
    Given a TU carrying PCM flags ("-fmodule-file=" or "-fprebuilt-module-path")
    And libclang at runtime reports PCM support is absent
    When the parallel pipeline processes that TU
    Then the TU is skipped (not silently partially parsed)
    And a WARNING or ERROR level log line is emitted naming the skipped TU
    And when the indexer run finishes, EITHER the process exits with a non-zero exit code OR the run summary reports a non-zero skip_count or failed_tus value
    # needs-clarification: which branch of the OR is the contract


Feature: Loud diagnostic on missing or invalid .pcm file (S3)

  Scenario: missing .pcm file — ERROR log and no partial graph
    # S3-AC1 | confirmed
    Given a compile command referencing "-fmodule-file=Foo=/build/Foo.pcm"
    And /build/Foo.pcm does not exist on disk
    When the indexer processes that TU
    Then an ERROR-level log line is emitted naming the missing file path "/build/Foo.pcm"
    And the log line also names the TU source file
    And no graph nodes or edges are emitted for that TU

  Scenario: corrupt or truncated .pcm file — ERROR log and no graph output
    # S3-AC2 | confirmed
    Given a compile command referencing "-fmodule-file=Foo=/build/Foo.pcm"
    And /build/Foo.pcm exists on disk but is corrupt or truncated
    When the indexer processes that TU
    Then an ERROR-level log line is emitted for that TU
    And no graph nodes or edges are emitted for that TU

  Scenario: .pcm failure causes non-zero outcome
    # S3-AC3 | needs-clarification (exit-code vs failed_tus contract undecided)
    Given at least one TU in the run has a missing or corrupt .pcm file
    When the indexer run finishes
    Then EITHER the process exits with a non-zero exit code OR the run emits a machine-readable summary where failed_tus > 0
    # needs-clarification: which branch of the OR is the contract

  Scenario: all .pcm files present and valid — no spurious error diagnostics
    # S3-AC4 | confirmed
    Given a compile_commands.json where all referenced .pcm files exist on disk and are well-formed
    And libclang >= 18 is present at runtime
    When the indexer runs to completion
    Then no ERROR-level log lines related to PCM loading are emitted


Feature: Integration test — mixed compile_commands with PCM and standard TUs (S4)

  Scenario: fixture with both TU kinds — both indexed, exit 0
    # S4-AC1 | confirmed
    Given a test fixture compile_commands.json with at least one standard TU and one PCM-consuming TU
    And the fixture provides a prebuilt .pcm file for the PCM-consuming TU
    And libclang >= 18 is present at runtime
    When the indexer runs against the fixture
    Then both TUs appear in the graph output
    And the process exits with code 0

  Scenario: fixture with .pcm removed — asserts failure
    # S4-AC2 | needs-clarification (exit-code vs failed_tus contract undecided)
    Given the same test fixture as above
    And the .pcm file has been removed from disk
    When the indexer runs against the fixture
    Then EITHER the process exits with a non-zero exit code OR the run summary reports failed_tus > 0
    # needs-clarification: which branch of the OR is the contract

  Scenario: test skips cleanly when libclang < 18 detected
    # S4-AC3 | confirmed
    Given libclang < 18 is detected at test setup time
    When the integration test for PCM support is invoked
    Then the test is skipped with a logged reason (not marked as failed)
    And CI does not fail due to this skip
```

---

## References

- Handoff: `/Users/husam/workspace/cpp-indexer/.claude/handoff/pcm/requirements.md`
- Charter: `/Users/husam/workspace/cpp-indexer/.claude/handoff/pcm/CHARTER.md`
- Source refs: `src/visit/modules_cpp20.rs:66,168,203`; `src/pipeline/parallel.rs:176`
- Wiki: `[[pages/planning/cpp-indexer-compact-ingest-path]]`
- Issue: Issue 0001 (silent total TU parse failure — PCM S3 closes this failure family for PCM TUs)
- Cognee tags: `task:pcm-support role:business-analyst`
