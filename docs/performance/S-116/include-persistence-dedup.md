# S-116 include persistence de-duplication

The before values are the accepted S-111 `--profile-json` baseline named in
S-116. The after captures were taken from fresh disposable databases on the
same arm64 macOS host with the release build from this branch. Absolute
worktree paths in the raw JSON are normalized to `{WORKTREE}`.

| Scope | Before | After | After / baseline | Result |
| --- | ---: | ---: | ---: | --- |
| `src/query/exec.cpp`, `--jobs 1` | 0.98s | 0.011890s | 1.21% | pass |
| five-TU batch, `--jobs 5` | 7.90s | 0.066587s | 0.84% | pass (limit: 30%) |

The five-TU capture indexed `src/query/exec.cpp`, `src/query/plan.cpp`,
`src/query/cxq.cpp`, `src/cli/commands.cpp`, and
`src/ast/index_engine.cpp`. Its include-family telemetry reports 94,029
attempted facts, 745 persisted facts, and 93,284 claim duplicates. The header
claim gate granted 104 candidates and denied 28 already owned in-flight.

Correctness is executable rather than inferred from timing:

- `fact_batch_writer_contract_test` proves that five TU configuration rows and
  all five main-file include associations are retained while the shared header
  edge, site, and macro-use rows are written once by the first claimant.
- `parallel_index_database_test` indexes a real five-TU Clang corpus serially
  and in parallel, compares canonical include projections, and checks the same
  per-TU main/header include results from both databases.
- `parallel_extraction_test` exercises concurrent claims for the same shared
  header and requires exactly one owner in legacy rank order.

Artifacts:

- `before-s111-baseline.json` — normalized accepted baseline fields and source.
- `after-single-tu.json` — raw post-change `--profile-json` capture.
- `after-five-tu.json` — raw post-change `--profile-json` capture.
