# `src/ast` — the indexing engine

[← docs index](../README.md) · related: [data flow](../data-flow.md) · [toolchain](toolchain.md) · [storage](storage.md)

The Layer-0 extraction engine, built on the **Clang C++ API** (LibTooling,
`clang::RecursiveASTVisitor`). It is the sole indexer: the libclang C API was
removed in the LibTooling cutover, and the retired byte-parity scaffolding was
removed in the visitor refactor (`docs/improvements/refactoring.md`). Clang
owns traversal; the visitors own the mapping from typed AST facts to cidx
records. Namespace `cidx::ast`; compiled as the `cidx_ast` object library.

## Architecture

```text
IndexFrontendAction
  -> IndexASTConsumer
       -> TranslationUnitIndexer      (per-TU stages)
            -> SymbolVisitor
            -> DeclarationEdgeVisitor
            -> FunctionDefinitionVisitor
                 -> StatementEdgeVisitor
            -> NamespaceUseVisitor

Visitors -> typed Clang AST accessors
         -> encoders/classifiers (TemplateArgumentEncoder, value provenance)
         -> SymbolEmitter / EdgeSink -> Storage
```

Entry point: `run_index_one` (`index_engine.cpp`) — flag assembly
(`build_clang_arguments` + [toolchain](toolchain.md) paths), `CompilationSetup`
(fixed compilation database + `ClangTool`), `read_strict_mode` (`CIDX_STRICT`),
the tool run, and `apply_diagnostic_policy` (a diagnostic at/above the abort
level fails the TU with no rows written). Per TU, `TranslationUnitIndexer`
runs the named stages `run_symbol_pass(main)` → `plan_owned_headers` →
`run_header_passes` → `run_edge_pass(main)` — the
[per-file interleave](../data-flow.md#the-per-file-interleave).

## Visitor responsibilities

| Component | Responsibility | Must not do |
|---|---|---|
| `IndexFrontendAction` | create the consumer, record inclusions (PPCallbacks), collect diagnostics | extract symbols or edges |
| `IndexASTConsumer` | diagnostics gate + run the per-TU stages | contain extraction logic |
| `SymbolVisitor` | symbols and declaration sites from `NamedDecl` | parse type/name strings |
| `DeclarationEdgeVisitor` | declaration/type-structure edges: contains, inherits (+CRTP), field_of, method_of, overrides, friend, specializes/instantiates, template params, static-member definitions | traverse function bodies |
| `FunctionDefinitionVisitor` | find indexable function definitions, create `definition` rows, run a `StatementEdgeVisitor` per body | inspect expressions |
| `StatementEdgeVisitor` | calls (incl. overload fan-out and factory edges), references, construction/destruction forms, local declarations, provenance | own storage transactions |
| `NamespaceUseVisitor` | namespace-use facts from typed qualifiers/directives | parse qualified-name strings |
| `EdgeEmissionContext` | per-definition identity, conditional depth, mint/encode utilities, edge+site emission | traverse the AST |
| `CallEdgeEmitter` | convert a resolved call target/site into edge/edge_site/call_arg records | resolve AST traversal |
| `TemplateArgumentEncoder` | the ONE `TemplateArgument` → `template_arg` conversion (see below) | string-based type lookup |
| value provenance (`value_provenance.*`, `receiver_provenance.*`) | classify expressions/receivers into provenance categories | preserve libclang bugs |

## Traversal rules

Production passes derive directly from `RecursiveASTVisitor` and implement
behavior in `Visit*` callbacks. The allowed `Traverse*` overrides are narrow,
base-delegating, and each exists for a documented cidx-semantic reason:

- `StatementEdgeVisitor`: `TraverseIfStmt`/`TraverseForStmt`/
  `TraverseWhileStmt`/`TraverseDoStmt`/`TraverseSwitchStmt`/
  `TraverseConditionalOperator` — scoped conditional depth (RAII `CondScope`;
  `CaseStmt` needs none, a case label is always inside its switch's guard);
  `TraverseVarDecl`/`TraverseCXXNewExpr` — record the direct-initializer
  expression selecting the construction form. Single-argument on purpose: RAV
  then dispatches without the data-recursion queue, so the guard brackets the
  whole subtree.
- `NamespaceUseVisitor`: `TraverseDecl` (scope stack) and
  `TraverseNestedNameSpecifierLoc` (qualifier extraction is its job).

No generic parent stack, no statement-child replay, no `TypeLoc`/qualifier
traversal suppression, and no `dataTraverseStmtPre/Post` remain.

## Record ordering

Persistence order is NOT a public contract: surrogate ids (`edge.id`,
`edge_site.edge_id`) follow RAV traversal order. Semantic comparisons use
normalized, ordered projections — the golden gate (`tests/index_golden_test.cpp`)
and `scripts/dump_layer0.sh` resolve surrogate keys to USR/kind/basename and
`ORDER BY` them. Duplicate facts are collapsed by the storage layer's
`ON CONFLICT` upserts on semantic keys.

## Template arguments

Every extraction path — class-spec edges, free-function and method call sites,
minted instances, instantiated owners, local-variable declarations — encodes
through `TemplateArgumentEncoder`, exhaustively over
`clang::TemplateArgument::ArgKind` with the canonical mapping
`1=type 2=non-type 3=template-template 4=pack` (Null emits no row).
The full contract, including literal/ref_id rules and the v28→v29 migration,
is [docs/improvements/template-arg-contract.md](../improvements/template-arg-contract.md).

## Value provenance

`normalize_value_expr` strips only provenance-neutral wrappers (parens,
implicit casts, cleanups, temporary materialization/binding, C-style value
casts) plus the two explicitly handled provenance-preserving unary operators
`&` and `*`. Every other spelled operator stays visible and classifies as a
derived value. `classify_value_source` categories:
local | global | member | this | construct | call_result | literal | unknown.
Locals include static locals, template-pattern locals, and lambda captures.
`classify_call_receiver` reuses the same classifier for receivers and resolves
`recv_param_pos` via `ParmVarDecl::getFunctionScopeIndex`.

## Complexity gate

`src/ast/.clang-tidy` scopes `readability-function-size` (statements ≤ 40,
nesting ≤ 4) and `readability-function-cognitive-complexity` (≤ 25) to this
module; run `scripts/check_ast_complexity.sh <builddir>`. A longer function
requires a written justification beside it.

## Files

| Concern | Files |
|---|---|
| engine driver | `index_engine.{hpp,cpp}` |
| symbol pass | `symbol_visitor.*`, `symbol_extractor.*`, `symbol_record.hpp`, `symbol_emitter.hpp`, `storage_symbol_sink.*`, `decl_flags.*`, `kind_map.*` |
| declaration edges | `declaration_edge_visitor.*`, `type_use.*`, `mint_builder.*`, `instance_minter.*` |
| body pass | `function_definition_visitor.*`, `statement_edge_visitor.*`, `edge_emission_context.*`, `call_edge_emitter.*`, `call_template_args.*`, `instantiation_edges.*` |
| namespace uses | `namespace_use_visitor.*` |
| encoding/classification | `template_argument_encoder.*`, `value_provenance.*`, `receiver_provenance.*`, `display_name_rewrite.*`, `names.*` |
| records/sinks | `edge_records.hpp`, `edge_sink.hpp`, `storage_edge_sink.*` |
| shared helpers | `usr.*`, `location.*`, `header_stats.hpp`, `clang_compat.hpp`, `clang_version.*` |
