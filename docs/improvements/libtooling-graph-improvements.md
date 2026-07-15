# LibTooling graph extraction roadmap

Status: canonical recommendation document
Last reviewed: 2026-07-15
Scope: C++23 LibTooling indexer and the SQLite semantic graph

## Executive recommendation

The LibTooling cutover is complete. The indexer already captures a strong
declaration, call, template, construction, and design-entity graph. The next
work should improve semantic query power without turning `index.db` into a
serialized compiler AST.

Recommended product order:

1. Add first-class parameter, type, concept, and constraint facts.
2. Add source-occurrence roles: read, write, address-taken, dynamic, and
   implicit.
3. Add a separate preprocessor/file graph for includes, macros, conditional
   compilation, and modules.
4. Add callable binding and lambda-capture facts to resolve indirect calls.
5. Pilot CFG and data-flow analysis as an opt-in `cidx analyze` capability.
6. Add selected attributes and modern C++ facts only for named queries.
7. Keep Clang cross-TU AST import deferred until a body-sensitive analysis
   proves that the persisted USR graph is insufficient.

The Clang Index API should be introduced early as a fixture-level completeness
oracle. It should not replace the direct `RecursiveASTVisitor` extraction path.

## Current baseline

Snapshot from the refreshed repository self-index on 2026-07-15:

| Metric | Value |
|---|---:|
| Schema version | 29 |
| Indexed files | 132 |
| Symbols | 6,976 |
| Low-level edges | 38,149 |
| Materialized entity nodes | 743 |
| Materialized entity edges | 2,698 |
| Template parameters | 152 |
| Template arguments | 2,177 |
| Edge sites | 60,216 |
| Call arguments | 32,364 |
| Declaration sites | 5,245 |
| Definitions | 1,832 |
| Per-definition edges | 25,146 |

The four parse diagnostics in this snapshot are warnings from external
Souffle headers, not incomplete project translation units.

### Current symbols

The stored symbol vocabulary covers:

- structs, classes, unions, enums, and enum constants;
- fields/members and variables;
- functions, methods, constructors, and destructors;
- function and class templates;
- namespaces, typedefs, and type aliases.

Each symbol can carry USR identity, spelling, qualified/display name, a type
string, declaration and definition locations, full source extent, linkage,
access, semantic parent, and pure/static/instantiation flags.

The schema seeds a macro kind, but normal AST extraction currently produces no
macro symbols. Parameters, concepts, general type expressions, modules,
namespace aliases, using declarations, and deduction guides are not
first-class graph nodes.

### Current low-level relations

The 18 stored edge kinds are:

- calls;
- inherits;
- contains;
- specializes and instantiates;
- overrides;
- uses;
- field-of and method-of;
- value, temporary, heap, copy, move, and factory construction;
- destroy;
- friend;
- dispatch-calls.

Call and use facts are not just endpoint pairs. `edge_site` records location,
conditional status, argument signature, and receiver provenance. `call_arg`
records argument position and value-source, type, declaration, callee, and
value-semantics provenance. Definitions have their own edge snapshots.

### Current materialized entity relations

`cidx resolve` projects the raw symbol graph into higher-level relations:

- generalizes and implements;
- specializes and instantiates;
- composes, aggregates, and associates;
- creates, uses, and destroys;
- befriends;
- declares.

These materialized relations should remain the cheap query substrate for
design-level and future concept/relation queries.

## Design principles

### Store semantic facts, not compiler structure

LibTooling exposes implicit nodes, instantiated bodies, expressions, source
tokens, and the entire Clang type system. Availability alone is not a reason
to persist a fact. A stored fact must have:

- a named query or analysis consumer;
- deterministic identity;
- explicit source and build-configuration provenance;
- bounded cardinality;
- defined unresolved/incomplete semantics;
- focused positive and negative tests.

Do not persist every AST node, token, implicit conversion, or system-header
template instantiation in the normal index.

### Keep graph domains separate

The current graph has several domains with different identity and lifecycle
rules. New storage should respect those boundaries:

- declarations and named entities;
- normalized type expressions;
- source occurrences;
- files and preprocessing events;
- per-definition control flow;
- materialized design entities.

Do not overload the hot `symbol`/`edge` tables with facts whose natural key is
a source site, compilation configuration, CFG block, or macro expansion.

### Preserve uncertainty

Translation-unit indexing is not whole-program proof. Unknown targets,
incomplete hierarchies, missing bodies, unresolved types, and open-world
dispatch must remain explicit. New analyses should use conservative top or
partial states rather than silently presenting guesses as exact relations.

## Priority 0: semantic declarations, types, and constraints

This is the highest-value product change. Today, parameter and return types
mostly appear as strings and coarse `uses` edges. Making signatures traversable
would directly support queries such as:

- which callables accept or return `T`;
- where `T` is used by value, pointer, reference, array, or wrapper;
- which aliases lead to a canonical type;
- which templates are constrained by a concept;
- which declarations are affected by a type or constraint change.

### Parameter facts

Represent parameters with a stable natural key even when Clang cannot produce
a useful USR:

```text
parameter(owner_symbol_id, position, usr?, name?, file_id?, line?, col?)
```

Required relations:

- callable `has_param` parameter;
- parameter `of_type` normalized type;
- callable `returns` normalized type.

Owner plus position is the canonical identity. A parameter USR, spelling, and
source site are optional attributes, not the primary key.

### Type facts

Create a dedicated normalized type domain. A raw printed spelling is not a
sufficient identity: aliases, qualifiers, pointer/reference layers, arrays,
function types, and template specializations need structural representation.

The type identity should be a deterministic encoding of the Clang type shape,
including the distinctions the graph promises to query. At minimum:

- builtin and named record/enum types;
- typedef/alias sugar and canonical target;
- const/volatile/restrict qualifiers;
- pointer, lvalue-reference, and rvalue-reference layers;
- arrays;
- function return/parameter types;
- template specialization plus arguments.

Recommended relations:

- `of_type`;
- `returns`;
- `pointee` or `refers_to`;
- `element_type`;
- `alias_of`;
- `underlying_type`;
- `template_argument_type`.

Preserve both as-written/sugared and canonical identities when they answer
different questions. Do not key a type by an AST pointer or other process-local
identity.

### Concepts and constraints

Index `ConceptDecl` as a named symbol with its USR. Capture:

- template/function/class `constrained_by` concept;
- concept `requires` concept or declaration;
- constraint-reference source sites;
- requires-clause and requires-expression extents;
- whether a constraint is written directly or inherited through another
  constrained declaration.

Do not attempt to persist Clang's complete constraint-satisfaction machinery
in the first phase. Start with the declared dependency graph and source
provenance.

### Additional declaration relationships

Add when covered by the same visitor work:

- namespace aliases and `aliases`;
- using declarations/directives with `uses_declaration` and `uses_namespace`;
- enum `enumerator_of` edges if consumers need a semantic edge distinct from
  lexical containment;
- conversion functions and deduction guides as explicit sub-kinds.

### Delivery gate

This phase requires a schema bump, migration, old-database tests, C++ fixtures,
query API coverage, and a refreshed committed `index.db`. The Python tree needs
storage/read-query compatibility only; do not extend Python extraction.

## Priority 0: occurrence roles

The current `uses` edge answers dependency questions but collapses important
site semantics. Add a source-occurrence table with a role bitset:

```text
occurrence(
  symbol_id,
  file_id,
  line,
  col,
  role_mask,
  context_symbol_id?,
  definition_id?,
  is_implicit,
  PRIMARY KEY(symbol_id, file_id, line, col, role_mask, context_symbol_id)
)
```

Initial roles:

- declaration;
- definition;
- reference;
- read;
- write;
- call;
- dynamic call;
- address-taken;
- implicit;
- undefinition.

This unlocks mutation queries, reference classification, callback discovery,
and later def-use/data-flow summaries without multiplying coarse edge kinds.

### Extraction strategy

Keep direct RAV visitors authoritative for cidx's detailed call, construction,
receiver, and argument-provenance semantics. Use typed expression analysis for
roles where cidx needs exact control. The Clang Index API's `SymbolRole` stream
can supplement coverage and serve as a regression oracle.

Do not replace the production visitor with `IndexDataConsumer`: its relation
model is coarser and its traversal/order should not become a public cidx
contract.

## Priority 1: preprocessor and file graph

Use `clang::PPCallbacks` to build a separate configuration-aware graph.

### File dependencies

Recommended fact:

```text
file_edge(
  src_file_id,
  dst_file_id?,
  kind,
  line,
  col,
  written_name,
  is_angled,
  compile_config_id
)
```

Initial kind: `includes`. Later kinds may cover import/module relationships.
Retain unresolved written includes rather than dropping them when no destination
file is found.

This enables:

- reverse-header impact;
- include chains and cycles;
- direct versus transitive dependencies;
- unused/direct-include analysis;
- configuration-specific include differences.

### Macro facts

Capture:

- macro definitions with stable identity;
- expansion sites and definition-to-expansion relations;
- `#undef` events;
- arguments when needed for a named query;
- whether a symbol or edge site originates in macro spelling or expansion.

Use `clang::index::generateUSRForMacro` only as part of this complete identity
model. Do not add isolated macro symbols without definitions, expansions, and
configuration provenance.

### Conditional compilation

Capture evaluated `#if`/`#ifdef`/`#ifndef` branches and skipped ranges. Every
fact must be keyed by a compilation-configuration identity derived from the
effective command, driver, target, language mode, include paths, and macro
state. The same source file can have different active graphs in different TUs.

### Modules

Record explicit module imports and module identity once cidx has a defined
file/module ownership model. Do not flatten module imports into ordinary file
includes when the semantic distinction matters.

### Cardinality control

Headers are seen from many translation units. Deduplicate configuration-
independent definition facts while keeping TU/configuration-specific include,
expansion, and conditional facts. Benchmark header-heavy corpora before making
the preprocessor graph default.

## Priority 1: callable bindings and lambda captures

The existing call graph is strong for direct and virtual calls but cannot fully
resolve higher-order call flow. Add the facts needed for a conservative
callable points-to analysis.

Recommended relations:

- holder `binds` callable target;
- lambda `captures` declaration, with by-value/by-reference/init-capture mode;
- expression `address_taken` callable;
- indirect call site `invokes` holder;
- analysis result `possible_call` concrete target with partial/top status.

Initial coverage order:

1. raw C/C++ function pointers;
2. callable parameters passed across direct calls;
3. lambdas and captures;
4. `std::function`;
5. `std::bind` and member-function pointers;
6. closed-world cross-TU propagation as an explicit opt-in mode.

Unknown or reassigned holders must retain the original mechanism edge and a
top/partial marker. Indirect resolution may add candidates; it must not remove
sound fallback behavior unless the analysis proves exclusivity.

## Test-only foundation: Clang Index API oracle

Before or alongside the first product schema phase, add fixture-level
comparison against `clang::index`.

The oracle should compare declaration/reference occurrence sets and report
what cidx omits or intentionally models differently. It should not require
identical traversal order and should not treat the Index API's coarser edges as
the cidx schema.

Useful upstream classifications include:

- parameter, concept, module, namespace-alias, using, macro, and include kinds;
- copy/move constructor and conversion-function sub-kinds;
- template, specialization, local, and unit-test properties;
- declaration, definition, read, write, call, dynamic, address-of, and implicit
  occurrence roles.

Persist a new upstream classification only after a concrete query needs it.
Every persisted field becomes a schema compatibility commitment.

## Priority 2: CFG and data-flow analysis

Do not build or persist a CFG during normal indexing yet. Introduce an opt-in
`cidx analyze` path and measure it before promotion.

### First CFG pilot

Build `clang::CFG` per selected definition and discard the Clang object after
producing bounded results. Start with:

- block count;
- cyclomatic complexity;
- maximum loop depth;
- call-site conditional/in-loop classification;
- explicit early-return/throw summaries;
- implicit destructor events, stored separately from source-level calls.

Do not initially persist all basic blocks, statement nodes, dominance trees,
or generic unreachable-code facts in `index.db`.

### Optional CFG artifacts

If path queries prove valuable, use a per-definition artifact or dedicated
tables:

```text
cfg_block(definition_id, local_block_id, flags, terminator_kind, source_extent)
cfg_edge(definition_id, src_local_id, dst_local_id, branch_kind)
```

Clang block IDs are local to one constructed CFG and are not stable global
identities. The stored identity must therefore include the definition and a
deterministic local numbering/normalization contract.

### Data-flow summaries

Add only analyses with explicit lattices, soundness rules, and query consumers.
Candidates include:

- definite initialization;
- liveness and dead stores;
- local def-use;
- output-parameter classification;
- value/ownership state;
- taint propagation;
- must-call relations.

`must-call` is especially easy to overstate. Its contract must define
exceptions, non-termination, indirect/unknown calls, and open-world behavior
before any result is stored.

## Priority 2: attributes and modern C++ semantics

### Attributes

Use a compact fact table rather than one column per attribute:

```text
symbol_attribute(
  symbol_id,
  kind,
  spelling,
  file_id?,
  line?,
  col?,
  is_implicit,
  is_inherited,
  payload?
)
```

Prioritize attributes that answer concrete queries, for example:

- `deprecated` and `nodiscard`;
- visibility/export/import;
- calling convention;
- `nonnull`, ownership, and lifetime annotations;
- framework/test annotations;
- `noinline`, `always_inline`, or target attributes when build analysis needs
  them.

### Other modern C++ facts

Potential later additions:

- exception specifications and computed `noexcept`;
- casts and implicit-conversion summaries;
- coroutine function, `co_await`, `co_yield`, and `co_return` relations;
- structured bindings;
- deduction guides and CTAD;
- constexpr evaluation results;
- object layout, size, alignment, and field offsets.

Each family needs an agreed identity and query contract. ABI/layout facts must
also include target and compilation-configuration identity.

## Deferred: Clang cross-TU AST import

Do not adopt `clang::cross_tu` for normal indexing. The existing USR-keyed
database already solves cross-TU identity and relationship queries without
loading foreign ASTs into one `ASTContext`.

Reconsider cross-TU import only when a named body-sensitive rule cannot be
implemented from stored edges, definitions, CFG summaries, and bounded query-
time composition. Candidate future rules are cross-TU purity, must-call, or
side-effect summaries.

If adopted later, it must be:

- an explicit `cidx analyze --ctu` prepass;
- limited to bodies requested by the active rule;
- bounded by import count, disk, wall-time, and RSS budgets;
- accompanied by AST import diagnostics and incomplete/top results;
- measured on a large corpus before general use.

## Delivery sequence

### Stage 0: completeness oracle

- Add a fixture-only Clang Index API comparison.
- Document intentional differences.
- No schema or product behavior change.

### Stage 1: signature and type graph

**Status: DELIVERED 2026-07-15 (schema v30) — see
[signature-type-tier.md](signature-type-tier.md).**

- Add parameter and normalized type storage.
- Add return/parameter/type/alias/underlying-type relations.
- Extend public query APIs and entity consumers.
- Measure row growth and indexing cost.

### Stage 2: concepts and occurrence roles

- Add concept/constraint nodes and edges.
- Add the occurrence-role table.
- Cover reads, writes, address-taken, implicit, and dynamic sites.

### Stage 3: preprocessor/file graph

- Define compilation-configuration identity.
- Add includes, macro definitions/expansions, conditional regions, and module
  imports.
- Validate header deduplication and reverse-impact queries.

### Stage 4: higher-order calls

- Add bindings, captures, holder provenance, and conservative indirect targets.
- Implement the function-pointer path first.
- Preserve unknown/top fallback behavior.

### Stage 5: optional analysis

- Add the CFG metrics/site-context pilot behind `cidx analyze`.
- Benchmark wall time and peak RSS.
- Promote only the summaries whose cost and semantics are acceptable.

### Stage 6: selected semantic enrichments

- Add attributes and modern C++ families one named query at a time.
- Revisit cross-TU import only if an accepted rule requires foreign bodies.

## Required acceptance gates

Every production extraction change must satisfy the applicable gates below.

### Semantic correctness

- Focused real-Clang fixtures for positive, negative, unresolved, and duplicate
  cases.
- Exact source-site and identity assertions.
- Query-level tests demonstrating the intended consumer.
- No duplicate semantic fact when the same header or instantiation appears in
  multiple translation units.
- Explicit open-world/partial behavior.

### Storage and compatibility

- Bump the schema version in `src/storage/storage.cpp` and
  `python/indexer/storage.py` together for every schema change.
- Provide migration and old-database tests.
- Keep Python storage/read-query parity; do not extend Python indexing.
- Preserve deterministic text/JSON output and ordering.
- Reindex and resolve the committed `index.db` in the same change.
- Verify `index.db`'s `meta.schema_version` equals the source schema version.

### Build and platform

- Focused AST/index tests.
- Default C++ test label.
- Clang/LibTooling test label.
- Python storage/query tests when the read model changes.
- macOS LLVM 22 and RHEL/Rocky LLVM 21 validation for new Clang API use.
- Keep LLVM-version compatibility shims isolated.

### Performance

- Record added rows by table and edge/node family.
- Measure cold indexing wall time and peak RSS on a representative corpus.
- Measure header-heavy duplication.
- Keep CFG, data-flow, and CTU work opt-in until measured evidence supports a
  default-on decision.

## Explicit non-goals

- Reintroducing libclang or extending the retired Python indexer.
- Serializing the complete Clang AST into `index.db`.
- Persisting every expression, token, or implicit system-header node.
- Treating one translation unit as whole-program truth.
- Replacing the direct RAV extractor with the coarser Clang Index API.
- Enabling CFG, data-flow, or cross-TU import by default without corpus
  measurements.
- Adding a fact without a query, analysis, or materialization consumer.

## Related implementation notes

This document is the single roadmap and recommendation source. The following
files retain focused background and implementation detail:

- [signature-type-tier.md](signature-type-tier.md) — Stage 1 (schema v30)
  implementation record: parameter/type_node/type_edge/symbol_type
- [index-api-symbol-enrichment.md](index-api-symbol-enrichment.md)
- [cfg-analysis.md](cfg-analysis.md)
- [crosstu-analysis.md](crosstu-analysis.md)
- [template-arg-contract.md](template-arg-contract.md)
- [fix-spec.md](fix-spec.md)
- [refactoring.md](refactoring.md)

When a focused note conflicts with this roadmap, this document is authoritative
unless a newer accepted design explicitly supersedes it.
