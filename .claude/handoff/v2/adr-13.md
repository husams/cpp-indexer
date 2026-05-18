# ADR-13: USES access classifier — taxonomy, libclang strategy, EXTERNAL_REF mirroring scope

Status: accepted
Date: 2026-05-18
Resolves: requirements OQ-3 (EXTERNAL_REF mirroring), AC-S43-1..6, PRD §AC-S2, §9

## Context

S43 introduces `source_association_type` and `target_association_type` on every USES edge. Today USES edges carry no access classification; queries like Q5 in PRD §6 ("which methods *write* `DBImpl::mutex_`?") cannot be expressed without leaving the graph. The taxonomy must be a **closed enumeration of exactly seven values** (AC-S43-1): `read`, `write`, `addr_of`, `call_arg`, `return`, `decl_ref`, `unknown`. No eighth value is permitted to reach the sink.

libclang exposes cursor context via `cursor.get_kind()`, parent cursor traversal, and `CXCursor_*` predicates. The classifier runs at each USES emission site in `src/visit/shallow.rs`. Some constructs (overloaded operators, deref writes through pointers, conditional reads inside `?:`) cannot be classified confidently without a full data-flow pass that libclang does not provide. AC-S43-3 fixes the policy: classify as `unknown`, log, do not drop.

PRD §9 leaves open whether the cross-repo `EXTERNAL_REF` edge (synthesized by Phase 5 from a `cross_repo_candidate` USES that resolved to a different repo) also carries the classification. The natural source is the original edge's class — Phase 5 already reads the unresolved edge's attributes; mirroring is a copy.

## Decision

### Taxonomy (closed, exactly seven values)

| Value | Meaning | Primary libclang signal |
|---|---|---|
| `read` | Right-hand side of assignment, condition, return-value path, argument to const ref. | Parent is `BinaryOperator` `=` and cursor is on RHS; parent is `IfStmt`/`WhileStmt` condition; parent is `ReturnStmt`; parent is `CallExpr` and the matching parameter type is `T` or `const T&`. |
| `write` | LHS of assignment / compound assignment; `++`/`--` operand; output param via non-const ref/pointer. | Parent is `BinaryOperator` in {`=`, `+=`, `-=`, …}, cursor on LHS; parent is `UnaryOperator` `++`/`--`; parent is `CallExpr` and matching parameter type is `T&` (non-const). |
| `addr_of` | `&x`, function pointer formation, pass to pointer-typed param without explicit deref. | Parent `UnaryOperator` `&`; parent `CallExpr` with param type `T*` and cursor is the matching argument. |
| `call_arg` | Argument to a function call where the precise read/write semantics cannot be inferred from the param type (template arg, `auto`, variadic). | Parent `CallExpr` and matching param type is dependent / unresolved. |
| `return` | Cursor is the operand of `ReturnStmt`. | Parent `ReturnStmt`. (Subset of `read` but reported as `return` for query clarity.) |
| `decl_ref` | Reference inside a declaration's initializer / default value where the referenced entity is captured by name, not yet semantically used. | Parent is `VarDecl` initializer, `ParmDecl` default, `FieldDecl` initializer. |
| `unknown` | Anything else, or any case where parent traversal hits a node libclang cannot classify (overloaded `operator=` to a UDT, `*p = x`, `?:` arms, macro expansions, dependent expressions). | Default arm. |

`target_association_type` mirrors the source class symmetrically (AC-S43-2): if the source uses the target as a write, the target is the write *recipient*, so its association is also `write`. This is a copy of the source class for the seven values defined here; we do not introduce a separate target-side enumeration in M8.

Enforcement: the taxonomy is a Rust `enum AccessKind` with `as_str()` / `try_from_str()` mirroring the existing `NodeKind` pattern. The visitor type-checks at compile time; only `enum` variants can be emitted; AC-S43-1's "no eighth value" is structurally enforced.

### libclang strategy

- Single classifier function `fn classify_use(cursor: &Entity, parent_chain: &[Entity]) -> AccessKind` in a new `src/visit/access_classifier.rs`.
- Inputs: the cursor that triggered the USES edge plus a walk of up to 4 parents (guard against runaway). Walking 4 parents covers `BinaryOperator → CompoundStmt → FunctionDecl` and `CallExpr → MemberExpr → BinaryOperator`.
- Decision order: `ReturnStmt` → `BinaryOperator` (split LHS/RHS) → `UnaryOperator` → `CallExpr` (param-type-driven) → `VarDecl`/`FieldDecl`/`ParmDecl` initializer → fallback `unknown`.
- Logging: when the classifier returns `unknown`, log at `debug` level with `(usr, file_path, line, parent_kinds)`. Do not log at `info`/`warn` — `unknown` is an expected outcome, not a defect.

### EXTERNAL_REF mirroring scope

**In scope for M8.** Phase 5 (`src/resolve/cross_repo.rs`) reads the unresolved USES edge's `attrs_json`/native fields to construct the `EXTERNAL_REF` edge. When the source USES edge carries `source_association_type` and `target_association_type` (post-M8), Phase 5 copies both values onto the synthesized `EXTERNAL_REF`. No new classification logic is run at Phase 5; it is a field copy.

Rationale for in-scope (overriding "Suggest yes" in PRD §9 with a binding decision): the mirror is a one-line copy in Phase 5's edge-builder; deferring would require a second schema bump (ADR-9: any added required attribute bumps) when adding it later. Doing it in M8 piggybacks on the v5 bump.

Test coverage: at least one Phase 5 integration test asserts that an `EXTERNAL_REF` derived from a USES with `source_association_type = 'write'` carries the same value.

## Alternatives considered

- **Open enumeration / free-form string.** Rejected: AC-S43-1 explicitly forbids it; a `String` field leaks classifier bugs into consumer queries.
- **Two parallel taxonomies (source-side vs target-side).** Rejected: doubles the schema surface for a use case (target-side asymmetry) PRD §2.2 does not motivate; can be added later if a real query needs it.
- **Defer EXTERNAL_REF mirroring to M9.** Rejected: forces a second `SCHEMA_VERSION` bump when M8's bump is already in flight; cost of in-scope is one field copy in Phase 5.
- **Run the classifier at Phase 5 over the unresolved cursor.** Rejected: Phase 5 does not have the cursor anymore (it's downstream of Phase 1 emission); reclassifying from the materialized edge is impossible without re-parsing the TU.
- **Use clang AST-matchers (clang-query) instead of cursor-walk.** Rejected: cpp-indexer's libclang-rust binding does not expose AST-matchers; the cursor-walk approach is consistent with existing visitor code.

## Consequences

Positive:
- Closed `enum` makes invalid values unrepresentable at compile time.
- `unknown` fallback satisfies AC-S43-3 and gives a measurable defect signal (count of `unknown` over corpus).
- EXTERNAL_REF mirroring lands in the same schema bump.

Negative:
- Classifier accuracy on overloaded operators and deref-writes is intentionally `unknown`; consumers querying `source_association_type = 'write'` may miss some real writes. Acceptable per AC-S43-3 and PRD §7 (graceful degradation).
- Parent-chain walk adds O(depth ≤ 4) overhead per USES emission; existing visitor already walks parents for context (`src/visit/shallow.rs` macro-expansion paths) so the cost is in the same order of magnitude.

Follow-ups:
- After M8 lands, instrument the `unknown` rate. If >25% of USES edges classify as `unknown`, evaluate per-construct fixes (deref tracking, overloaded operator resolution) in M9.
- Consider promoting a small set of common operator overloads (`operator=`, `operator+=`) to `write` with a curated allow-list — deferred until measurement justifies it.

## References

- `[[pages/planning/cpp-indexer-structured-attrs-prd]]` §AC-S2, §6 Q5, §7, §9
- requirements.md AC-S43-1..6, OQ-3
- `src/visit/shallow.rs` (USES emission site)
- `src/resolve/cross_repo.rs` (EXTERNAL_REF synthesis)
- Cognee tags: `task:cpp-indexer-m8 role:architect`
