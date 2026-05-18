## developer log — S43 QD-3 fix (ImplicitCastExpr wrapping in access classifier)

### Skills loaded
- `rust-conventions` (loaded; Cargo.toml present)

### Skills considered but not loaded
- `cpp-conventions`: project is Rust, not C++
- `implement-story`: QD-3 retry, not a new story dispatch

### Root-cause analysis

Debug instrumentation added to `emit_uses_edge` revealed:

1. **Visitor parent for `DeclRefExpr` inside `return global_val;`** is `UnexposedExpr`
   (clang-rs exposes `ImplicitCastExpr` as `UnexposedExpr`).
2. **`UnexposedExpr.get_lexical_parent()`** returns `None` — cannot traverse upward
   through statement nodes to reach `ReturnStmt` or `VarDecl`.
3. **`UnexposedExpr.get_semantic_parent()`** returns `FunctionDecl` for the `return`
   case (ambiguous), and `VarDecl` for the `int x = global_val;` case (usable but only
   for the VarDecl path).
4. The `addr_of` case works because `&global_val` produces a parent of `UnaryOperator`
   (not `UnexposedExpr`), which is matched directly.

### Fix approach (approved by advisor)

Context-capture at visit time in `Collector` (pre-order DFS guarantees the
`UnexposedExpr` wrapper is visited before its `DeclRefExpr` child):

- Added `return_wrappers: HashSet<(u32, u32)>` and `decl_init_wrappers: HashSet<(u32, u32)>` 
  fields to `Collector`.
- In `visit()`, when `entity.get_kind() == UnexposedExpr`, record the entity's spelling
  location in the appropriate set based on `parent.get_kind()`:
  - `ReturnStmt` → `return_wrappers`
  - `VarDecl | FieldDecl | ParmDecl` → `decl_init_wrappers`
- In `emit_uses_edge`, when `parent.get_kind() == UnexposedExpr`, look up
  `parent.get_location()` in the captured sets before calling `classify_use`.
  Short-circuit to `AccessKind::Return` or `AccessKind::DeclRef` on hit.

This is additive: only fires for `UnexposedExpr` parents with a recorded context;
all other parent kinds fall through to the existing `classify_use` logic unchanged.

### Commands run

```
# Debug instrumentation (temporary, removed before final commit)
cargo test --test access_classifier_boundary -- --nocapture
# → revealed: parent_chain = ["UnexposedExpr"] for return/decl_ref cases
# → UnexposedExpr.sem_parent: VarDecl for decl_ref, FunctionDecl for return
# → get_lexical_parent() = None for all UnexposedExpr nodes

# After fix implementation:
cargo build --all-targets
# → Compiling cpp_indexer v0.1.0 — Finished

cargo test --test access_classifier_boundary -- --nocapture
# → 4 passed; 0 failed (return_operand, decl_initializer, addr_of, no_out_of_band all ok)

cargo test --test access_classifier
# → 10 passed; 0 failed (no regressions)

cargo clippy --all-targets -- -D warnings
# → Finished (clean)

cargo fmt --all -- --check
# → exit 1 (multiline match arm formatting)

cargo fmt --all
# → formatted shallow.rs and tests/visit/access_classifier_boundary.rs

cargo fmt --all -- --check
# → exit 0 (clean)

# Final gate run:
cargo build --all-targets && cargo clippy --all-targets -- -D warnings
# → both clean

cargo test --test access_classifier_boundary && cargo test --test access_classifier
# → 4 passed + 10 passed; 0 failed
```

### Files changed

- `src/visit/shallow.rs` — added `SpellLoc` type alias, `return_wrappers` and
  `decl_init_wrappers` fields to `Collector`, context-capture block in `visit()`,
  and lookup block in `emit_uses_edge()` before `classify_use` call.
  Also imported `AccessKind` alongside `classify_use`.

### Deviations from plan.md
- None: this is a QD-3 retry with a targeted fix scope.

### Test results
- `cargo test --test access_classifier_boundary`: 4 passed (was 2 passed, 2 failed)
- `cargo test --test access_classifier`: 10 passed (no regression)
- `cargo clippy --all-targets -- -D warnings`: clean
- `cargo fmt --all -- --check`: clean
- `cargo build --all-targets`: clean

### Exit-gate status
- BUILD_FAIL: clear
- LINT_FAIL: clear
- TEST_FAIL: clear

### Follow-ups
- None for QD-3 scope. QD-1 (schema_drift_live_neo4j template_params/template_args)
  remains open; routed to devops per test-report.md.
