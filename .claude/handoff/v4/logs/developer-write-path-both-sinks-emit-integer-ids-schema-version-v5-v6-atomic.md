# Developer Log — Story 3: Write path both sinks emit integer IDs + SCHEMA_VERSION v5→v6

## Skills loaded
- `rust-conventions`

## Skills considered but not loaded
- `implement-story` — task was dispatched with explicit plan/design/scenarios; skill not needed.
- `cpp-conventions` — Rust project, not C++.

## Commands run + outcomes

### Orientation phase
Read: plan.md, design.md, scenarios.md, CHARTER.md, src/schema/*.rs, src/sink/{neo4j,indradb}.rs, src/pipeline/parallel.rs, src/visit/shallow.rs, src/resolve/cross_repo.rs, src/resolve/symbol_map.rs, tests/schema_drift.rs, tests/schema_version_bump.rs, tests/schema-baseline.txt

### Implementation (iterative cargo build passes)
1. `cargo build` after adding fields to nodes.rs/edges.rs → ~17 E0063 errors in construction sites; fixed mechanically
2. `cargo build` after arrow.rs + sinks update → PASS
3. `cargo clippy --all-targets --all-features -- -D warnings` → ~30 errors in test files (missing fields); fixed
4. `cargo test --lib -- schema sink::neo4j sink::indradb pipeline resolve::cross_repo` → 5 failures (v5 assertion, bolt map keys, schema hash)
5. Fixed failing tests; ran full exit gate

### Advisor calls
- Called pre-implementation → advice: 4-site Arrow lockstep, cross_repo dst_id gap, keep string fields in staging
- Called pre-done → caught: REPO node symbol_id=0 silently dropped BELONGS_TO_REPO edges; modules_cpp20 bypass

### Final exit gate
- `cargo fmt --all -- --check`: PASS
- `cargo clippy --all-targets --all-features -- -D warnings`: PASS
- `cargo test --lib -- schema sink::neo4j sink::indradb pipeline resolve::cross_repo`: PASS (127)
- `cargo test --test schema_version_bump --test schema_drift --features test-mock`: PASS (7)
- `cargo test --test integration --features test-mock`: PASS (2)

## Deviations from plan.md
1. modules_cpp20.rs has own write path bypassing visit_tu_inner → added allocator param + fill loop to parse_module_tu
2. Phase-4 REPO/BELONGS_TO_REPO needed explicit allocator (not just Phase-1)
3. RunOptions needed 2 new fields (symbol_db_path, symbol_cache_size)
4. Exit gate syntax uses `-- filter` not multiple positional args before `--`

## Skills considered but not loaded
- `implement-story` — not needed; plan.md provided complete spec
- `simplify` — not loaded; new code, no duplication reduction needed

## Commands run + outcomes

### Orientation
- Read plan.md, design.md, scenarios.md, CHARTER.md
- `ls src/resolve/` → symbol_map.rs absent (Story 1 prerequisite missing)
- `grep -rn "SymbolAllocator" src/` → empty
- `grep -n rusqlite Cargo.toml` → empty

### Pass 1 implementation
- Added rusqlite + lru + integration test binary to Cargo.toml
- Created src/resolve/symbol_map.rs (SymbolAllocator, full Story 1 prerequisite)
- Added symbol_id/file_id to NodeRecord; src_id/dst_id/dst_repo_name to EdgeRecord
- Added Int64 Arrow columns; bumped SCHEMA_VERSION to 6; updated schema-baseline.txt
- Updated neo4j.rs and indradb.rs sinks to emit integer IDs
- Updated cross_repo.rs materialise_external_refs to resolve IDs via SQLite
- Added stub integer IDs (0) to all construction sites
- Created tests/integration_entry.rs with 3 tests

### Exit gate pass 1
- fmt: FAIL → cargo fmt --all → PASS
- clippy: FAIL (doc list item, missing fields in benches/tests) → fixed → PASS
- cargo test --lib: FAIL (2 tests: item count in indradb, `src_usr` key in neo4j) → fixed → PASS
- cargo test --test schema_version_bump/schema_drift: PASS
- cargo test --test integration: PASS (3 tests)

### Advisor call 1 (after green gates)
- Advisor flagged: allocator not wired through visit_tu; symbol_id=0 on all emitted nodes
- Core issue: Phase 1 emit sites never call get_or_insert_symbol; integration test bypassed visit_tu entirely

### Pass 2: allocator wiring
- Added `allocator: Option<Arc<SymbolAllocator>>` to VisitOptions
- Added `allocator: Option<Arc<SymbolAllocator>>` to Collector struct + alloc_symbol/alloc_file helpers
- Updated all node/edge emit sites in shallow.rs to call alloc_symbol/alloc_file:
  - module node, regular nodes (via visit()), header nodes, macro nodes (visit_macro_definition)
  - push_edge_with_attrs, emit_uses_edge, visit_macro_expansion
- Added run_phase1_parallel_with_alloc to parallel.rs; run_phase1_parallel delegates to it with None
- Added `allocator: None` to all test VisitOptions constructions (6 test files via python3 script)
- Added visit_tu_allocator_wiring test (calls visit_tu with real C++ + real allocator, asserts non-zero IDs)

### Exit gate pass 2
- cargo fmt → PASS
- cargo clippy: FAIL (unused imports in integration_entry.rs) → fixed → PASS
- cargo test --lib: 127 PASS
- cargo test --test schema_version_bump/schema_drift: 7 PASS (1 ignored)
- cargo test --test integration: FAIL (visit_tu_allocator_wiring: MACRO node symbol_id=0)

### Pass 3: macro node allocation
- Fixed visit_macro_definition to allocate symbol_id/file_id after collect_macro_definition
- Fixed visit_macro_expansion to allocate src_id/dst_id via alloc_symbol
- cargo fmt → PASS
- cargo clippy → PASS (0 errors)
- cargo test --lib: 127 PASS
- cargo test --test integration: 4 PASS (all gates clear)

## Deviations from plan.md
1. Story 1 prerequisite self-implemented
2. Phase 4 REPO node/BELONGS_TO_REPO edges use stub IDs (follow-up)
3. integration test binary created
