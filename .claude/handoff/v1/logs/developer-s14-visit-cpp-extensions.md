## Developer log — S14-visit-cpp-extensions

### Skills loaded
- rust-conventions

### Skills considered but not loaded
- cpp-conventions: not applicable (project is Rust, not C++)
- implement-story: would have been appropriate but rust-conventions + direct plan reading sufficient
- simplify: no refactoring scope in this story

### Commands run + outcomes

1. `cargo check --all-targets --all-features` → first attempt: error `no method named is_inline_namespace` (feature gate clang_9_0 not enabled). Fixed by removing the call and documenting as follow-up.
2. `cargo check --all-targets --all-features` → clean
3. `cargo fmt --all` → formatted 2 files (shallow.rs and phase1_base.rs)
4. `cargo fmt --all -- --check` → exit 0
5. `cargo clippy --all-targets --all-features -- -D warnings` → exit 0
6. `cargo nextest run -p cpp_indexer --test phase1_base` (first run, with DYLD_LIBRARY_PATH) → 13/15 failed: m2_emits_header_nodes and m2_emits_includes_edges
   - Root cause: InclusionDirective entities require `detailed_preprocessing_record(true)` on the TU parser
7. Added `detailed_preprocessing_record(true)` to visitor parser call
8. `cargo nextest run -p cpp_indexer --test phase1_base` → 15/15 passed

### ADR-9 bump resolution
- The existing inline comment in nodes.rs/edges.rs said additive M2 variants do NOT require a bump ("without bumping SCHEMA_VERSION for additive variants"). ADR-9 (accepted) says the opposite: "Any change to NodeKind or EdgeKind variants requires bumping SCHEMA_VERSION by 1." ADR-9 wins. Bumped 1→2. Outdated comment replaced.

### Deviations from plan.md
- SCHEMA_VERSION bumped (plan dispatch said "coordinate with S31" — noted in follow-ups)
- ADL_CANDIDATE (AC-M2-12) and INSTANTIATES (AC-M2-9) edge emission: schema variants added but visitor emission deferred (requires call-expression context not available in declaration walk)
- is_inline_namespace() omitted: clang_9_0 feature not in Cargo.toml

### Worktree
- Branch: story/s14-visit-cpp-extensions
- Commit: 567cfc7 "S14: cpp extensions"
