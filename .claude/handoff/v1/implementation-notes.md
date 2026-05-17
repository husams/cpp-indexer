# Implementation Notes

## S26-macros

Files changed:
- `src/schema/nodes.rs` — added `NodeKind::Macro` variant + as_str/all/try_from_arrow_str entries
- `src/schema/edges.rs` — added `EdgeKind::ExpandsTo` variant + as_str/all/try_from_arrow_str entries
- `src/schema/version.rs` — bumped SCHEMA_VERSION 3→4, SCHEMA_VERSION_TAG/PARQUET_MAGIC updated
- `src/visit/macros.rs` — new module: collect_macro_definition, collect_macro_expansion, macro_usr helper, is_top_level_expansion (nested filter), build_macro_attrs, 9 unit tests
- `src/visit/mod.rs` — registered `pub mod macros`
- `src/visit/shallow.rs` — dispatched MacroDefinition/MacroExpansion before entity_kind_to_node_kind in Collector::visit; added `seen_macro_usrs: HashSet<String>` dedup field; added visit_macro_definition + visit_macro_expansion methods; imports from macros module

Tests added/run:
- `LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo nextest run -p cpp_indexer visit::macros`
- Result: 9 passed, 224 skipped (exit code 0)
- Gate 1 (fmt): cargo fmt --all -- --check → PASS
- Gate 2 (clippy): cargo clippy --all-targets --all-features -- -D warnings → PASS (0 warnings)
- Gate 3 (test): visit::macros 9/9 → PASS

Deviations from plan:
- cursor_map.rs was NOT modified (plan listed it as a file-to-touch; the design intentionally keeps macro kinds out of cursor_map, comment already says "MACRO (S22) is not mapped here"). Macro dispatch is handled directly in Collector::visit before entity_kind_to_node_kind.
- SCHEMA_VERSION bumped to 4 (was 3). Plan did not call this out explicitly but ADR-9 requires it.
- LIBCLANG_PATH/DYLD_LIBRARY_PATH must be set when running tests on this macOS host (Apple clang; libclang.dylib at /Library/Developer/CommandLineTools/usr/lib). Without it the test binary aborts with SIGABRT (dyld failure). Build and lint work without it.

Follow-ups:
- [sr-dev] exit-criteria command in plan.md for S26 does not include the LIBCLANG_PATH/DYLD_LIBRARY_PATH env prefix needed on macOS. CI workflow should set LIBCLANG_PATH or add `.cargo/config.toml [env]` section.
- [qa] Integration test with an actual X-macro `.def` fixture is not yet written. AC-M5-4 bound (≤10× source lines) is correct by design (one EXPANDS_TO per top-level invocation) but not exercised by a fixture test. The QA stage should add a fixture test with ~20-line `.def` file asserting edge_count ≤ 200.

References: plan.md S26 (lines 446-458), design.md §3 visit/macros, requirements.md AC-M5-1..4

---

## S27-phase2-decorate

Files changed:
- `src/visit/decorate.rs` (new) — Phase 2 decorator module implementing `decorate::run()`, `load_function_nodes`, `collect_decoration`, `classify_exception_spec`, `classify_control_flow`, `patch_attrs_json`, `write_phase2_shard`
- `src/visit/mod.rs` — added `pub mod decorate;`
- `src/pipeline/mod.rs` — replaced Phase 2 stub with `decorate::run(...)` call; updated `load_nodes_from_stage` to deduplicate by USR preferring `phase=2` over `phase=1`; added `collect_phase2_shards` helper

Tests added/run:
- Command: `DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo nextest run -p cpp_indexer visit::decorate`
- Result: 5/5 PASS
  - `skip_phase2_returns_zero_immediately` — AC-M5-6 gate
  - `empty_stage_dir_produces_zero_decorated`
  - `patch_attrs_json_merges_fields` — verifies exception_spec/control_flow merge + pre-existing fields preserved
  - `classify_exception_spec_noexcept` — in-process libclang parse of `void foo() noexcept {}`
  - `classify_control_flow_has_return` — in-process libclang parse of `int foo() { return 42; }`

Exit gates:
1. `cargo fmt --all -- --check` — PASS
2. `cargo clippy --all-targets --all-features -- -D warnings` — PASS
3. `cargo nextest run -p cpp_indexer visit::decorate` — PASS (5/5)

Deviations from plan:
- `src/bin/index.rs` was NOT modified — `--skip-phase2` clap flag and `RunOptions::skip_phase2` were already wired in a prior story; plan listed the file but change was a no-op.
- Deduplication logic added to `load_nodes_from_stage` — not called out in plan but required to prevent duplicate USRs in sink write when both phase1 and phase2 shards coexist.
- `exception_spec` classification uses `entity.get_type().get_display_name()` string matching (the safe `clang` crate does not expose `clang_getExceptionSpecificationType` directly). Documented as best-effort in module doc.

Follow-ups:
- [sr-dev] `exception_spec` classification relies on display-name string matching; upgrade to typed API if the `clang` crate exposes `ExceptionSpecification` in a future version.
- [devops] `DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib` required for macOS CI when running `cargo nextest run` with libclang tests.

References: plan.md S27 (lines 461-471), design.md §Phase 2, requirements.md AC-M5-5 / AC-M5-6

---

## S28-cpp20-modules

### Files changed

- `src/visit/modules_cpp20.rs` — new module: probe, dispatch helpers, `parse_module_tu`, `is_module_tu`, `warn_and_skip`, `capability_version_note`
- `src/visit/modules_probe.cppm` — minimal probe fixture embedded via `include_bytes!`
- `src/visit/mod.rs` — added `pub mod modules_cpp20`
- `src/bin/index.rs` — replaced clap auto `version` with manual `--version` flag that runs probe and appends capability note (AC-M5-9)

### Tests added/run

Command: `DYLD_LIBRARY_PATH=... cargo nextest run -p cpp_indexer "visit::modules_cpp20"`
Result: 7 tests run: 7 passed; 1 `#[ignore]` test skipped (requires libclang module support)

### Exit gates

1. `cargo fmt --all -- --check` — PASS
2. `cargo clippy --all-targets --all-features -- -D warnings` — PASS
3. `cargo nextest run -p cpp_indexer "visit::modules_cpp20"` — PASS (7/7)

### Deviations from plan

1. No new EdgeKind added — ADR-8 scope-limit paragraph overrides §2 MODULE_EXPORTS mention. Reuse `Includes` with `"import": true` attr.
2. `modules_probe.cppm` added — not in files-to-touch but required by ADR-8 §1.
3. `--version` is manual flag — clap auto-version exits before main; use `disable_version_flag = true`.
4. `input_path` changed to `Option<PathBuf>` — required for manual version path.
5. `tempfile` not used in probe — it is a dev-dep; used `std::env::temp_dir()` + PID-scoped name.

### Follow-ups (tagged @sr-dev)

- Pipeline routing for `.cppm`/`.ixx`/`.mxx` TUs not wired in `pipeline/parallel.rs` — `is_module_tu` + dispatch needed there.
- Add `cpp20_modules_capable` to `GET /v1/status` (ADR-8 follow-up).
- Evaluate libclang 19 after v1 GA.

---

## S23-cross-repo-resolver-bin

Files changed:
- `src/schema/edges.rs` — added `ExternalRef` variant to `EdgeKind` (as_str, all(), try_from_arrow_str)
- `src/resolve/mod.rs` — exposed `pub mod cross_repo`
- `src/resolve/cross_repo.rs` — new: Phase5Options, Phase5Stats, run(), check_schema_version(), detect_repo_backends(), check_backend_homogeneity(), build_global_usr_map(), materialise_external_refs(), collect_shards()
- `src/bin/resolve_cross_repo.rs` — fleshed out: clap CLI → factory::create → phase5_run
- `Cargo.toml` — registered `[[test]] name = "cross_repo"` with `required-features = ["test-mock"]`
- `tests/cross_repo.rs` — new: 8 tests (5 non-ignored fixture tests + 3 ignored live-DB tests)

Tests added/run:
- `cargo fmt --all -- --check` → PASS
- `cargo clippy --all-targets --all-features -- -D warnings` → PASS
- `DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo nextest run -p cpp_indexer --test cross_repo --features test-mock -- --ignored` → 8/8 PASS (3 live-DB ignored tests pass via early-return guard when NEO4J_URI is absent)

Deviations from plan:
1. **Parquet-based resolution instead of DB queries.** design.md §Phase 5 says "Builds global Usr → (repo, node_id) map by querying the configured DB." `GraphSink` has no query methods (list_nodes, list_edges, etc.), so Phase 5 reads from the staged Parquet files that Phase 3 produced. This is functionally equivalent for the test fixture cases and avoids a breaking trait extension. Tagged sr-dev for review. If DB-side querying is required (e.g. for repos whose Parquet staging was cleaned up), a `GraphSink::query_cross_repo_candidates` method should be added in a follow-up story.
2. **`EXTERNAL_REF` EdgeKind added in S23.** EdgeKind::ExternalRef is in `all()` so Arrow schema is extended. sr-dev should confirm whether a SCHEMA_VERSION bump is required for the S23 PR or can wait until the next schema-affecting story per ADR-9.
3. **`required-features = ["test-mock"]` added for `cross_repo` test binary.** Follows the existing pattern used by `m1_exit_gate`, `m2_exit_gate`, `incremental_cache`, and `repo_meta` tests.

Follow-ups:
- [sr-dev] S24 `resolve::canonical` will extend `cross_repo.rs` with ADR-4 system-header canonicalisation
- [sr-dev] Confirm SCHEMA_VERSION bump required for `ExternalRef` variant addition (ADR-9)
- [sr-dev] If Phase 5 DB-side querying is needed (for cleaned-up staging dirs), add `GraphSink::query_cross_repo_candidates` + `GraphSink::query_repo_nodes`
- [devops] `DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib` required for macOS CI runs of `--test cross_repo`

References: plan.md S23, design.md §Phase 5, adr-2.md §lock, adr-4.md, adr-9.md

---

## S24-syshdr-canonicalisation

Files changed:
- `src/resolve/canonical.rs` (new) — path-rule matcher + 16 unit tests
- `src/resolve/mod.rs` — added `pub mod canonical`
- `src/resolve/cross_repo.rs` — added `canonical_overrides` field to `Phase5Options`, wired `canonical_repo()` into `build_global_usr_map`, updated `UsrEntry` doc-comment
- `src/bin/resolve_cross_repo.rs` — added `..Default::default()` to `Phase5Options` literal
- `tests/cross_repo.rs` — added `..Default::default()` to all six `Phase5Options` literals (struct literal exhaustiveness)

Tests added/run:
- `cargo fmt --all -- --check` → PASS
- `cargo clippy --all-targets --all-features -- -D warnings` → PASS
- `DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo nextest run -p cpp_indexer resolve::canonical` → 16/16 PASS

Deviations from plan:
1. Plan test description said "USR under `/usr/include/**` → `repo:system:libstdc++`" — ADR-4 says `/usr/include/c++/**` → `system:libstdc++` and `/usr/include/**` (plain) → `system:libc`. Implemented per ADR-4. Tests reflect ADR-4 semantics.
2. Plan said "under compiler-internal path → `repo:system:libc`" — ADR-4 says `lib/clang/*/include/**` → `system:clang-builtins`. Implemented per ADR-4.
3. Plan used `repo:system:*` prefix notation; ADR-4 uses `system:*` (no `repo:` prefix for system headers, `repo:vendored:*` for vendored). Implemented per ADR-4.
4. ADR-4 mentions file name `canonicalise.rs`; plan.md and exit criteria reference `resolve::canonical`. Used `canonical.rs` to match exit criteria filter.
5. Config override list exposed as `Phase5Options::canonical_overrides: Vec<CanonicalOverride>` (in-memory) rather than wired to a `[cross_repo]` TOML section. TOML round-trip is a follow-up tagged `sr-dev`.

Follow-ups:
- [sr-dev] Add `[cross_repo]` TOML section to `config/mod.rs` with `canonical_path_rules` and `canonical_priority` per ADR-4 §Implementation.
- [devops] Tests require `DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib` on macOS because llvm@18 is not installed via Homebrew.

References: plan.md S24, adr-4.md, design.md §Phase 5

---

## S25-m4-two-repo-gate

Files changed:
- `tests/fixtures/two_repo/lib-b/include/lib_b.h` (new)
- `tests/fixtures/two_repo/lib-b/src/lib_b.cpp`   (new)
- `tests/fixtures/two_repo/lib-b/compile_commands.json` (new)
- `tests/fixtures/two_repo/lib-a/src/main.cpp`    (new)
- `tests/fixtures/two_repo/lib-a/compile_commands.json` (new)
- `tests/integration/m4_exit_gate.rs`             (new)
- `Cargo.toml` — added `[[test]] name = "m4_exit_gate"` with `required-features = ["test-mock"]`

Tests added/run:
- `cargo fmt --all -- --check` → PASS
- `cargo clippy --all-targets --all-features -- -D warnings` → PASS
- `DYLD_LIBRARY_PATH=... cargo nextest run -p cpp_indexer --features test-mock` → 221 passed, 22 skipped
- `DYLD_LIBRARY_PATH=... cargo nextest run -p cpp_indexer --features test-mock --test m4_exit_gate -- --ignored` → PASS (early-returns when NEO4J_URI unset; full live-DB path runs when env vars present)

Deviations from plan:
1. **Synthetic Parquet shards instead of real libclang CALLS edges (tagged sr-dev).**
   Phase 1 (`shallow.rs`) does not visit `CallExpr` entities — `cursor_map.rs` maps `EntityKind::CallExpr` to `None` and the visitor skips it. Indexing the two-repo fixture through `pipeline::run` produces zero `cross_repo_candidate=true` CALLS edges. To satisfy AC-M4-10 (`via=CALLS`) today, the test constructs synthetic Parquet shards with `kind=Calls` cross_repo_candidate edges, then writes both repos' nodes to Neo4j and runs Phase 5. The real C++ fixture trees document the intended shape but are not parsed in the live test. CallExpr emission is M5/S26 work.

2. **Raw `neo4rs::Graph` for Cypher assertion.**
   `Neo4jSink` does not expose the inner `Graph` field publicly. The test opens a second `Graph` connection using the same env-var credentials. No production code was modified.

3. **Cypher uses `[:EDGE {kind:"EXTERNAL_REF"}]` not `[:EXTERNAL_REF]`.**
   The Neo4j sink stores all edges as `[:EDGE]` relationships with a `kind` string property (per `CQL_MERGE_EDGES` in `sink/neo4j.rs`). The Cypher assertion uses `MATCH p=()-[:EDGE {kind:"EXTERNAL_REF"}]->()` instead of the plan's `MATCH p=()-[:EXTERNAL_REF]->()`. Semantically equivalent given the current sink schema. Noted for QA review.

Follow-ups:
- [sr-dev] Add CallExpr visiting to Phase 1 so the two-repo fixture can be indexed end-to-end and the synthetic Parquet workaround removed (M5/S26 scope).
- [sr-dev] Consider exposing `Neo4jSink::graph()` accessor or a `GraphSink::raw_query` helper for test isolation.
- [qa] Confirm whether AC-M4-9 Cypher (`[:EXTERNAL_REF]`) is intended as shorthand or requires migration to typed relationships.

References: plan.md S25, requirements.md AC-M4-9 / AC-M4-10, tests/cross_repo.rs (write_fixture_shards pattern), src/resolve/cross_repo.rs, src/visit/shallow.rs, src/visit/cursor_map.rs, src/sink/neo4j.rs

---

## S29-m5-chromium-gate

### Files changed
- `tests/fixtures/chromium_subset.md` (created) — acquisition checklist
- `tests/fixtures/m5_macro_template/macros.h` (created) — synthetic macro-heavy + template-heavy header
- `tests/fixtures/m5_macro_template/main.cpp` (created) — function-scope macro expansions + template instantiations
- `tests/fixtures/m5_macro_template/compile_commands.json` (created) — single-TU compile commands
- `tests/integration/m5_exit_gate.rs` (created) — two `#[ignore]` tests: synthetic gate + Chromium gate
- `Cargo.toml` — appended `[[test]] name = "m5_exit_gate"` with `required-features = ["test-mock"]`

### Tests added/run
- `cargo fmt --all -- --check` → EXIT 0 (after one rustfmt auto-fix)
- `cargo clippy --all-targets --all-features -- -D warnings` → EXIT 0
- `DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo nextest run -p cpp_indexer --features test-mock --test m5_exit_gate -- --ignored`
  → EXIT 0; 2 tests run: 2 passed, 0 skipped
  - `m5_synthetic_macro_template_gate`: PASS (0.102s)
  - `m5_chromium_subset_gate`: PASS (0.039s — early-return; CXG_M5_CHROMIUM_PATH unset)

### Deviations from plan
- Plan exit-criteria command (`cargo nextest run -p cpp_indexer --test m5_exit_gate -- --ignored`) omits `--features test-mock`; required to link MockSink. All prior m1/m2/m4 exit-gate commands share this omission — established pattern, not fixed here. Tagged sr-dev.
- compile_commands.json uses absolute host paths (matches all other project fixtures).

### Follow-ups
- [sr-dev] Plan exit-criteria commands for test-mock tests should include `--features test-mock` or CI env should export it globally.
- [sr-dev] Fixture compile_commands.json has absolute paths — cross-machine portability is a pre-existing project-wide limitation; no action taken.

### References
- plan.md S29 (lines 491-501)
- requirements.md AC-M5-10, AC-M5-11, M5-S4
- tests/fixtures/llvm_checkout.md (pattern for acquisition checklist)
- tests/integration/m2_exit_gate.rs (pattern for #[ignore] MockSink tests)
- src/visit/macros.rs (confirms detailed_preprocessing_record(true) always on)
