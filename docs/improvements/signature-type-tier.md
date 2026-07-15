# Signature/type tier (schema v30) — implementation record

Status: implemented (Stage 1 of the
[LibTooling graph extraction roadmap](libtooling-graph-improvements.md))
Landed: 2026-07-15
Schema: 29 → 30

## What was added

Parameters and normalized types are now first-class graph facts, making
callable signatures traversable instead of strings and coarse `uses` edges.

### Storage (both `src/storage/storage.cpp` and `python/indexer/storage.py`)

- `type_node` — one row per distinct type SHAPE, keyed by `type_key`, a
  deterministic structural encoding (grammar documented in
  `src/ast/type_graph.cpp`). Carries display spelling, `type_kind`,
  const/volatile/restrict flags, the named declaration's USR (`decl_usr`),
  and a `canonical_id` link from sugared shapes to their canonical shape.
- `type_edge` — structural relations between type nodes: `pointee`,
  `element_type`, `alias_of`, `return_type`, `param_type`,
  `template_argument_type` (positional).
- `parameter` — one row per callable parameter, keyed `(owner_id, position)`;
  name/site optional attributes; `type_id` into `type_node`. Refreshed
  wholesale per owner on re-index (`replace_parameters`), so arity changes
  leave no stale rows.
- `symbol_type` — symbol → type relations: `returns` (callables;
  ctors/dtors record none), `of_type` (variables/fields), `underlying_type`
  (typedef/alias symbols).
- Seed tables `type_kind`, `type_edge_kind`, `symbol_type_kind` (display
  names, same pattern as `symbol_kind`/`edge_kind`).

Migration v29 → v30 is purely additive (tables created by the schema script;
version stamped); old data untouched. Python is storage + read-query parity
only — the tables are written exclusively by the C++ LibTooling indexer.

### Extraction (`src/ast/type_graph.{hpp,cpp}` + `DeclarationEdgeVisitor`)

`TypeInterner` walks a `clang::QualType` bottom-up and interns one node per
layer. Named alias layers (`TypedefType`, alias-template specializations)
keep their identity and link one desugar step via `alias_of`; transparent
sugar (elaborated/paren/subst/...) is skipped; class-template specialization
nodes expose their (canonical) type arguments via `template_argument_type`
edges. Everything else falls back to one opaque node keyed by the canonical
print — no unbounded AST persistence.

Emission points mirror the existing signature-uses walk policy exactly
(non-template callables at their declaration sites; fields; variables;
typedef/alias decls). Function templates are skipped, matching
`emit_signature_uses`.

### Query surface

- C++: `GraphQuery::signature(sym_id)` and
  `GraphQuery::type_users(usr, limit)` (recursive closure over `type_edge` +
  `canonical_id`, anchored on `type_node.decl_usr`).
- CLI: `cidx graph signature <selector>` and
  `cidx graph typeusers <selector>` (text + `--json`).
- Python read parity: `GraphQuery.signature` / `GraphQuery.type_users` in
  `indexer/query.py` (same SQL, same ordering).

## Tests

- `tests/ast_visitor_test.cpp` — five fixture cases: params/returns/of_type/
  underlying facts; type shapes + alias canonical link + template-arg edges +
  reverse closure; template-pattern exclusion; wholesale arity refresh on
  re-index; ctor params without returns.
- `tests/storage_migration_test.cpp` — v29 → v30 migration (tables created,
  version stamped, old rows intact, intern round-trip).
- `tests/index_golden_test.cpp` — four new normalized projections
  (`type_node`/`type_edge`/`parameter`/`symbol_type`), keyed by `type_key`
  and USR; golden regenerated (purely additive diff).
- `python/tests/test_migrate_v30.py` — migration + `signature`/`type_users`
  read parity on hand-inserted rows.

## Review fixes (PR #18, round 1)

- **Alias retarget**: `intern_type_node` is now an authoritative upsert
  (`ON CONFLICT DO UPDATE`) and `add_type_edge` an `OR REPLACE`, so
  `using Alias = Foo;` → `= Bar;` refreshes the alias node's
  `canonical_id`/`alias_of` in place on reindex. Sugared/canonical instances
  that encode to the same key keep `canonical_id` NULL (no self-loops); the
  display spelling is now the as-written form. Note: a callable with the
  alias in its *parameter list* changes USR on retarget (canonical types are
  encoded in function USRs) — the old symbol row and its parameter rows
  linger exactly like every other stale-USR symbol; that is the pre-existing
  symbol lifecycle, not a tier property.
- **Deterministic spelling (round 2)**: the conflict update is narrowed to
  `canonical_id` only. Every other column is derived from the key —
  structurally (kind/flags/decl_usr) or by first-writer-wins (`spelling`,
  which a key deliberately collapses across canonically equivalent written
  forms: `Box<Foo>` vs `Box<Alias>` share one node). A partial reindex can
  therefore never rewrite a shared node's display form; the first writer is
  always the canonical instance (emit_node interns the canonical target
  before the node itself), so the stored spelling is the canonical print.
- **Function type shape**: `FunctionProtoType` keys now encode variadicness
  (`,...`), method cv-quals, ref-qualifier, and throwability
  (`canThrow()` — spelling-insensitive), so `void(int)`, `void(int, ...)`
  and `void(int) noexcept` intern distinct nodes.
- **Edge-empty indexes**: `cidx graph signature`/`typeusers` open the index
  without the empty-`edge`-table rejection — a valid declaration-only TU has
  type facts but zero symbol edges. Edge queries keep the guard.

## Deliberate limits (deferred, per roadmap)

- No parameter facts for function templates (matches the signature-uses walk
  policy; revisit with the concepts/constraints phase).
- A specialization's `template_argument_type` edges use the spec decl's
  stored (canonical) arguments — `Box<FooAlias>` links to `Foo`, while the
  alias identity survives on the parameter node itself.
- `type_node` rows are an append-only interned dictionary; orphaned shapes
  after renames are harmless and bounded by distinct shapes seen.
- Concepts/constraints, occurrence roles, and the include graph are the next
  roadmap phases — not part of this change.
