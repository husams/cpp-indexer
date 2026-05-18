---
run_id: cpp-indexer-m8-v2
story: S40, S41, S42, S43, S44, S45, S46
author: qa-engineer
date: 2026-05-18
---

# M8 Test Report — Structured Node Attributes (S40–S46)

## Scope

M8 milestone: schema version bump, NodeRecord/EdgeRecord structured fields, Arrow round-trip,
callable extraction, template extraction, USES access classifier, Neo4j and IndraDB sinks,
covering indexes, schema docs and drift detection.

## Test plan

unit | integration | regression | boundary (QA-added)

## Commands run

```bash
# Full unit + integration suite (no live infra)
cargo test --all-targets
# → 321 passed; 0 failed; 1 ignored (lib)
# → access_classifier_boundary: 2 passed; 2 FAILED (QD-3, new QA tests)

# Per-story tests
cargo test --test arrow_roundtrip                          # 26 passed
cargo test --test callable_extraction                      # 7 passed
cargo test --test template_extraction                      # 6 passed
cargo test --test access_classifier                        # 10 passed
cargo test --test schema_version_bump --features test-mock # 4 passed
cargo test --test schema_drift                             # 3 passed; 1 ignored
cargo test --lib sink::neo4j                               # 31 passed (S44 unit)
cargo test --lib sink::indradb                             # 28 passed (S45 unit)

# Cross-repo mirror (mock-backed)
cargo test --test cross_repo_access_mirror --features test-mock -- --ignored --nocapture
# → 1 passed

# Live Neo4j (CPP_INDEXER_LIVE_NEO4J=1, bolt://192.168.1.200:7687, password from kubernetes)
CPP_INDEXER_LIVE_NEO4J=1 NEO4J_URI=bolt://192.168.1.200:7687 NEO4J_PASSWORD=<from-k8s> \
  cargo test --test neo4j_indexes -- --ignored --nocapture
# → 4 passed (EXPLAIN test passes via HTTP attempt but gracefully skips — see QD-2)

NEO4J_URI=bolt://192.168.1.200:7687 NEO4J_PASSWORD_ENV=MY_PW MY_PW=<from-k8s> \
  cargo test --test sink_neo4j -- --ignored --nocapture
# → 3 passed (AC-S44-5 regression)

CPP_INDEXER_LIVE_NEO4J=1 NEO4J_URI=bolt://192.168.1.200:7687 NEO4J_PASSWORD=<from-k8s> \
  cargo test --test schema_drift -- --ignored --nocapture
# → 1 FAILED (QD-1): template_params and template_args absent from live DB property registry
```

## Results

| Story | Test command | Result | Scenario IDs |
|-------|-------------|--------|--------------|
| S40 | `cargo test --test schema_version_bump --features test-mock` | 4 passed | AC-S40-1, AC-S40-5 |
| S40 | `cargo test --test arrow_roundtrip` | 26 passed | AC-S40-4 |
| S40 | `cargo test --lib schema::` | 34 passed (in lib 321) | AC-S40-2, AC-S40-3 |
| S40 | `arrow_roundtrip::promoted_fields_absent_from_attrs_json` | passed | AC-S40-6 |
| S41 | `cargo test --test callable_extraction` | 7 passed | AC-S41-1 through AC-S41-6, AC-S41-8 |
| S41 | AC-S41-7 (leveldb live) | not run (no indexed leveldb v5 corpus) | AC-S41-7 |
| S42 | `cargo test --test template_extraction` | 6 passed | AC-S42-1, AC-S42-2, AC-S42-4 |
| S42 | `specialization_attrs_json_does_not_contain_template_args_key` | passed | AC-S42-2 + AC-S40-6 |
| S42 | AC-S42-3 (leveldb live) | not run | AC-S42-3 |
| S43 | `cargo test --test access_classifier` | 10 passed | AC-S43-1, AC-S43-2, AC-S43-3, AC-S43-4, AC-S43-6 (read/write/call_arg/unknown) |
| S43 | `cargo test --test access_classifier_boundary` | 2 passed; **2 FAILED** | AC-S43-6 (addr_of/return/decl_ref) → QD-3 |
| S43 | `cross_repo_access_mirror -- --ignored` | 1 passed | ADR-13 EXTERNAL_REF mirror |
| S44 | `cargo test --lib sink::neo4j` | 31 passed | AC-S44-1, AC-S44-2 (unit) |
| S44 | `neo4j_indexes -- --ignored` | 4 passed | AC-S44-3 (indexes exist, idempotent), AC-S44-1+2 (write+read) |
| S44 | EXPLAIN plan assertion | graceful skip (HTTP 7474 closed) | AC-S44-4 → QD-2 |
| S44 | `sink_neo4j -- --ignored` | 3 passed | AC-S44-5 (regression) |
| S45 | `cargo test --lib sink::indradb` | 28 passed | AC-S45-1 (unit), AC-S45-5 (unit) |
| S45 | `indradb_properties -- --ignored` | not run (CPP_INDEXER_LIVE_INDRADB=1 not configured) | AC-S45-1, AC-S45-2, AC-S45-3, AC-S45-4, AC-S45-5 |
| S45 | `sink_parity -- --ignored` | not run | AC-S45-1/@AC-S44-1, AC-S45-2/@AC-S44-2 |
| S46 | `cargo test --test schema_drift` | 3 passed; 1 ignored | AC-S46-5 (non-live checks pass) |
| S46 | `schema_drift_live_neo4j -- --ignored` | **FAILED** (exit 101) | AC-S46-5 → QD-1 |
| S46 | `docs/schema/SCHEMA.md` present, correct fields | manual pass | AC-S46-1 |
| S46 | `grep -q "re-index" docs/runbooks/staging-recovery.md` | pass | AC-S46-2 |
| S46 | `grep -q "source_association_type" prompt/graph_database/cpp/schema.txt` | pass | AC-S46-4 |
| S46 | `grep -q "return_type" prompt/graph_database/cpp/schema.txt` | pass | AC-S46-4 |

## Defects

- defect-id: QD-1
  scenario-id: AC-S46-5
  failing-command: "CPP_INDEXER_LIVE_NEO4J=1 NEO4J_URI=bolt://192.168.1.200:7687 NEO4J_PASSWORD=<redacted> cargo test --test schema_drift -- --ignored --nocapture"
  exit-code: 101
  description: >
    `schema_drift_live_neo4j` fails because `template_params` and `template_args` are absent
    from the live Neo4j property-key registry. The other 10 promoted fields are present.
    Root cause: no TEMPLATE_DECL-containing repo has been indexed against the dev Neo4j with
    the v5 schema. The test correctly enforces AC-S46-5 — drift is detectable when fields
    are missing. Resolution requires either: (a) running a fresh v5 index of a template-rich
    corpus (e.g. leveldb) against bolt://192.168.1.200:7687, OR (b) devops running the
    re-index recipe from docs/runbooks/staging-recovery.md §6 on the dev cluster.
  status: resolved
  resolution: >
    Operator re-index executed 2026-05-18. Steps: (1) cargo build --release (binaries rebuilt
    from M8 HEAD at 19:40 UTC); (2) old v4 graph wiped via Cypher DETACH DELETE; (3) cxg-daemon
    started with DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib and config
    pointing at bolt://192.168.1.200:7687; (4) leveldb corpus indexed (TEMPLATE_DECL nodes
    populated template_params — 6 nodes, schema bumped to cxg-schema-v5); (5) boost_optional
    fixture indexed to populate template_args (SPECIALIZATION node with template_args JSON);
    (6) schema_drift_live_neo4j test: exit 0, 1 passed. All 12 promoted fields now present
    in dev Neo4j property-key registry. Log: .claude/handoff/v2/logs/operator-reindex.md

- defect-id: QD-3
  scenario-id: AC-S43-6
  failing-command: "cargo test --test access_classifier_boundary -- --nocapture"
  exit-code: 101
  description: >
    Two QA-added boundary tests fail:
    - `return_operand_produces_return_classification`: classifier returns `unknown` for
      `return global_val;` instead of `return`. The ReturnStmt parent is not matched in the
      4-parent walk, likely because an ImplicitCastExpr wraps the DeclRefExpr and consumes
      one parent slot before ReturnStmt is reached.
    - `decl_initializer_produces_decl_ref_classification`: classifier returns `unknown` for
      `int x = global_val;` instead of `decl_ref`. Same cause — VarDecl is not in
      the 4-parent window.
    These 3 variants (addr_of, return, decl_ref) are in the closed AC-S43-1 enumeration
    but `return` and `decl_ref` have no end-to-end fixture coverage demonstrating they
    fire correctly. `addr_of` (addr_of_operator_produces_addr_of_classification) passes.
    The 4-parent walk depth in `classify_use` may need to be extended to 5–6 for the
    ImplicitCastExpr wrapping pattern, or the ReturnStmt/VarDecl cases need to be matched
    at any depth rather than only in the first 4 slots.
    Failing test output: [Some("unknown"), Some("unknown"), Some("write"), Some("unknown"),
    Some("addr_of"), Some("unknown"), Some("unknown")] — `return` and `decl_ref` absent.
  status: resolved
  resolution: >
    Root cause confirmed via debug instrumentation: clang-rs exposes ImplicitCastExpr as
    UnexposedExpr, and UnexposedExpr.get_lexical_parent() returns None (cannot traverse to
    ReturnStmt or VarDecl through the expression parent chain).  Fix: added context-capture
    at visit time in Collector — when an UnexposedExpr is visited with parent=ReturnStmt,
    its spelling location is recorded in `return_wrappers`; VarDecl/FieldDecl/ParmDecl
    parent records in `decl_init_wrappers`.  emit_uses_edge looks up the visitor-supplied
    parent location in these sets before calling classify_use, short-circuiting to
    AccessKind::Return or AccessKind::DeclRef.  Fix in src/visit/shallow.rs.
    All 4 boundary tests pass; 10 primary access_classifier tests still pass; clippy clean.
    Fix PR: developer-s43-qd3-fix (2026-05-18).

## Observations (advisory — do not block dispatch)

- AC-S41-7, AC-S42-3, AC-S43-5: leveldb live Cypher queries not run; no indexed v5 leveldb
  corpus exists on dev Neo4j. These are `#[ignore]` gated tests requiring a full corpus
  index; same pre-condition as QD-1.
- IndraDB live tests (AC-S45-1/2/3/4/5, sink_parity) not run: `CPP_INDEXER_LIVE_INDRADB=1`
  is not configured for this QA pass. 12 existing IndraDB unit tests pass via lib (AC-S45-4
  regression covered in unit scope).
- AC-S46-3 (wiki page update) deferred to doc-writer per developer-s46-docs.md follow-up.
- `is_volatile_method()` absent in clang-rs 2.0.0: volatile suffix omitted from signature;
  M9 follow-up per developer-s41 deviation log.
- `schema_drift.rs` path: top-level `tests/schema_drift.rs` rather than
  `tests/integration/schema_drift.rs` (deviation from plan.md). Top-level path is valid and
  works correctly; plan.md should be corrected.
- `pipeline::parallel::tests::clang_err_return_is_counted_as_error` showed one transient
  failure in a full `--all-targets` run; passes when run in isolation. Pre-existing flaky test
  (simulates thread panics in parallel pipeline); not an M8 regression.

## Additions made

Category: **mutation/boundary** — new test file `tests/visit/access_classifier_boundary.rs`
(4 tests) + fixture `tests/fixtures/access_classifier/access_extended.cpp` covering the
3 access modes (addr_of, return, decl_ref) not exercised by the primary `access_classifier.rs`
suite. These tests are end-to-end fixture-based (real libclang parse → Arrow Parquet →
EdgeRecord) matching the category-3 requirement. The tests found QD-3 (return and decl_ref
classifier miss).

## References

- scenarios.md: /Users/husam/workspace/cpp-indexer/.claude/handoff/v2/scenarios.md
- implementation-notes.md: /Users/husam/workspace/cpp-indexer/.claude/handoff/v2/implementation-notes.md
- developer logs: /Users/husam/workspace/cpp-indexer/.claude/handoff/v2/logs/developer-s4{0,1,2,3,4,5,6}-*.md
- Cognee tags: task:cpp-indexer-m8, role:qa-engineer
