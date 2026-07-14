# Clang visitor indexing refactoring plan

- Status: implemented (phases 0–6); 2026-07-14 review findings addressed
- Scope: C++ Layer-0 indexing under `src/ast/`
- Primary goal: make Clang's typed AST and `RecursiveASTVisitor` the design,
  rather than reproducing the retired libclang walk inside visitor-shaped code.

## Executive decision

The current implementation uses `clang::RecursiveASTVisitor` (RAV), but it
still preserves too much of the former libclang traversal contract:

- six CRTP visitor mixins forward callbacks into free handlers;
- `BodyVisitor` and `BodyPassVisitor` divide responsibilities using the vague
  term "body";
- custom pre/post stacks and `Traverse*` overrides reproduce traversal order;
- expression peeling and template-name resolution reimplement typed Clang
  operations;
- template argument conversion is repeated in several paths and has already
  diverged;
- several extraction and orchestration functions remain 80--122 lines;
- active names and documentation still describe an optional `lt` backend,
  libclang parity, and modules that no longer exist.

The refactor will replace this with a small number of directly named RAV
visitors. Each visitor will use Clang AST accessors, emit semantic records
through narrow sinks, and accept Clang's traversal unless a documented cidx
semantic requirement makes a `Traverse*` override unavoidable.

This is not another indexing-engine rewrite. It is a constrained cleanup of
the sole C++ indexing implementation.

## Goals

1. Make every AST pass directly and visibly based on Clang visitor APIs.
2. Remove custom recursion, child-order replay, forwarding mixins, arbitrary
   peel-depth limits, and source-spelling parsers where Clang exposes a typed
   answer.
3. Give classes, files, methods, and namespaces responsibility-based names.
4. Keep visitor callbacks small and make orchestration read as a sequence of
   named stages.
5. Centralize record conversion rules, especially template arguments, source
   locations, and call/receiver provenance.
6. Correct compatibility bugs that only existed to match the retired libclang
   implementation.
7. Preserve deterministic public output and the Layer-0 storage contract unless
   an explicitly approved correction requires a schema or product-version
   change.
8. Rewrite documentation so it describes the LibTooling-only implementation.

## Non-goals

- No new graph node or edge families.
- No Concepts, macro graph, CFG, CTU, or new analysis features. Those remain in
  the other improvement plans.
- No Python indexer extension. Python remains storage/read-query only.
- No redesign of `Storage`, `resolve`, Layer-1 entities, CLI output, or query
  APIs except where a corrected Layer-0 fact necessarily affects them.
- No adoption of `clang::index::IndexingAction` as the production engine in
  this refactor. It may remain a later coverage oracle.
- No new dependency.

## Required design rules

### 1. Clang owns traversal

- Each production pass directly derives from
  `clang::RecursiveASTVisitor<ConcreteVisitor>`.
- Normal behavior is implemented in `Visit*` callbacks.
- A `Traverse*` override is allowed only when it:
  1. expresses cidx context that a `Visit*` callback cannot express;
  2. delegates child traversal to the matching RAV base implementation; and
  3. has a focused test proving why it exists.
- Production code must not manually enumerate statement children to reproduce
  a historical visitation order.
- Persistence order must be made deterministic at the record/sink boundary;
  traversal order is not a public contract.
- `dataTraverseStmtPre/Post` must not be used to maintain a parallel generic
  AST walk. If scoped state is required, use a narrow `TraverseIfStmt`,
  `TraverseForStmt`, or equivalent override with an RAII guard and a call to
  the base traversal.

### 2. Prefer typed Clang facts

Use the narrow typed API for the question being answered:

| Required fact | Preferred Clang API |
|---|---|
| Direct call target | `CallExpr::getDirectCallee()` / `getCalleeDecl()` |
| Method receiver | `CXXMemberCallExpr::getImplicitObjectArgument()` |
| Referenced declaration | `DeclRefExpr::getDecl()` / `MemberExpr::getMemberDecl()` |
| Local/global classification | `VarDecl::isLocalVarDecl()`, `hasLocalStorage()`, declaration context |
| Ignore non-semantic wrappers | `Expr::IgnoreParenImpCasts()`, `IgnoreImplicit()`, or `IgnoreUnlessSpelledInSource()` |
| Class template arguments | `ClassTemplateSpecializationDecl::getTemplateArgs()` |
| Function template arguments | `FunctionDecl::getTemplateSpecializationArgs()` and `getTemplateSpecializationArgsAsWritten()` |
| Template identity | `getPrimaryTemplate()`, `getSpecializedTemplate()`, specialization-kind APIs |
| Type declaration | `QualType`, `Type::getAsTagDecl()`, `getAsCXXRecordDecl()` |
| Declaration identity | `clang::index::generateUSRForDecl()` |
| Declaration name | `DeclarationName`, `NamedDecl`, `PrintingPolicy` |
| Namespace qualifier | `NestedNameSpecifierLoc` and referenced namespace declarations |
| Source spelling/location | `SourceManager`, `Lexer`, `SourceRange` |

Custom code is appropriate for mapping typed AST facts into cidx graph
semantics. It is not appropriate for reparsing C++ syntax, recreating AST
parentage, or emulating removed cursor behavior.

### 3. Complexity limits

- Visitor `Visit*` methods: target at most 25 logical lines.
- `Traverse*` overrides: target at most 20 logical lines, including the base
  traversal call.
- Other functions/methods: target at most 40 logical lines.
- Maximum nesting depth: three blocks.
- A longer function requires a written justification beside the function and
  a follow-up issue; compatibility with the retired implementation is not a
  justification.
- Add `clang-tidy` checks using the already installed Clang toolchain:
  `readability-function-size` and `readability-function-cognitive-complexity`.
  Configure them for `src/ast/`; do not add a library dependency.

## Target architecture

```text
IndexFrontendAction
  -> IndexASTConsumer
       -> SymbolVisitor
       -> DeclarationEdgeVisitor
       -> FunctionDefinitionVisitor
            -> StatementEdgeVisitor
       -> NamespaceUseVisitor

Visitors
  -> typed Clang AST accessors
  -> focused encoders/classifiers
  -> SymbolSink / EdgeSink
  -> Storage adapters
```

### Responsibilities

| Component | Responsibility | Must not do |
|---|---|---|
| `IndexFrontendAction` | Create the consumer and connect diagnostics/preprocessor state | Extract symbols or edges |
| `IndexASTConsumer` | Run named passes for the main file and owned headers | Contain extraction logic or long lambdas |
| `SymbolVisitor` | Emit symbols and declaration sites from `NamedDecl` | Parse type/name strings |
| `DeclarationEdgeVisitor` | Emit declaration/type-structure edges | Traverse function bodies manually |
| `FunctionDefinitionVisitor` | Identify indexable function definitions and create definition scope | Inspect every expression kind |
| `StatementEdgeVisitor` | Emit calls, references, construction/destruction, local declaration, and provenance facts | Own storage transactions or replay RAV children |
| `NamespaceUseVisitor` | Emit namespace-use facts from typed qualifiers/directives | Parse qualified-name strings |
| `EdgeEmissionContext` | Hold current source definition, file identity, conditional depth, and sink access | Traverse the AST |
| `CallEdgeEmitter` | Convert an already identified call target/site into cidx records | Resolve AST traversal or own unrelated template logic |
| `TemplateArgumentEncoder` | Convert `TemplateArgument`/`TemplateArgumentLoc` into one canonical storage representation | Query by reparsed type spelling |
| `ValueProvenanceClassifier` | Map a typed expression/declaration into cidx provenance categories | Remove arbitrary AST nodes or preserve libclang bugs |

### Naming migration

| Current name | Target name/action |
|---|---|
| namespace `cidx::lt` | `cidx::ast` |
| `lt_engine.*` | `index_engine.*` |
| `EngineConsumer` | `IndexASTConsumer` |
| `EngineAction` / factory variants | `IndexFrontendAction` / `IndexFrontendActionFactory` |
| `EdgeVisitor` | `DeclarationEdgeVisitor` |
| `BodyPassVisitor` | `FunctionDefinitionVisitor` |
| `BodyVisitor` | `StatementEdgeVisitor` |
| `BodyEmitContext` | `EdgeEmissionContext` |
| `CallEmitter` | `CallEdgeEmitter` |
| `NsUsesVisitor` | `NamespaceUseVisitor` |
| `value_source.*` | `value_provenance.*` |
| `llvm_compat.hpp` | `clang_compat.hpp` |
| `edge_records2.hpp` | merge into `edge_records.hpp` |
| every `*VisitorMixin` | delete; callbacks move onto `StatementEdgeVisitor` |

Avoid `body`, `lt`, `classic`, `cursor`, and `parity` in active production
names unless the term is literally part of a Clang API or historical document.

## Problem-specific corrections

### Function-template local provenance

`classify_value_source` currently preserves a former cursor bug by treating a
local variable inside a function-template pattern as global. Replace the
compatibility branch with Clang's local-variable/storage predicates. Add
positive and negative fixtures for:

- ordinary function locals;
- function-template locals;
- method-template locals;
- lambda locals and captures;
- static local variables;
- namespace/file-scope globals;
- class static data members;
- parameters.

The resulting `call_arg` and receiver provenance must use the same classifier.

### Expression normalization

Delete `unwrap_once` and the fixed-depth `peel_expr` loop. Introduce, only if a
shared name is useful, a one-line wrapper around the selected Clang API.
Semantic unary operators must remain visible unless the specific classifier
handles that operator explicitly. Test `x`, `(x)`, implicit casts, explicit
casts, `&x`, `*p`, `!x`, `-x`, cleanups, and materialized temporaries.

### Template arguments

Create one `TemplateArgumentEncoder` used by:

- class-template specialization edges;
- function/method-template call sites;
- minted class instances;
- callable instantiation edges;
- display-name formatting when a stored display value is still required.

Before implementation, document and test the one canonical `arg_kind` mapping.
The current paths disagree for packs and some non-type/template kinds. The
encoder must handle every `clang::TemplateArgument::ArgKind` explicitly and
produce the same meaning regardless of extraction path. Prefer a referenced
declaration/type directly from the AST; remove string-based `base_name` lookup
and `TemplateArgResolver` once all callers use typed declarations.

If normalizing `arg_kind` changes the persisted meaning, treat it as an
on-disk semantic change: bump both schema-version declarations, add migration
and old-database tests, require reindexing, and refresh `index.db`.

### Names and display text

Remove code whose sole purpose is mirroring `clang_getCursorDisplayName`.
Define the cidx display contract independently:

- stable symbol identity remains USR;
- qualified names come from `NamedDecl`/`DeclarationName`;
- signatures and template arguments use a single `PrintingPolicy` configured
  in one place;
- display text is presentation, never an input to semantic resolution;
- no parsing of `<...>`, qualifiers, pointers, references, or namespaces from
  strings.

### Traversal order and duplicate suppression

Delete range-for child replay and `TraverseTypeLoc`/
`TraverseNestedNameSpecifierLoc`/`TraverseTemplateArgumentLoc` suppression
unless a focused test demonstrates a current semantic requirement. Let RAV
perform its canonical traversal. Prevent duplicate facts by canonical record
keys in the sink/collector, then sort records deterministically before writing
when stable write order matters.

Do not encode historical traversal order into visitor structure.

### Large orchestration methods

Split `EngineConsumer::HandleTranslationUnit` into named operations:

- `diagnostics_allow_indexing()`;
- `run_symbol_pass(file)`;
- `plan_owned_headers()`;
- `run_header_passes(plan)`;
- `run_edge_pass(file)`.

Split `run_index_one` into:

- `build_clang_arguments()`;
- `create_compilation_database()`;
- `read_strict_mode()`;
- `run_clang_tool()`;
- `apply_diagnostic_policy()`.

Split long extraction routines by semantic record, not by AST spelling. For
example, `emit_callable_template_args` becomes a short request to the shared
template encoder plus an independently named display update.

## Delivery phases

### Phase 0 — contracts and baseline

1. Capture the current default and Clang test results.
2. Record normalized Layer-0 table output for the manifests corpus.
3. Add focused failing tests for function-template local provenance, unary
   expression handling, and template pack/kind consistency.
4. Write the canonical template `arg_kind` contract.
5. Decide whether the template correction requires schema v29 or only a
   product-version bump plus reindex.

Exit gate: the new tests fail for the intended reasons; all pre-existing tests
retain their baseline result.

### Phase 1 — direct statement visitor

1. Move the nine `Visit*` callbacks directly onto `StatementEdgeVisitor`.
2. Delete all six `*VisitorMixin` classes and their source/header files.
3. Keep existing emitters temporarily so this phase is structurally isolated.
4. Replace generic pre/post stack usage with narrow scoped traversal overrides.
5. Remove manual range-for traversal and traversal suppression where tests show
   it is unnecessary.

Exit gate: no production `*VisitorMixin`; no manual statement-child recursion;
existing semantic row sets remain stable except for explicitly approved
ordering-neutral differences.

### Phase 2 — typed AST normalization and provenance

1. Replace `peel_expr`/`unwrap_once` with Clang normalization APIs.
2. Correct local/global classification using `VarDecl` APIs.
3. Use `getImplicitObjectArgument()` for receivers.
4. Use direct callee/reference accessors before considering unresolved
   candidate sets.
5. Rename `value_source` to `value_provenance` and make classification tables
   exhaustive and testable.

Exit gate: provenance fixtures pass; no fixed-depth AST peeling remains; no
compatibility comments preserve known libclang misclassification.

### Phase 3 — canonical template and name handling

1. Add `TemplateArgumentEncoder` and route every template path through it.
2. Remove duplicate `TemplateArgRecord` switches and loops.
3. Remove `base_name` and string-based template type resolution.
4. Define one `PrintingPolicy` provider and simplify names/display helpers.
5. Apply the schema/version decision from Phase 0.

Exit gate: one template argument mapping; all argument kinds covered; nested,
defaulted, deduced, non-type, template-template, and pack fixtures pass.

### Phase 4 — orchestration and method-size cleanup

1. Decompose `HandleTranslationUnit` and `run_index_one` into named stages.
2. Split extraction methods exceeding the complexity limits.
3. Merge record headers and remove numbered or compatibility-only files.
4. Add the scoped clang-tidy complexity gate.

Exit gate: no unjustified function in `src/ast/` exceeds the agreed limits;
orchestration contains no extraction lambdas or AST-specific branching.

### Phase 5 — naming and file layout

1. Apply the naming migration table.
2. Rename namespace `cidx::lt` to `cidx::ast`.
3. Move the remaining `src/clangx/toolchain.*` into a clearly named
   `src/toolchain/` module, because `clangx` no longer represents an engine.
4. Audit `main.cpp`/`index_main.cpp` and action/consumer variants; retain only
   entry points with distinct, documented purposes.
5. Update CMake source lists and semantic-index paths atomically with renames.

Exit gate: active source names describe responsibility; no production `lt`,
`body`, `classic`, or numbered record-file naming remains.

### Phase 6 — documentation and final validation

1. Rewrite `docs/README.md`, `overview.md`, `data-flow.md`, `build.md`, and the
   AST module page for the sole visitor-based engine.
2. Delete or archive stale `indexing-engines.md`, `modules/clangx.md`,
   `modules/clangx_lt.md`, and `modules/astcache.md` content.
3. Update `AGENTS.md`, `CLAUDE.md`, build/test skills, and diagrams to remove
   Python/libclang byte-parity claims.
4. Document visitor responsibilities, allowed `Traverse*` exceptions, record
   ordering, and the canonical template argument mapping.
5. Regenerate the committed `index.db` after all source and semantic changes.

Exit gate: documentation matches the filesystem and runtime; all required
checks pass; the refreshed index schema matches both schema declarations.

## Test strategy

### Focused visitor fixtures

- Function-template and method-template local provenance.
- Explicit and implicit receiver expressions.
- Parentheses, casts, cleanups, temporaries, and meaningful unary operators.
- Direct, member, overloaded, unresolved, dependent, and operator calls.
- Range-for with C++20 init statement.
- Namespace aliases, nested qualifiers, using directives, and using
  declarations.
- Class/function/method template arguments covering all Clang argument kinds.
- Partial/explicit specializations and instantiated members.
- Duplicate declaration/reference paths that previously required traversal
  suppression.

### Required gates per phase

1. Build the affected targets.
2. Run the focused test executable(s).
3. Run `ctest --test-dir build -L default --output-on-failure`.
4. Run `ctest --test-dir build -L clang --output-on-failure` for every AST
   phase.
5. Compare normalized Layer-0 row sets against the Phase-0 baseline and review
   every intentional delta.
6. Run storage migration and old-database tests if the schema changes.
7. Reindex cidx itself, run `resolve`, verify zero pending project source files,
   and check `meta.schema_version`.

Python indexing parity is not a gate. Python storage/read-query tests are only
required when storage schema or reader semantics change.

## Review and commit strategy

- One phase per branch or reviewable commit series.
- Do not combine mechanical renames with semantic corrections.
- Every semantic correction includes a failing fixture first.
- Review diffs by responsibility: traversal, typed extraction, encoding,
  orchestration, naming, documentation.
- Do not preserve a known bug solely to keep the old golden file unchanged.
- Update the golden file only after the semantic delta is reviewed and named in
  the commit message.
- Regenerate `index.db` in the same commit as the final source/semantic change
  that affects its contents.

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| RAV order changes database insertion order | Compare normalized row sets; sort at the persistence boundary where order must be stable |
| Removing traversal suppression introduces duplicate facts | Deduplicate by semantic record key and add duplicate-path fixtures |
| Typed Clang APIs differ between LLVM 21 and 22 | Keep differences in `clang_compat.hpp`; validate macOS LLVM 22 and RHEL LLVM 21 |
| Correcting provenance changes downstream devirtualization results | Add query-level receiver/call-argument assertions before accepting deltas |
| Template `arg_kind` correction changes stored meaning | Make the mapping explicit; schema/version/reindex if required |
| Large rename obscures semantic changes | Land structural, semantic, and naming phases separately |
| Documentation becomes stale again | Make the AST module page and design rules part of the final review checklist |

## Success criteria

- The only traversal mechanisms in production indexing are direct RAV visitors
  and narrowly justified base-delegating `Traverse*` overrides.
- No custom statement-child recursion, generic parent stack, fixed-depth peel
  loop, or C++ syntax reparsing remains.
- `StatementEdgeVisitor` owns its callbacks directly; all visitor mixins are
  deleted.
- Function-template locals are classified as local in both receiver and
  argument provenance.
- Every template argument path uses one encoder and one `arg_kind` contract.
- No unjustified `src/ast/` method exceeds the complexity limits.
- Active names describe semantic responsibility and no longer imply a second
  LibTooling backend.
- Project documentation describes the actual files, runtime, test gates, and
  C++-only indexing contract.
- Default and Clang suites pass, intentional semantic deltas are reviewed, and
  `index.db` is regenerated and schema-verified.

## Implementation review — 2026-07-14

**Outcome: changes requested. Do not merge until the two blocking semantic
findings below are fixed and covered by regression tests.**

### Blocking findings

1. **Template type arguments can lose their semantic `ref_id`.**
   `TemplateArgumentEncoder::encode()` calls `getAsTagDecl()` directly on the
   outer `QualType`. Pointer and reference arguments therefore do not resolve
   their underlying record declaration. A reproduced `Box<Foo *>` row retained
   `literal = "Foo *"` but stored a NULL `ref_id`; the removed resolver linked
   that argument to `Foo`. Resolve pointer/reference wrappers with typed Clang
   APIs before looking up the declaration, and add pointer/reference fixtures.
   Location: `src/ast/template_argument_encoder.cpp`.

2. **Construction edges can be assigned the wrong copy/move kind.**
   `StatementEdgeVisitor::emit_construction_form()` searches the rendered
   parameter type for `&` or `&&`. This examines nested type spelling, not the
   constructor category. A reproduced constructor taking
   `Holder<void(int &)>` by value emitted `construct-copy`. Use
   `CXXConstructorDecl::isCopyConstructor()` and `isMoveConstructor()` and add
   a non-copy single-argument constructor fixture.
   Location: `src/ast/statement_edge_visitor.cpp`.

### Repository cleanup

- Remove unrelated local Obsidian state and empty `Untitled*` files from the
  branch before merge. These files are not part of the refactoring.
- Fix the trailing whitespace and extra end-of-file blank lines reported by
  `git diff --check` in `CMakeLists.txt` and this document.

### Verification performed

- C++ default gate: **15/15 passed**.
- Clang gate: **6/6 passed**.
- Python storage/read-query suite: **998 passed, 2 skipped**.
- AST complexity gate: **passed**.
- Refreshed semantic index: schema v29, 132 indexed files, zero missing,
  5 diagnostics, and 2,059 unresolved stubs.
- Normalized Layer-0 output from the refreshed index matched the committed
  index.
- LLVM 21/RHEL validation was not run during this review.

The green suites do not cover the two reproduced semantic failures above, so
they are not sufficient for approval.

### Resolution — 2026-07-14

Both blocking findings are fixed, each with a regression fixture that failed
before the fix (`tests/ast_visitor_test.cpp`):

1. `TemplateArgumentEncoder::encode()` resolves the referenced record through
   `record_usr_of_type()`, which strips pointer/reference wrappers typedly, so
   `Box<Foo *>` / `Box<Foo &>` link `ref_id` to `Foo` while keeping the
   written literal. Fixture: "pointer and reference type args keep their
   ref_id".
2. `StatementEdgeVisitor::emit_construction_form()` classifies by constructor
   category (`isMoveConstructor()` / `isCopyConstructor()`), not by searching
   the printed parameter type. A by-value `Holder<void(int &)>` parameter now
   emits construct-value; real copy/move constructors still emit 13/14.
   Fixture: "construction form uses the constructor category".

Repository cleanup applied in the same change: Obsidian/agent state and empty
`Untitled*` files untracked and gitignored; `git diff --check` whitespace in
`CMakeLists.txt` and this document fixed. Manifests Layer-0 row set is
unchanged by the fixes (the corpus contains neither pattern); the committed
`index.db` was re-selfindexed and gains the corrected pointer/reference
`ref_id` links.

## Source anchors

- `src/ast/body_visitor.hpp` — mixin-composed RAV and traversal overrides.
- `src/ast/body_emit_context.*` — parallel traversal/context state.
- `src/ast/*_visitor_mixin.*` — forwarding visitor hierarchy.
- `src/ast/value_source.cpp` — manual expression peeling and compatibility
  provenance.
- `src/ast/call_template_args.cpp`, `instance_minter.cpp`,
  `instantiation_edges.cpp`, `edge_visitor.cpp` — repeated template encoding.
- `src/ast/lt_engine.cpp` — large orchestration methods and stale backend name.
- `docs/README.md`, `docs/overview.md`, `docs/indexing-engines.md`,
  `docs/modules/clangx*.md`, `docs/build.md` — stale two-engine documentation.
- `tests/index_golden_test.cpp` and `tests/golden/index_layer0.txt` — current
  behavior baseline.
