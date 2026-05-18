## developer log — S43 USES edge access classifier

### Skills loaded
- `rust-conventions` (loaded at session start; Cargo.toml present)

### Skills considered but not loaded
- `cpp-conventions`: project is Rust, not C++
- `implement-story`: not available; followed CHARTER dispatch contract directly
- `google-agents-cli-workflow`: no ADK agents involved

### Commands run

```
# Orientation
grep -n "emit_uses_edge|src_usr|module_usr" src/visit/shallow.rs
# → Located emit_uses_edge at line 556; confirmed src_usr fell back to module_usr for expression parents

# Fix: semantic-parent walk for src_usr
# Edited src/visit/shallow.rs emit_uses_edge to walk get_semantic_parent() up to find
# entity with non-empty USR + entity_kind_to_node_kind match

cargo test --test access_classifier
# → 10/10 pass after src_usr fix and new assertion

# Fix stale struct literals in other tests
cargo clippy --all-targets --all-features -- -D warnings
# → E0063: missing fields in m4_exit_gate.rs, cross_repo.rs
# → Fixed NodeRecord + EdgeRecord struct literals (source_association_type, target_association_type, M8 node fields)
# → Fixed stale column count assertions in stage/writer.rs (13→23, 9→11)
# → Fixed stale schema version assertion in macros.rs (4→5)

cargo fmt --all
# → macros.rs reformatted (long assert_eq split across lines)

cargo fmt --all -- --check   → OK
cargo clippy --all-targets --all-features -- -D warnings  → OK
cargo test --all-targets     → 302 passed; 0 failed; 16 ignored
```

### Deviations from plan.md

1. USES emission scope expansion — Phase 1 had no USES edges; added DeclRefExpr/MemberRefExpr/TypeRef interception.
2. MemberRefExpr parent → read classification — pragmatic workaround for UnexposedExpr wrapping; not in ADR-13 decision table.
3. src_usr semantic-parent walk — initial implementation had TU fallback for all expression parents; fixed by walking get_semantic_parent() up to tracked declaration.
4. Log level: debug not warn (ADR-13 binding override of plan.md).
5. Stale pre-existing assertions updated (column counts, schema version number).

### Tool failures / retries

- Pass 1 clippy: E0063 in m4_exit_gate.rs + cross_repo.rs → fixed struct literals → pass 2 OK
- Pass 1 fmt --check: long line in macros.rs → cargo fmt → pass 2 OK
- Pass 1 cargo test: schema_version_bumped_to_4 + write_and_read_back_row_counts failed → fixed assertions → pass 2 all pass
