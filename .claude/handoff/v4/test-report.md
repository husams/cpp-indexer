Scope: graph-symbol-ids (Stories 1–6)
Test plan: unit | integration | parametrised/boundary | regression

Commands run:
  LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo test --lib
  LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo test --tests --features test-mock
  LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo test --test qa_symbol_id_boundary

Results: 391 lib / all integration+test targets pass; 0 failed; new QA test target: 9 passed

Defects: []

observations:
  - developer-verification log claims `cargo test --tests` runs 26 targets; actual count at time of QA run is consistent with 0 failures across all targets (no new failures vs baseline)
  - `tests/integration/qa_boundary.rs` already exists covering Issue 0001 / non-symbol-id scenarios; QA new file is `tests/qa_symbol_id_boundary.rs` to avoid polluting the earlier target
  - `id_resolver_unknown_repo_returns_error` passes but the error message wording may vary across versions; the assertion is intentionally permissive (non-empty message) per S5-SC-03

Verified scenarios:
  S1-SC-01, S1-SC-02: SQLite get-or-insert idempotency (developer sc01..sc04 + QA idempotency_and_write_through_across_cache_sizes, path_idempotency_across_cache_sizes)
  S2-SC-03, S2-SC-04, S2-SC-05: LRU hit/miss/eviction + size-0 path (developer sc06..sc09 + QA eviction_write_through_mutation_guard)
  S7-SC-12: per-repo ID stability across simulated re-index (developer integration reindex_symbol_id_stability + QA reindex_stability_multi_run)
  S7-SC-13, S7-SC-14, S5-SC-04: both-sink ID round-trip (developer symbol_id_roundtrip_via_id_resolver + QA both_direction_read_only_handle_round_trip)
  S5-SC-03: explicit error on missing id (developer id_resolver_missing_id_returns_explicit_error + QA id_resolver_unknown_*_returns_error x3)
  S1-SC-03 (D1): cross-repo independence (QA per_repo_id_independence)
  S6-SC-03: no USR strings on v6 graph output (developer integer_ids_populated_when_allocator_present + symbol_id_size.rs suite)

Note on cross-repo EXTERNAL_REF USR-space resolution (scenarios.md "cross-repo EXTERNAL_REF still resolving in USR-space"):
  Verified via existing `tests/cross_repo.rs` (8 tests pass with --features test-mock) confirming `materialise_external_refs` resolves across repos by USR string matching unchanged. No integer-ID comparison across repos occurs. CHARTER D1 invariant confirmed structurally by `per_repo_id_independence` test.

Regression gate (S7-SC-15):
  Pre-feature baseline (implementation-notes.md): 0 failing tests
  Post-feature: 0 failing tests
  Gate: post ⊆ pre → PASS

Additions made: property-based / parametrised (category 2)
  New file: tests/qa_symbol_id_boundary.rs — 9 parametrised/boundary tests covering:
    cache_size sweep {0,1,2,100} for USR and path idempotency + write-through (mutation guard),
    eviction write-through mutation guard (10-entry sweep on cache_size=1),
    per-repo ID independence (S1-SC-03 / D1),
    multi-run re-index stability,
    IdResolver explicit-error paths (unknown repo, unknown symbol_id, unknown file_id),
    ReadOnlyHandle bidirectional round-trip at unit level.
  Registered as [[test]] name = "qa_symbol_id_boundary" in Cargo.toml.

References: scenarios.md, implementation-notes.md, plan.md, CHARTER.md
