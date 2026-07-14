# Fix template specialization handling

Status: implemented (2026-07-14)  
Scope: C++ LibTooling indexer only  
Schema impact: none expected (reuse existing symbol fields, `template_arg`, and
`specializes`/`instantiates` edges)  
Data impact: full reindex and resolve required

## Problem

Template handling is currently declaration-driven for class specializations but
call-driven for function and method specializations. This produces incomplete
or inconsistent graph data:

- partial class specializations are skipped by the declaration visitor;
- an unused partial specialization may be absent entirely;
- a used partial specialization is recovered as an incomplete synthetic node,
  without its definition, template parameters, or `specializes` edge;
- explicit function and method specializations are stored as symbols but lack
  template arguments and a relationship to their primary template;
- uncalled explicit function and method instantiations may be absent;
- inferred method-template arguments are lost because the method path reads
  only explicit `<...>` arguments at the call site;
- explicit class instantiations have an `instantiates` edge but retain
  `is_instantiation = false` on the symbol.

These are extraction gaps, not merely stale documentation. The intended graph
contract remains:

- explicit and partial specializations use `specializes`;
- implicit and explicit instantiations use `instantiates`;
- the specialization or instantiation is a first-class symbol with its own USR;
- template arguments belong to that concrete symbol;
- callable declarations are modeled even when never called.

## Required behavior

### Class templates

1. Index a partial specialization definition as a first-class template symbol.
2. Emit `partial specialization -> primary` as `specializes`.
3. Store the partial specialization's template parameters and pattern arguments.
4. Emit a full explicit specialization as `specializes` with all arguments.
5. Emit explicit-instantiation declarations and definitions as `instantiates`.
6. Set `is_instantiation = true` for explicit and implicit instantiation symbols.
7. When a concrete class instance selects a partial specialization, emit
   `instance -> partial specialization` as `instantiates`.

### Function and method templates

1. Handle explicit specializations and explicit instantiations during the
   declaration pass, independently of call sites.
2. Emit explicit specializations as `specializes` and explicit instantiations
   as `instantiates`.
3. Mark only instantiations with `is_instantiation = true`.
4. Store arguments from `FunctionDecl::getTemplateSpecializationArgs()` for
   both free functions and methods, including deduced and defaulted arguments.
5. Preserve `method_of` from every concrete method specialization or
   instantiation to its owning concrete class.
6. Keep call edges targeting the concrete callable symbol.
7. Use call-site `TemplateArgumentLoc` only to preserve as-written type spelling;
   it must not be the source of truth for whether arguments exist.

### Argument encoding

All paths must continue through `TemplateArgumentEncoder`. The schema-v29
contract remains:

- `1`: type;
- `2`: non-type value;
- `3`: template-template;
- `4`: pack;
- `Null`: no row.

Do not add another argument encoder or parse template syntax from source text.

## Implementation outline

### Declaration extraction

- Update `SymbolVisitor`/`is_template_pattern` so a partial-specialization
  declaration is retained while the duplicated templated record pattern remains
  suppressed.
- Add a dedicated partial-specialization callback in
  `DeclarationEdgeVisitor`; do not route it through the branch that currently
  skips `ClassTemplatePartialSpecializationDecl`.
- Generalize template-parameter emission to accept a
  `TemplateParameterList`, allowing class partial specializations to use the
  canonical path.
- Add callable-specialization handling for `FunctionDecl`/`CXXMethodDecl` based
  on `getTemplateSpecializationKind()` and `getPrimaryTemplate()` (or the
  instantiated-from declaration for member specializations).
- Share one helper between declaration-time and call-time callable handling so
  symbol flags, arguments, display names, and structural edges cannot diverge.
- Make structural edge emission idempotent. A later call must not increment or
  duplicate a specialization-to-primary relationship already emitted from the
  declaration.

### Call extraction

- Replace the method-only explicit-call-site fallback with the callable's full
  `getTemplateSpecializationArgs()` list.
- Optionally overlay as-written types from `TemplateArgumentLoc` when positions
  align.
- Keep call-site processing responsible for `calls`, `edge_site`, and
  `call_arg`; declaration processing owns template identity and relationships.

### Symbol flags

- Set `is_instantiation` from `TemplateSpecializationKind`, not from whether the
  symbol happened to be minted at a call or type-use site.
- Explicit specializations must remain `is_instantiation = false`.

## Acceptance matrix

| C++ construct | Required result |
|---|---|
| `template<class T> struct Box<T*>` | indexed definition with parameter `T`, pattern argument, and `specializes -> Box<T>` |
| unused `Box<T*>` partial specialization | remains queryable without a concrete use |
| `Box<int*> value` | concrete `Box<int*> instantiates Box<T*>` with argument `int*` |
| `template<> struct Box<bool>` | `specializes -> Box<T>` with argument `bool` |
| `template struct Box<int>` | `instantiates -> Box<T>`, argument `int`, `is_instantiation = true` |
| `extern template struct Box<long>` | same relationship and flag without requiring a definition body |
| `template<> int twice<int>(int)` | indexed without calls, argument `int`, `specializes -> twice<T>` |
| `template double twice<double>(double)` | indexed without calls, argument `double`, `instantiates -> twice<T>`, flag true |
| `template<> int Worker::convert<int>(int)` | indexed without calls, `specializes`, argument `int`, and `method_of Worker` |
| inferred `worker.convert(float_value)` | concrete method instance records argument `float` |
| explicit `worker.convert<char>(value)` | concrete method instance records argument `char` |

For every row, assert the concrete USR, symbol kind, flags, template arguments,
relationship kind and target, and absence of duplicate structural edges.

## Tests and verification

Add focused real-Clang fixtures covering unused declarations, partial selection,
full specialization, explicit-instantiation declaration/definition, inferred
arguments, explicit arguments, non-type arguments, template-template arguments,
and packs.

Required gates:

1. focused AST/index tests;
2. `ctest --test-dir build -L default --output-on-failure`;
3. `ctest --test-dir build -L clang --output-on-failure`;
4. full Python read/query suite;
5. normalized Layer-0 row-set comparison for unrelated fixtures;
6. regenerate `index.db`, run `cidx resolve`, and verify schema version 29.

## Relevant implementation files

- `src/ast/symbol_visitor.cpp`
- `src/ast/declaration_edge_visitor.cpp`
- `src/ast/call_template_args.cpp`
- `src/ast/call_edge_emitter.cpp`
- `src/ast/instantiation_edges.cpp`
- `src/ast/instance_minter.cpp`
- `src/ast/template_argument_encoder.cpp`
- `tests/ast_visitor_test.cpp`
- `tests/index_golden_test.cpp`

## Representation limits (as implemented)

Two bounds come from Clang's data model and repo-wide storage semantics, and
are pinned by tests rather than worked around:

- A function explicit-instantiation statement produces no AST node of its
  own, and the specialization keeps a single, first-write-wins
  `PointOfInstantiation`. Ownership/location therefore anchor at the
  specialization's FIRST materialization point in the TU (a preceding
  implicit use, or a preceding `extern template` declaration). Removing the
  statement holding that point re-anchors the symbol to the next remaining
  point on reindex. The definition-directive's own location is unrecoverable
  when an earlier point exists.
- Reindexing a file never garbage-collects symbol or `template_arg` rows of
  removed declarations (true for every declaration kind, not just
  instantiations); per-file cleanup covers edges and definitions. The
  guarantee here is relationship cleanup from the owning file.

## Non-goals

- extending the retired Python/libclang indexer;
- changing template argument kind codes;
- parsing USRs or source tokens to recover arguments;
- changing query output ordering or formatting;
- adding new schema tables or edge kinds.
