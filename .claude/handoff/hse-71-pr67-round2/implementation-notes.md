Files changed:
- architecture/cidx-self-host-policy.json (recomputed 35 mis-keyed direct-construction baseline `col` values)
- architecture/cidx-module-manifest.json (new `mutuallyExclusiveSourceGroups` for souffle_stub.cpp/souffle_runner.cpp)
- scripts/self_host_architecture_report.py (external-symbol split for `unwitnessedCallSites`/new `externalCallSites`; mutually-exclusive TU group handling in `check_index_coverage`)
- tests/self_host_architecture_test.py (3 new regression tests + 2 new module-scope helpers)
- .github/workflows/architecture.yml (self-host report step now `continue-on-error: true`, disclosed/temporary)
- docs/self-host-architecture.md (documents all of the above plus the `queryUnknown` open follow-up)

Tests added/run:
- `python3 -m pytest tests/self_host_architecture_test.py -q` -> 80/80 passed (was 77 before this round; 3 new tests added)
- Mutation-proved each new test red-without-fix / green-with-fix (see log for exact commands):
  - test_real_policy_construction_baselines_are_keyed_at_the_variable_not_the_type
  - test_call_site_through_a_known_external_symbol_is_not_unwitnessed (+ negative control against test_deleted_file_row_with_a_dangling_call_edge_fails_closed)
  - test_mutually_exclusive_source_group_satisfied_by_either_member (+ negative control: neither member indexed still fails closed)
- `python3 scripts/check_architecture.py --manifest architecture/cidx-module-manifest.json` -> pass
- `python3 scripts/check_platform_contract.py --module-manifest architecture/cidx-module-manifest.json` -> pass
- `python3 tests/architecture_test.py` -> 27/27 passed
- `python3 tests/platform_contract_test.py` -> 9/9 passed
- `python3 scripts/generate_contracts.py --check` -> exit 0
- `python3 scripts/check_release_contract.py` -> exit 1, sole error is the pre-approved index.db schema 39-vs-40
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4` -> exit 0, clean
- `ctest --test-dir build -L default --output-on-failure` -> 32/32 passed
- `ctest --test-dir build -L clang --output-on-failure` -> 8/8 passed
- `python3 -m pytest python/tests -q` -> 1154 passed, 2 skipped, 3 failed (all pre-approved: test_contracts.py schema-39-vs-40; test_repository.py x2 checkout-basename `cpp-indexer-hse-71`)
- tests/e2e NOT run this round: no C++/goldens/computed-symbol-fact changes were made (scripts/tests/docs/manifest/policy/workflow only)
- clang-tidy/clang-format NOT run this round: no .cpp/.hpp files were touched

Deviations from plan:
- Could not produce a real, completed self-host index run (`cidx import`/`index`/`resolve` are forbidden this round per hard constraints, and prior rounds show this repo's self-index does not reliably finish on this shared machine). Per both reviewers' own explicit fallback recommendation, made the self-host CI job's report step `continue-on-error: true` (disclosed, temporary) instead of asserting an unproven `status: pass`.
- `queryUnknown` (a `newFindings` item from the senior-dev verdict, not one of the 4 numbered blocking reasons) is documented as an open follow-up in docs/self-host-architecture.md, not root-caused/fixed this round -- it likely intersects unrelated in-progress possible-call-ambiguity work (HSE-78/80) and fixing it blind, unverified against a real self-index, risked scope creep without proof.

Follow-ups (tag sr-dev / devops):
- Provision a machine/timeout able to complete a real self-index of this repository (~150 TUs) and run the self-host CI job once for real, then flip `continue-on-error` back off in .github/workflows/architecture.yml.
- Root-cause `index.coverage.queryUnknown` (QueryPlan evidence status) once a real self-index run is available to reproduce it deterministically.

References: PR #67 (HSE-71), QA round-2 verdict, senior-developer round-2 verdict, docs/self-host-architecture.md, architecture/cidx-self-host-policy.json, architecture/cidx-module-manifest.json.
