# CIDX Graph Search and Navigation Skill

Status: draft design

This document specifies a high-level graph search and navigation skill for
finding and exploring C++ structures in the CIDX semantic graph. Its
controlled-English language is the primary user interface, but the skill is
larger than the language: it defines seed discovery, relationship navigation,
result shaping, evidence, and execution policy.

It is intentionally not a textual form of the existing Python/C++ `QueryPlan`
API. The existing QueryPlan DSL remains a lower-level programmatic escape hatch;
this skill is the user-facing search and exploration surface.

## Design contract

The compilation pipeline is:

```text
Controlled English request → typed Skill IR → logical Query IR → backend adapter
```

ANTLR4 (or an equivalent grammar tool) defines the syntax. It does not define
the complete language contract. The following are also versioned contracts:

- typed Search IR;
- semantic applicability rules for C++ traits;
- name and operator resolution;
- inheritance depth and direction;
- quantifier semantics;
- completeness and unknown handling;
- canonical formatting and stable errors;
- deterministic result and provenance rules;
- backend capability negotiation.

The backend must never need to understand the controlled-English syntax.

## Search and navigation are one skill

Graph work has two user-visible phases:

1. **Search** finds a typed set of seed entities using traits, members,
   inheritance constraints, quantifiers, and aggregations.
2. **Navigation** expands or inspects those seeds through named semantic
   relationships and returns the related entities with their origin.

Navigation is expressed with concepts such as `direct children`, `ancestors`,
`methods`, `fields`, `callers`, and `callees`. Users do not write `nodes`,
`out`, `in`, raw edge kinds, or arbitrary path expressions.

Example:

```text
Find all abstract classes
Where must have a public pure virtual method "authorize"
Navigate to their direct children
Include methods with public
Select class.name, child.name, method.name, component
```

Another example:

```text
Find all functions named "normalize"
Navigate to their callers up to 3 levels
Select function.qualified_name, caller.qualified_name, caller.component
```

The skill compiler resolves these phrases to bounded semantic navigation. It
does not expose the graph storage model as part of the public contract.

### Navigation relations

The navigation vocabulary is semantic and directional:

| Surface phrase | Meaning | Default depth |
|---|---|---:|
| `direct children` | immediate derived classes | 1 |
| `children` / `descendants` | derived classes | 1..N with explicit bound |
| `direct parents` | immediate base classes | 1 |
| `ancestors` | base classes | 1..N with explicit bound |
| `methods` | ordinary methods of each origin | 1 |
| `data members` / `fields` | data members of each origin | 1 |
| `callers` | functions calling each origin | 1..N with explicit bound |
| `callees` | functions called by each origin | 1..N with explicit bound |

The semantic relation catalog is the authority for source domains, target
domains, direction, depth, completeness, and default bindings:

| Relation | Allowed source kinds | Target set | Direction | Depth | Completeness partition | Default binding |
|---|---|---|---|---:|---|---|
| `DirectChildren` | class, struct, record | class, struct, record | base → derived | exactly 1 | inheritance | `child` |
| `Children` | class, struct, record | class, struct, record | base → derived | 1..N | inheritance | `child` |
| `DirectParents` | class, struct, record | class, struct, record | derived → base | exactly 1 | inheritance | `parent` |
| `Ancestors` | class, struct, record | class, struct, record | derived → base | 1..N | inheritance | `ancestor` |
| `DeclaredMethods` | class, struct, record | ordinary methods | owner → declaration | exactly 1 | member declarations | `method` |
| `DeclaredDataMembers` | class, struct, record | data members | owner → declaration | exactly 1 | member declarations | `data_member` |
| `ComponentRecords` | component | class, struct, record | component → contained record | exactly 1 | component containment | `record` |
| `ComponentMethods` | component | ordinary methods | component → directly contained declaration | exactly 1 | component containment + member declarations | `method` |
| `Callers` | callable | callable | callee → caller | 1..N | call relations | `caller` |
| `Callees` | callable | callable | caller → callee | 1..N | call relations | `callee` |
| `ImplementedInterfaces` | class, struct | interface-classified class, struct | implementor → interface | 1..N | public bases + interface classification | `interface` |

`callable` is the target set `{Function, Method, Constructor, Destructor,
Operator}`. In this catalog, `record` is shorthand for the target set
`{Class, Struct}`; it is not a declaration kind. A `TargetSet` is a finite set
of `TargetKind` values plus the
relation's semantic predicate; it is used wherever a relation can return more
than one target kind. `methods` resolves to `DeclaredMethods` for a record and
to `ComponentMethods` for a component. `count(methods)` on a component
therefore counts ordinary methods directly declared by records contained in
that component, with no inheritance or call closure. `count(classes)` on a
component uses `ComponentRecords` filtered to exact C++ class records; use
`count(records)` for the class-like union.

All direct and member relations normalize to depth `[1, 1]`. Supplying any
other depth is a semantic error `E_DEPTH`; transitive relations require an
explicit finite upper bound and cannot exceed 32.

Call traversal uses cycle-safe set semantics. `Callers` and `Callees` never
return the origin callable itself, even when a self-recursive or mutually
recursive cycle reaches it at depth 1 or greater. A callable already visited
for the same origin is not emitted again, but traversal may continue through
unvisited callables until the requested bound. This policy is shared by all
profiles and is distinct from evidence, which may still show the recursive
call edge.

The cycle fixture is normative:

```text
CallCycleFixture {
  edges: self -> self, a -> b, b -> a,
  queries: {
    callees(self, up to 32 levels): [],
    callers(self, up to 32 levels): [],
    callees(a, up to 32 levels): [b],
    callers(a, up to 32 levels): [b]
  },
  excluded_target: origin
}
```

The self-loop cases must return empty target sets in both directions. The
mutual-recursion cases return `[b]` for origin `a` and exclude `a`. Evidence
may retain recursive edges, but result identities are set-deduplicated.

Every multi-hop navigation must have a finite upper bound. Navigation keeps
the origin binding so that a result can answer both “what was found?” and “why
is this row related to the seed?” without exposing a raw path object.
The language-wide maximum traversal depth is 32 for inheritance and navigation;
backend configuration cannot change query validity or increase this maximum.

Navigation operations have distinct semantics:

- `Navigate to` changes the active result binding to the related target and
  retains the source as its origin binding; seeds without a related target are
  not returned.
- `Include` retains the active source binding and adds the related binding for
  filtering and projection; it emits one result per matching origin/related
  pair and emits no standalone source row when the related set is empty.
- `Show` is a left-outer pair expansion: it retains the active source binding,
  emits one row per related entity, and emits one row with null related fields
  when the related set is empty. It does not filter the source set and never
  creates a raw path or a storage-level edge result.

Navigation steps compose as relational joins on the retained origin and
bindings. Sequential `Include` steps produce the Cartesian product of their
matching related bindings for each origin; a missing inner side removes that
origin. Sequential `Show` steps produce the corresponding left-outer product,
including one null row for each empty side. An `Include` followed by `Show`
first applies the inner filter and then expands the surviving rows outward.
Ambiguous flat combinations are therefore not rejected or grouped: their
relational multiplicity is explicit and is normalized by `Distinct` only when
the query requests a distinct projection.

### Bindings and member scope

The binding catalog covers every target and relation domain:

| Binding role | Canonical binding names |
|---|---|
| seed targets | `class`, `record`, `struct`, `union`, `method`, `function`, `constructor`, `destructor`, `operator`, `data_member`, `field`, `component`, `file`, `specialization`, `instantiation` |
| inheritance | `child`, `parent`, `ancestor` |
| implementation | `interface` |
| members | `method`, `data_member` |
| calls | `caller`, `callee` |
| explicit aliases | any non-keyword identifier introduced with `AS` |

The `Find` target creates the corresponding seed binding. Navigation creates
the default binding from the relation catalog, and `AS name` overrides it in
the canonical syntax. Singular names are canonical; plural keyword spellings
are accepted as binding aliases and formatted back to the singular name. A
binding name may be used only once in a query scope; collisions fail with
`E_BINDING_COLLISION` rather than silently shadowing an earlier binding.

Member requirements and member counts refer to directly declared members of
the current owner. To search inherited members, navigate to `ancestors` and
then `Include methods` or `Include data members`; each included member remains
directly declared by its ancestor. There is no implicit visible-member
expansion.

## User-facing examples

```text
Find all abstract classes
Where child of "BaseController"
  And must have a public pure virtual method "authorize"
  And must have a private data member "token"
Select name, component
```

```text
Find all classes
Where descendant of "Widget" up to 8 levels
  And must have any of [
    public method "draw",
    public operator "()"
  ]
  And must not have a private data member "mutex"
Select name, qualified_name, component
```

```text
Find all classes
Where must have a public constructor
  And must have a protected virtual destructor
  And must have a public operator "=="
Select name, component, file, line
```

```text
Find all classes
Where ancestor of "ConcreteController" up to 8 levels
Select name, component
```

```text
Find all classes
Where at least 2 public methods
  And at most 1 private data member
  And not child of "DeprecatedBase"
Select name, count(methods) as method_count,
  count(data members) as data_member_count, component
```

```text
Find all classes
Where child of "BaseController"
Navigate to their ancestors up to 8 levels
Select class.name, ancestor.name, ancestor.component
```

Interpolated projections are first-class string projections:

```text
Find all classes
Select "class name is $name at $filename:$lineno" as location
```

```text
Find all records
Include methods AS method
Select "${record.name} -> ${method.qualified_name}" as declaration
```

Hierarchy classification and implementation queries use semantic C++ facts:

```text
Find all classes
Where abstract
  And implements all of [interface "app::IReadable", interface "app::IWritable"] up to 8 levels
  And must have an overriding method
Select name, count(implemented interfaces up to 8 levels) as interface_count
```

```text
Find all classes
Where implements at least 1 interface up to 8 levels
Select name, qualified_name
```

Template specializations and instantiations are searchable entities rather
than opaque name spellings:

```text
Find all template specializations
Where specializes template "app::Box"
  And template arguments are [type "int", non_type "4"]
Select name, qualified_name, kind
```

```text
Find all template instantiations
Where instantiates template "app::Box"
  And explicit template argument 1 is type "int"
Select "${instantiation.qualified_name}:${instantiation.file}:${instantiation.line}"
```

The outer query string and inner signature string are decoded in two explicit
steps:

```text
Find all classes
Where must have a method "quote\"name" signature "\"quote\\\"name\"(\"path\\\\value\") const volatile &&"
Select name
```

The profile and language-version spellings are also full grammar fixtures:

```text
Find all classes
Select name
Using backend cidx-sqlite-v1
Language cxx17
```

```text
Find all structs
Select qualified_name
Using backend cidx-souffle-v1
Language cxx20
```

```text
Find all functions
Select name, signature
Using backend cidx-kuzu-cypher-v1
Language cxx23
```

## Search targets

The target vocabulary is typed. Exact record-kind filters are disjoint; the
explicit `class-like` category is the documented union of class and struct.

| User term | Semantic target |
|---|---|
| `class` / `classes` | C++ class records only |
| `record` / `records` | class-like record: C++ class or struct, excluding union |
| `struct` / `structs` | C++ struct records only |
| `union` / `unions` | C++ union records only |
| `method` / `methods` | ordinary member function; excludes constructors, destructors, and operators |
| `function` / `functions` | non-member, non-operator function |
| `constructor` / `constructors` | constructor declaration |
| `destructor` / `destructors` | destructor declaration |
| `operator` / `operators` | callable with a structured operator identity; storage may still be a function or method |
| `data member` | static or non-static data member |
| `field` / `fields` | friendly alias for `data member` |
| `component` / `components` | CIDX architectural component |
| `file` / `files` | indexed source file |
| `template specialization` / `template specializations` | user-written explicit full or partial specialization; excludes instantiations |
| `template instantiation` / `template instantiations` | implicit instantiation, explicit-instantiation declaration, or explicit-instantiation definition; excludes user-written full/partial specializations |

`class`, `struct`, and `union` targets are exact record-kind filters. The
`record` spelling is the explicit `ClassLike` union of class and struct
records; exact record-kind queries must not be silently widened.

The two template targets are intentionally disjoint query categories even
though the C++ standard sometimes uses *specialization* as an umbrella term.
This is the principled CIDX interpretation of requests to search template
specializations and instantiations: a user-written full/partial specialization
is a `TemplateSpecialization`; an entity produced by implicit or explicit
instantiation is a `TemplateInstantiation`. The entity also retains its
underlying declaration category (`class`, `function`, `variable`, or member)
as `template_entity_kind`; no backend may infer the category from a formatted
name.

Template targets accept no pre-target C++ declaration traits because they are
cross-kind semantic categories; such a trait fails with
`E_TRAIT_APPLICABILITY`. The underlying declaration category remains available
as `template_entity_kind` for filtering in the programmatic IR and projection.
Template relation and argument predicates on a non-template target fail with
`E_RELATION_APPLICABILITY`.

## Traits and modifiers

Traits are typed; they are not one unvalidated modifier list.

### Class traits

```text
abstract
final
```

### Ordinary method traits

```text
public | protected | private
virtual | pure virtual | final | overriding
static
const | volatile
constexpr | consteval
noexcept
deleted
```

`overriding` is a semantic trait: it is true exactly when the declaration has
at least one C++ override relation to a virtual base declaration. The accepted
legacy spelling `override` is an alias and the formatter emits `overriding`.
CIDX does not test whether the source happens to spell the `override`
virt-specifier; a declaration that overrides implicitly matches, and a token
that cannot be validated against an override relation does not.

### Free-function traits

Free functions have no access, virtual, overriding, or member cv traits. Their
applicable traits are `static` when internal linkage is represented,
`constexpr`, `consteval`, `noexcept`, and `deleted`. `defaulted` is valid only
for language-defined defaultable comparison operators, never for an ordinary
free function.

### Constructor traits

```text
public | protected | private
explicit
constexpr | consteval
noexcept
deleted | defaulted
```

Constructors cannot be `virtual`, `static`, `const`, `pure virtual`, or
`final`.

### Destructor traits

```text
public | protected | private
virtual | pure virtual
overriding
constexpr
noexcept
deleted | defaulted
```

Destructors cannot be `static`, `const`, or `consteval`.

### Data-member traits

```text
public | protected | private
static
const | volatile
mutable
constexpr
```

`mutable` applies to non-static data members. `constexpr` applies to static
data members under C++ rules. The validator rejects illegal combinations.

### Operator traits

Operators use the applicable callable traits plus a structured identity:

```text
operator "=="
operator "()"
operator "[]"
conversion operator to "bool"
literal operator "_km"

public | protected | private
virtual | pure virtual | final | overriding
static | const | volatile
constexpr | consteval | noexcept
deleted | defaulted
```

Operators are a semantic search category, not a new required Clang storage
kind. The resolver checks the operator form, memberness, token, and selected
C++ language version.
`defaulted` is valid only for constructors, destructors, and eligible
comparison operators; it is not a general operator or method trait.

The canonical identity is:

```text
OperatorIdentity {
  form: Symbol | Conversion | Literal,
  token_or_target: string,
  memberness: Any | Member | NonMember
}
```

The language version is part of `ResolutionContext`, not the operator's
semantic identity. The resolver validates allocation/deallocation operators,
conversion operators, literal operators, and C++23 static `operator()` and
`operator[]` against that context.

Operator-form validation is normative:

| Form | Canonical payload | Required semantic validation |
|---|---|---|
| `Symbol` | an operator token such as `==`, `()`, `[]`, `new`, `new[]`, `delete`, or `delete[]` | the declaration is a legal overload for that token; allocation/deallocation tokens use their corresponding allocation form |
| `Conversion` | a target type such as `bool` | the declaration is a conversion operator and therefore member-only; ambiguous target-type resolution fails with `E_NAME_AMBIGUOUS` |
| `Literal` | a user-defined suffix such as `_km` | the declaration is a legal namespace-scope literal operator for the selected language version and suffix |

`memberness = Any` means that the surface did not constrain memberness; the
resolver still returns only declarations legal for the selected form.
`static` on `operator()` or `operator[]` is valid only for a member operator in
C++23 or a newer language version. Other illegal form/trait combinations fail with
`E_TRAIT_APPLICABILITY`.

The complete symbol-token catalog is:

```text
new  new[]  delete  delete[]  +  -  *  /  %  ^  &  |  ~  !  =
<  >  +=  -=  *=  /=  %=  ^=  &=  |=  <<  >>  >>=  <<=  ==  !=
<=  >=  <=>  &&  ||  ++  --  ,  ->*  ->  ()  []  co_await
```

`co_await` requires C++20 or newer. The three-way comparison `<=>` requires
C++20 or newer. Static `operator()` and `operator[]` require C++23 or newer.
The non-overloadable punctuation forms `.`, `.*`, `::`, and `?:` are rejected
with `E_TRAIT_APPLICABILITY`; literal operators use the separate `Literal`
form and suffix payload.

```text
ResolutionContext {
  query_language_version: CxxVersion,
  selected_scope: ScopeKey,
  index_schema_version: Integer
}

DeclarationCompilationContext {
  declaration: SemanticIdentity,
  language_version: CxxVersion,
  translation_units: [FileIdentity],
  provenance: [Evidence]
}
```

The canonical spelling of `pure` is `pure virtual`.

### Trait applicability matrix

The following matrix is normative. `✓` is generally applicable, `—` is
invalid, and `C` is conditionally valid only when the declaration and selected
C++ language version permit it.

| Trait | Class | Struct | Union | Method | Function | Constructor | Destructor | Operator | Data member |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| access (`public`/`protected`/`private`) | C | C | C | ✓ | — | ✓ | ✓ | C | ✓ |
| `abstract` | ✓ | ✓ | — | — | — | — | — | — | — |
| `final` | ✓ | ✓ | ✓ | C | — | — | C | C | — |
| `virtual` | — | — | — | ✓ | — | — | ✓ | C | — |
| `pure virtual` | — | — | — | C | — | — | C | C | — |
| `overriding` (`override` alias) | — | — | — | C | — | — | C | C | — |
| `static` | — | — | — | ✓ | C | — | — | C | ✓ |
| `const` / `volatile` | — | — | — | C | — | — | — | C | ✓ |
| `constexpr` | — | — | — | C | C | ✓ | C | C | C |
| `consteval` | — | — | — | C | C | ✓ | — | C | — |
| `explicit` | — | — | — | — | — | ✓ | — | C | — |
| `noexcept` | — | — | — | C | C | ✓ | ✓ | C | — |
| `deleted` | — | — | — | C | C | C | C | C | — |
| `defaulted` | — | — | — | — | — | C | C | C | — |
| `mutable` | — | — | — | — | — | — | — | — | ✓ |

`C` is resolved against memberness, virtual status, special-member rules, and
the selected language version. `Record` is not a declaration kind and has no
independent matrix column: the `record` target dispatches each declaration to
the `Class` or `Struct` column and excludes `Union`. Union `final` is legal but
has no possible base class; union `abstract` is invalid. Operator access is
conditional and valid only for member operators; non-member operators and
namespace-scope literal operators have no access. Class, struct, and union
access is conditional: it is valid for nested records with a containing record
and invalid for top-level records. The semantic validator applies this table
before logical planning and emits `E_TRAIT_APPLICABILITY` for invalid
combinations.

For `overriding`, conditional validity means the declaration is a non-static
virtual ordinary method, destructor, or member operator to which the C++
override rules apply. It is evaluated from the semantic `Overrides` relation,
not tokens. Constructors, static members, and free functions never override.

## Inheritance vocabulary

All inheritance predicates exclude the target itself.

| Surface phrase | Canonical relation | Depth |
|---|---|---:|
| `child of X` | direct subclass of `X` | 1 |
| `descendant of X up to N levels` | subclass of `X` | 1..N |
| `direct parent of X` | direct base of `X` | 1 |
| `ancestor of X up to N levels` | base of `X` | 1..N |

The canonical internal names are `direct_subclass`, `subclass`, `direct_base`,
and `base`. The English forms above are aliases. A finite traversal limit is
required by every backend; the logical IR records the requested closure.

### Interfaces, implementation, and overriding

C++ has no interface declaration kind. CIDX therefore defines `Interface(R)`
as a semantic classification, not a spelling heuristic. It applies only to a
complete class or struct record `R` and is true exactly when:

1. `R` is abstract under the C++ semantic model;
2. `R` has no directly declared non-static data member;
3. every directly declared non-static ordinary method of `R` is pure virtual;
4. every direct base of `R` is itself an interface; and
5. the public-base closure of `R` contains at least one pure virtual ordinary
   method or pure virtual destructor.

Constructors, static members, type aliases, and a non-pure virtual destructor
do not disqualify an interface. A concrete class is never an interface, and an
abstract class is not automatically an interface. Items 2--5 are negative or
universal facts and therefore require complete member/base partitions; if
they cannot be proven, interface classification is `unknown`.

The implementation relations are:

```text
DirectlyImplements(C, I) :=
  C != I and PublicDirectBase(C, I) and Interface(I)

Implements(C, I, [1, N]) :=
  C != I and exists a public-base path C -> ... -> I of length 1..N
  and Interface(I)
```

`C` must be a class or struct and `I` must resolve to one interface class or
struct. A public virtual-base edge is still a public-base edge. Private and
protected inheritance do not establish implementation. Intermediate records
on a transitive path need not be interfaces; this lets a class derived from a
concrete implementation still implement its ancestor interfaces. Direct
implementation is depth `[1,1]`; transitive implementation requires `up to N
levels`, excludes self, is cycle-safe, and uses the language maximum 32.

The controlled-English quantifiers are exact:

```text
directly implements interface "I"                    = one direct relation
implements interface "I" up to N levels             = one transitive relation
implements any of [interface "I", interface "J"] ... = OR over references
implements all of [interface "I", interface "J"] ... = AND over references
implements at least K interface(s) ...               = cardinality >= K
implements at most K interface(s) ...                = cardinality <= K
implements exactly K interface(s) ...                = cardinality = K
```

The same five interval rules used by other counts govern interface
cardinality. Identity is the resolved interface declaration, so diamond paths
do not double-count. Empty `any of`/`all of` lists are syntactically forbidden.
Name ambiguity is resolved before hierarchy evaluation.
The formatter emits `interface` after the integer literal `1` and `interfaces`
after every other literal.

`Overrides(D, B)` is the compiler-semantic C++ relation saying that virtual
member `D` overrides virtual base member `B`, including implicit overrides and
all overridden declarations in multiple-inheritance cases. It is not derived
from the `override` token and does not require that token. `must have an
overriding method` is therefore `Exists(DeclaredMethods where
Exists(Overrides(method, _)))`; it counts ordinary methods only. Destructor
and operator targets may use the same `overriding` trait directly, but they do
not enter a `methods` related set.

Positive implementation and override facts include the base/override edges,
resolved declarations, source locations, and interface-classification facts
in evidence. A witnessed path or override proves existence even when unrelated
partitions are incomplete. Absence, `all of`, upper-bound counts, and interface
classification remain `unknown` until every touched base/member/override
partition is complete. `explain` names each incomplete partition; no backend
may treat a missing override edge or non-public base as an interface match.

### Template specialization and instantiation semantics

The template model keeps declarations and arguments structurally distinct:

- `TemplateParameter` belongs to a primary or partial template declaration and
  describes a declared slot (`type`, `non_type`, `template`, or parameter
  pack). It is never a specialization argument.
- `TemplateArgumentValue` belongs to a specialized or instantiated entity and
  is a value of kind `Type`, `NonType`, `Template`, or `Pack`. A pack contains
  ordered argument values; it is not flattened.
- `TemplateArgumentBinding` adds a zero-based position and origin
  `SpelledExplicit | Deduced | Defaulted | Substituted`. “Explicit template
  argument” means origin `SpelledExplicit`, not “template parameter.”

CIDX exposes these direct semantic relations:

```text
Specializes(S, P)   // S is a user-written full/partial specialization of P
Instantiates(I, P)  // I is an implicit/explicit instantiation of selected pattern P
TemplateOf(E, P)    // normalized union used for identity/projection, not surface syntax
```

`Specializes` applies only to `TemplateSpecialization`; `Instantiates` applies
only to `TemplateInstantiation`. For an instantiation selected from a partial
specialization, `P` is that selected partial specialization and its evidence
also identifies the primary template. These relations are depth one and do
not silently roll member instantiations into their owning class instantiation.

Structured argument predicates compare normalized kind and value at each
position. `template arguments are [...]` compares the complete effective list
and exact arity. `explicit template arguments are [...]` compares only the
ordered `SpelledExplicit` bindings. `template argument N is X` and `explicit
template argument N is X` use one-based surface positions; the latter indexes
the filtered explicit list. `any` is a predicate wildcard and never appears in
stored IR or canonical result values. Type and template values resolve to
semantic identities when possible; non-type values are canonical constant
expressions; unresolved or dependent values retain canonical text plus
`resolution = Unknown`.

Canonical argument serialization is:

```text
type "std::int32_t"
non_type "4"
template "std::allocator"
pack [type "int", non_type "4"]
```

The formatter emits `, ` between arguments, `non_type` with an underscore,
and nested pack brackets exactly as shown. A template entity's canonical
display name is `primary-qualified-name<canonical-argument-list>`; callable
entities additionally use their canonical signature for identity. Semantic
identity is the tuple `(template relation target, template category,
template_entity_kind, canonical arguments, canonical callable signature)` and
never the display string alone.

A template reference must resolve to exactly one primary/partial template in
the required domain. Overloaded function templates require a signature
constraint; otherwise `E_NAME_AMBIGUOUS` lists candidates. Wrong argument
kind, invalid position, non-canonical value, or pack-shape mismatch fails with
`E_TEMPLATE_ARGUMENT`. An argument predicate evaluates `false` only when the
argument list and resolution are complete; otherwise it is `unknown` and
follows the query's unknown policy. Evidence includes the specialization or
instantiation relation, argument origin/value rows, selected pattern, and
completeness partitions.

## Member requirements and quantifiers

Member requirements are evaluated relative to the current target.

```text
must have P       = EXISTS(P)
must not have P   = NOT EXISTS(P)
must have any of  = EXISTS(P1) OR EXISTS(P2) ...
must have all of  = EXISTS(P1) AND EXISTS(P2) ...
```

`must not have` is only a proven negative when the relevant index relation is
complete. Otherwise the result is `unknown` or the query is rejected, according
to the query's completeness policy.

The phrase `all of` means all listed requirements, not universal quantification
over every member. Universal predicates use distinct syntax:

```text
Where every public method is pure virtual
```

`every` is vacuously true for an empty, complete member set. A query that
requires at least one matching member must state that separately, for example:

```text
Where at least 1 public method
  And every public method is pure virtual
```

On an incomplete member set, `every` evaluates to `unknown` unless the
observed facts already prove a violation.

Boolean precedence is:

```text
not > and > or
```

Parentheses are supported.

## Aggregations and cardinality

Aggregations are available both as search constraints and as selected result
fields. The supported aggregate measure is cardinality:

```text
at least N P  = COUNT(P) >= N
at most N P   = COUNT(P) <= N
exactly N P   = COUNT(P) = N
more than N P = COUNT(P) > N
fewer than N P = COUNT(P) < N
```

Every aggregate constraint and projection is built from the same
`RelatedSetSpec(source_binding, relation, target, predicate, depth, identity)`
record. An unqualified related set uses the current binding; a qualified form
such as `count(class.methods) as method_count` supplies `source_binding`
explicitly. This prevents `count(methods)` and `at least 2 methods` from
using different scopes or duplicate policies. The default identity is one
resolved declaration/entity. The controlled-English contract exposes only
`Declaration` and `Entity` identity; `EvidenceSite` is a lower-level
programmatic IR option outside this grammar and backend conformance. Search
language counts never count evidence sites.

For a count interval `[L, U]`, where `U = null` means infinity, comparisons
have these exact truth rules. Let `C` mean `truncated = false`; every rule that
uses an upper bound requires `C` because enumeration truncation makes the upper
bound non-decisive for query truth.

| Comparison | true when | false when | otherwise |
|---|---|---|---|
| `count >= N` | `L >= N` | `C and U != null and U < N` | unknown |
| `count <= N` | `C and U != null and U <= N` | `L > N` | unknown |
| `count = N` | `C and U != null and L = U = N` | `C and U != null and U < N`, or `L > N` | unknown |
| `count > N` | `L > N` | `C and U != null and U <= N` | unknown |
| `count < N` | `C and U != null and U < N` | `L >= N` | unknown |

The interval is valid only when `L >= 0` and either `U = null` or `L <= U`.
The invariant is `exact = (!truncated && U != null && L = U)`. Runtime
enumeration truncation sets `truncated = true`; it may retain an independently
proven finite upper bound, but `exact` remains false while the marker is set. A
lower-bound comparison may still be proven true after truncation; an upper-bound
or exact comparison cannot be proven true from a truncated count.

Examples:

```text
Find all classes
Where at least 2 public methods
  And exactly 1 protected constructor
  And at most 3 data members
Select name, count(methods) as method_count, component
```

```text
Find all components
Where at least 10 classes
Select name, count(classes) as class_count, count(methods) as method_count
```

Aggregate projection syntax is intentionally explicit:

```text
Select name, count(methods) as method_count,
  count(public methods) as public_method_count, component
```

Every aggregate projection has a required unique alias in the query scope and
has type `CountValue`. `ORDER BY` may name that alias or repeat the aggregate
expression; the formatter always emits the alias form. Repeated related-set
expressions therefore remain distinct typed columns rather than colliding on a
generic `count` name.

Related sets can be qualified by an existing binding:

```text
Find all classes
Navigate to their direct children AS child
Select class.name, count(child.methods) as child_method_count
Order by child_method_count descending
```

Aggregates must define their scope, ordering, null handling, and duplicate
policy. CIDX search results use set semantics for entity counts: an overloaded
method is counted once per resolved declaration. Evidence-site counting is
available only through the lower-level programmatic IR, not this language.

The logical IR adds:

```text
Aggregate(RelatedSetSpec, measure=Count)
Having(comparison)
```

SQLite lowers these to `COUNT` plus `HAVING`; Datalog and Cypher backends may
use native aggregates or a host post-processing layer only when the resulting
`CountValue` and truth rules are identical.

## Interpolated string projections

A quoted string in `Select` is an `InterpolatedProjection`, not an opaque
backend expression. Its decoded body consists of literal text and typed field
placeholders:

```text
$name $qualified_name $kind $signature $component
$filename $lineno $column $truth
${name}_suffix ${filename}.bak
${binding.name} ${binding.qualified_name} ...
${binding.file} ${binding.line}
```

`$filename` and `$lineno` are the canonical user-facing placeholder spellings
for unqualified `File` and `Line`. An unqualified placeholder uses `$field`
when the field boundary is clear, such as `$name` or `$filename`. A qualified
placeholder uses `${binding.field}`; braces are also required when a simple
placeholder is immediately followed by identifier characters, such as
`${name}_abstract`. Simple placeholders use `name`, `qualified_name`, `kind`,
`signature`, `component`, `filename`, `lineno`, `column`, or `truth`.
Qualified placeholders use the ordinary projection-field spellings `name`,
`qualified_name`, `kind`, `signature`, `component`, `file`, `line`, `column`,
or `truth`. Thus `$filename` and `${record.file}` both lower to
`FieldRef(File)`; `${record.filename}` is not an alias.
`evidence` is intentionally excluded because an evidence collection has no
lossless scalar string form. Binding and field applicability are checked just
like an ordinary field projection.

The post-string interpolation scanner uses this normative syntax, where
`identifier_continue` is `[A-Za-z0-9_]`:

```text
placeholder := simple_placeholder | braced_placeholder
simple_placeholder := "$" simple_field
  // valid only when the next decoded character is not identifier_continue
braced_placeholder := "${" simple_field "}"
                     | "${" binding "." qualified_field "}"
```

The scanner consumes the longest identifier after an unbraced `$` before
validating it as a field. Consequently `$name_suffix` is one unknown
placeholder, not `$name` plus `_suffix`; write `${name}_suffix`. A dot never
extends a simple placeholder, so `$record.name` is invalid rather than a
qualified reference. Qualified references always use `${binding.field}`.

Interpolation is parsed after the outer `STRING` token is decoded. `$$`
produces one literal dollar sign. A single `$` must begin a valid placeholder;
unknown placeholders, missing braces, malformed field names, and dangling `$`
fail with
`E_INTERPOLATION` at the placeholder span. Quotes and backslashes continue to
use the outer string escapes `\"` and `\\`; interpolation introduces no new
backslash escapes. Literal text is UTF-8 NFC and may contain `<`, `>`, and `.`
without escaping.

The canonical formatter:

1. emits an unqualified field as `$field` when the following literal does not
   begin with `identifier_continue`, otherwise as `${field}`, using
   `filename`/`lineno` for `File`/`Line`;
2. singularizes binding aliases and emits every qualified placeholder as
   `${binding.field}`, using `file`/`line` for `File`/`Line`;
3. emits literal dollars as `$$`;
4. NFC-normalizes literal text and applies only the standard outer quote and
   backslash escapes.

An interpolated projection must contain at least one placeholder. It may have
`AS identifier`. Without an alias, its `ProjectionKey` is the canonical
formatted template body (for example `$filename:$lineno`, without outer quotes
and retaining `$$` for a literal dollar);
with an alias, the alias is the key. That key shares the same row namespace as
field and aggregate keys, so a duplicate fails with `E_PROJECTION`. `ORDER BY`
may use an interpolation alias; an unaliased template body is not an
`Identifier` and cannot be named by `ORDER BY`.

Each placeholder is read as its typed `FieldRef` and converted deterministically:

| Typed input | Interpolated text |
|---|---|
| `String` | unchanged UTF-8 NFC text |
| `Integer` | base-10 digits with `-` only for a negative value |
| `Boolean` | `true` or `false` |
| `TruthValue` | `true`, `false`, or `unknown` |
| `Null` | `<null>` |
| unavailable field with unknown completeness | `<unknown>` |

No locale, path separator, backend collation, or JSON formatting participates.
`Show` null bindings therefore render `<null>`, while an existing binding whose
requested field is unavailable because facts are incomplete renders
`<unknown>` and contributes its completeness diagnostic. Literal text equal to
these markers remains literal; interpolation is display shaping, not a
reversible encoding.

Lowering is backend-neutral:

```text
ProjectFields([FieldRef ...])
→ RenderInterpolation([TextSegment | FieldSegment], ConversionProfileV1)
→ Project(ProjectionKey, String)
```

Rendering occurs after field truth/completeness evaluation and before
`Distinct`, `Sort`, and `Limit`. A built-in adapter may render in its engine or
in the declared host post-layer, but it must consume typed values and produce
the same UTF-8 bytes. It may not pass the source template to SQL, Datalog, or
Cypher string interpolation.

## Negation

Negation has two forms:

```text
must not have P   = proven NOT EXISTS(P)
not P             = logical negation of P
```

`not` may apply to any parenthesized predicate:

```text
Find all classes
Where not child of "DeprecatedBase"
  And not (must have a private data member "password")
Select name, component
```

```text
Find all classes
Where must not have an operator "=="
Select name, component
```

Search uses three-valued logic because an index may be incomplete:

| Input | `not` result |
|---|---|
| `true` | `false` |
| `false` | `true` |
| `unknown` | `unknown` |

Conjunction and disjunction must also preserve `unknown` rather than treating
missing graph facts as false. The query option `on unknown` selects one of
three policies:

- `exclude` (default): return only rows whose truth is `true`;
- `include`: return `true` and `unknown` rows, with a `truth` field and reason;
- `error`: reject any unknown result with `E_INCOMPLETE_FACTS`.

The `error` policy rejects only an actually unknown result. It does not reject
an interval comparison already proven `true` or `false` without an exact
scalar.

Strong Kleene conjunction and disjunction are normative:

| AND | true | false | unknown |
|---|---:|---:|---:|
| **true** | true | false | unknown |
| **false** | false | false | false |
| **unknown** | unknown | false | unknown |

| OR | true | false | unknown |
|---|---:|---:|---:|
| **true** | true | true | true |
| **false** | true | false | unknown |
| **unknown** | true | unknown | unknown |

Negation over an incomplete relation is never silently converted into an
anti-join. The planner may use `AntiSemiJoin` only when the relevant relation
and predicate coverage are complete.

## Names and resolution

C++ names should be quoted in canonical output:

```text
"BaseController"
"app::Controller::authorize"
"operator=="
```

Bare identifiers may be accepted as sugar, but the formatter always emits
quoted strings. Resolution is exact and qualified-name aware. A reference
resolves to a deterministic set of declarations; overloads are not silently
collapsed. A callable reference used where one declaration is required must
include a signature constraint, otherwise compilation fails with
`E_NAME_AMBIGUOUS` and the candidate declarations in `explain` output.

`named "X"` is an exact, case-sensitive C++ name predicate. `name matching
"X*"` is a case-sensitive glob predicate; `name matching regex "..."` is a
case-sensitive regular-expression predicate; `name matching glob "..."` is
the explicit glob spelling. The formatter emits the explicit pattern kind for
non-exact predicates. No case folding is applied to C++ names. These forms
lower to `NameEquals` or `NameMatches` in the typed IR, and every `where`
trait lowers to `TraitTest` rather than being discarded during planning.

Pattern portability is normative. Inputs and indexed names are UTF-8 NFC
strings; invalid UTF-8 or non-NFC index evidence is rejected with `E_PATTERN`.
Glob patterns use only `*` (zero or more Unicode code points), `?` (exactly
one Unicode code point), and `\\` to escape `*`, `?`, or `\\`; character classes
and other glob operators are literal text. Regex patterns use the ECMAScript
Unicode regular-expression profile over Unicode code points, with no
engine-specific extensions, backreferences, or lookbehind. Invalid syntax or
an unsupported construct fails with `E_PATTERN`. Every backend must use this
profile or the declared host matcher, never its native pattern dialect.

The canonical vocabulary is finite and versioned. Only aliases declared in the
grammar/catalog are accepted; unrecognized natural-language synonyms fail with
`E_UNKNOWN_TERM`. This prevents backend and parser behavior from changing as
an uncontrolled synonym list grows.

Operator and trait validation uses the query-selected language version together
with each declaration's `DeclarationCompilationContext`. A declaration is
eligible only when both contexts permit the requested form. The same header
declaration indexed from C++20 and C++23 translation units is compatible when
its canonical target kind, signature, operator identity, and trait
applicability are identical; the versions are merged as provenance and do not
alone cause an error. `E_LANGUAGE_CONFLICT` applies only when the indexed
declarations disagree in semantic classification, canonical signature, or
feature-gated operator legality. The diagnostic reports both contexts and
translation units. A query selecting a version in which an otherwise valid
declaration is not legal simply excludes that declaration with an explainable
non-match; it is not a provenance conflict.

## Diagnostics

Diagnostics are stable, machine-readable, and attached to source spans when
the parser has a location. The required error codes are:

| Code | Meaning |
|---|---|
| `E_PARSE` | syntax does not match the versioned grammar |
| `E_UNKNOWN_TERM` | identifier or synonym is outside the vocabulary catalog |
| `E_NAME_AMBIGUOUS` | a single declaration was required but multiple candidates matched |
| `E_NAME_NOT_FOUND` | a required name reference has no candidate |
| `E_BINDING_COLLISION` | a binding name is created more than once in one query scope |
| `E_LANGUAGE_CONFLICT` | declaration provenance has incompatible language-version contexts |
| `E_PATTERN` | a glob or regex pattern is invalid or non-portable |
| `E_TRAIT_APPLICABILITY` | a trait is invalid for the selected target/declaration |
| `E_RELATION_APPLICABILITY` | an interface, override, specialization, or instantiation relation is invalid for the selected domain |
| `E_INTERPOLATION` | a string projection has an unknown or malformed placeholder, missing/unbalanced braces, an unbraced qualified or boundary-sensitive reference, an invalid binding/field, or an unsupported scalar conversion |
| `E_TEMPLATE_ARGUMENT` | a template argument predicate has an invalid kind, position, arity, value, or pack shape |
| `E_DEPTH_REQUIRED` | a multi-hop navigation omitted its finite upper bound |
| `E_DEPTH` | a depth range is invalid or exceeds the language maximum of 32 |
| `E_PROJECTION` | a selected field/aggregate, projection key, alias, or order reference is invalid or ambiguous |
| `E_INCOMPLETE_FACTS` | `on unknown error` requires facts that are not complete |
| `E_BACKEND_UNSUPPORTED` | the selected adapter cannot implement a logical operator |
| `E_LIMIT` | a limit is non-positive or exceeds the language maximum |
| `E_INTERNAL` | an implementation invariant failed; no partial result is returned |

Diagnostics include the code, message, source span, relevant entity or
relation, and an `explain` payload when name resolution or completeness is
involved. Error text may evolve, but codes and structured fields are stable.

`LIMIT` is optional. When omitted, `SearchOptions.limit` is 1000. A supplied
limit must be positive and no greater than the language maximum of 100000;
omission is not an error. Ordering defaults to ascending canonical semantic
identity.

The only canonical backend spellings are `cidx-sqlite-v1`,
`cidx-souffle-v1`, and `cidx-kuzu-cypher-v1`. The supported language-version
spellings are `cxx17`, `cxx20`, and `cxx23`; the formatter emits these exact
tokens. Profile and version selection is therefore stable across parsers and
cannot depend on identifier tokenization.

## Normative ANTLR grammar

The following combined grammar is the normative syntax. It is case-insensitive
and is compiled with the ANTLR4 toolchain. Semantic validation after parsing
enforces the applicability, completeness, name-resolution, and backend rules
defined below.

```antlr
grammar CidxSearch;

options { caseInsensitive = true; }

query
    : findClause whereClause? navigationClause? selectClause
      orderClause? limitClause? unknownClause? backendClause?
      languageClause? EOF
    ;

findClause
    : FIND target
    ;

target
    : ALL? trait* targetKind namedClause?
    ;

targetKind
    : CLASS | CLASSES | STRUCT | STRUCTS | UNION | UNIONS
    | RECORD | RECORDS
    | METHOD | METHODS | FUNCTION | FUNCTIONS
    | CONSTRUCTOR | CONSTRUCTORS | DESTRUCTOR | DESTRUCTORS
    | OPERATOR | OPERATORS | DATA MEMBER | DATA MEMBERS
    | COMPONENT | COMPONENTS | FILE | FILES
    | TEMPLATE SPECIALIZATION | TEMPLATE SPECIALIZATIONS
    | TEMPLATE INSTANTIATION | TEMPLATE INSTANTIATIONS
    ;

namedClause
    : NAMED string
    | NAME MATCHING (GLOB | REGEX)? string
    ;

whereClause
    : WHERE orExpression
    ;

orExpression
    : andExpression (OR andExpression)*
    ;

andExpression
    : unaryExpression (AND unaryExpression)*
    ;

unaryExpression
    : NOT unaryExpression
    | LPAREN orExpression RPAREN
    | atom
    ;

atom
    : traitAtom
    | nameAtom
    | inheritanceAtom
    | interfaceRequirement
    | templateRequirement
    | memberRequirement
    | aggregateRequirement
    | universalRequirement
    | scopeAtom
    ;

traitAtom
    : trait
    ;

nameAtom
    : NAME string
    | NAME MATCHING (GLOB | REGEX)? string
    ;

scopeAtom
    : IN (COMPONENT | REPOSITORY | FILE) string
    ;

inheritanceAtom
    : CHILD OF string
    | DESCENDANT OF string depthClause
    | DIRECT PARENT OF string
    | ANCESTOR OF string depthClause
    ;

interfaceRequirement
    : DIRECTLY IMPLEMENTS INTERFACE string
    | IMPLEMENTS INTERFACE string depthClause
    | IMPLEMENTS (ANY | ALL) OF LBRACK interfaceReference
      (COMMA interfaceReference)* RBRACK depthClause
    | IMPLEMENTS interfaceCardinality integer (INTERFACE | INTERFACES) depthClause
    ;

interfaceReference
    : INTERFACE string
    ;

interfaceCardinality
    : AT LEAST | AT MOST | EXACTLY | MORE_WORD THAN | FEWER THAN
    ;

templateRequirement
    : SPECIALIZES TEMPLATE string signatureClause?
    | INSTANTIATES TEMPLATE string signatureClause?
    | EXPLICIT? TEMPLATE ARGUMENTS ARE templateArgumentList
    | EXPLICIT? TEMPLATE ARGUMENT integer IS templateArgumentPattern
    ;

templateArgumentList
    : LBRACK templateArgumentPattern (COMMA templateArgumentPattern)* RBRACK
    ;

templateArgumentPattern
    : ANY
    | TYPE string
    | NON_TYPE string
    | TEMPLATE string
    | PACK LBRACK templateArgumentPattern
      (COMMA templateArgumentPattern)* RBRACK
    ;

memberRequirement
    : MUST HAVE article? memberExpression
    | MUST NOT HAVE article? memberExpression
    ;

aggregateRequirement
    : AT LEAST integer relatedSetSpec
    | AT MOST integer relatedSetSpec
    | EXACTLY integer relatedSetSpec
    | MORE_WORD THAN integer relatedSetSpec
    | FEWER THAN integer relatedSetSpec
    ;

universalRequirement
    : EVERY memberSelector IS traitExpression
    ;

memberExpression
    : memberSpec
    | ANY OF LBRACK memberSpec (COMMA memberSpec)* RBRACK
    | ALL OF LBRACK memberSpec (COMMA memberSpec)* RBRACK
    ;

memberSpec
    : trait* memberHead trait*
    ;

memberHead
    : METHOD string? signatureClause?
    | CONSTRUCTOR signatureClause?
    | DESTRUCTOR signatureClause?
    | OPERATOR string signatureClause?
    | CONVERSION OPERATOR TO string signatureClause?
    | LITERAL OPERATOR string signatureClause?
    | DATA MEMBER string? signatureClause?
    | FIELD string? signatureClause?
    ;

memberSelector
    : trait* memberKind
    ;

memberKind
    : METHOD | METHODS | CONSTRUCTOR | CONSTRUCTORS
    | DESTRUCTOR | DESTRUCTORS | OPERATOR | OPERATORS
    | DATA MEMBER | DATA MEMBERS | FIELD | FIELDS
    ;

traitExpression
    : trait+
    ;

relatedSetSpec
    : trait* (bindingName DOT)? relatedSetAtom depthClause? predicateClause?
    ;

relatedSetAtom
    : DIRECT CHILDREN | CHILDREN | DESCENDANTS
    | DIRECT PARENTS | ANCESTORS
    | METHODS | DATA MEMBERS | FIELDS | CALLERS | CALLEES
    | CLASS | CLASSES | RECORD | RECORDS | STRUCT | STRUCTS | UNION | UNIONS
    | METHOD | FUNCTION | FUNCTIONS
    | CONSTRUCTOR | CONSTRUCTORS | DESTRUCTOR | DESTRUCTORS
    | OPERATOR | OPERATORS | COMPONENT | COMPONENTS | FILE | FILES
    | DATA MEMBER | FIELD
    | IMPLEMENTED INTERFACES
    ;

predicateClause
    : WHERE orExpression
    ;

navigationClause
    : navigationStep+
    ;

navigationStep
    : NAVIGATE TO THEIR? navigationRelation depthClause? aliasClause?
    | INCLUDE navigationRelation memberFilter? aliasClause?
    | SHOW navigationRelation depthClause? aliasClause?
    ;

aliasClause
    : AS bindingName
    ;

memberFilter
    : WITH traitExpression
    | WHERE orExpression
    ;

navigationRelation
    : DIRECT CHILDREN | CHILDREN | DESCENDANTS
    | DIRECT PARENTS | ANCESTORS
    | METHODS | DATA MEMBERS | FIELDS | CALLERS | CALLEES
    ;

selectClause
    : SELECT projection (COMMA projection)*
    ;

projection
    : fieldProjection
    | countProjection
    | interpolatedProjection
    ;

fieldProjection
    : binding? fieldName projectionAlias?
    ;

countProjection
    : COUNT LPAREN relatedSetSpec RPAREN AS identifier
    ;

interpolatedProjection
    : string projectionAlias?
    ;

projectionAlias
    : AS identifier
    ;

orderClause
    : ORDER BY orderTerm (COMMA orderTerm)*
    ;

orderTerm
    : orderReference (ASC | ASCENDING | DESC | DESCENDING)?
    ;

orderReference
    : binding? fieldName
    | COUNT LPAREN relatedSetSpec RPAREN
    | identifier
    ;

limitClause
    : LIMIT integer
    ;

unknownClause
    : ON UNKNOWN (EXCLUDE | INCLUDE | ERROR)
    ;

backendClause
    : USING BACKEND backendId
    ;

languageClause
    : LANGUAGE cxxVersion
    ;

backendId
    : CIDX_SQLITE_V1
    | CIDX_SOUFFLE_V1
    | CIDX_KUZU_CYPHER_V1
    ;

cxxVersion
    : CXX17
    | CXX20
    | CXX23
    ;

depthClause
    : UP TO integer LEVELS
    ;

signatureClause
    : SIGNATURE string
    ;

article
    : A | AN
    ;

binding
    : bindingName DOT
    ;

bindingName
    : identifier
    | CLASS | CLASSES | RECORD | RECORDS | STRUCT | STRUCTS | UNION | UNIONS
    | METHOD | METHODS | FUNCTION | FUNCTIONS
    | CONSTRUCTOR | CONSTRUCTORS | DESTRUCTOR | DESTRUCTORS
    | OPERATOR | OPERATORS | COMPONENT | COMPONENTS | FILE | FILES
    | CHILD | CHILDREN | PARENT | PARENTS | ANCESTOR | ANCESTORS
    | CALLER | CALLERS | CALLEE | CALLEES | FIELD | FIELDS
    | SPECIALIZATION | SPECIALIZATIONS | INSTANTIATION | INSTANTIATIONS
    | INTERFACE | INTERFACES
    ;

fieldName
    : NAME | QUALIFIED_NAME | KIND | SIGNATURE | COMPONENT
    | FILE | LINE | COLUMN | EVIDENCE | TRUTH
    | TEMPLATE_ENTITY_KIND | TEMPLATE_ARGUMENTS
    | EXPLICIT_TEMPLATE_ARGUMENTS | TEMPLATE_ORIGIN | TEMPLATE_TARGET
    ;

trait
    : PURE VIRTUAL
    | ABSTRACT | FINAL | PUBLIC | PROTECTED | PRIVATE
    | VIRTUAL | OVERRIDING | OVERRIDE | STATIC | CONST | VOLATILE
    | CONSTEXPR | CONSTEVAL | EXPLICIT | NOEXCEPT
    | DELETED | DEFAULTED | MUTABLE
    | IDENTIFIER
    ;

identifier
    : IDENTIFIER
    ;

string
    : STRING
    ;

integer
    : INTEGER
    ;

FIND         : 'find';
ALL          : 'all';
A            : 'a';
AN           : 'an';
CLASS        : 'class';
RECORD       : 'record';
RECORDS      : 'records';
STRUCT       : 'struct';
UNION        : 'union';
FUNCTION     : 'function';
ABSTRACT     : 'abstract';
FINAL        : 'final';
PUBLIC       : 'public';
PROTECTED    : 'protected';
PRIVATE      : 'private';
PURE         : 'pure';
VIRTUAL      : 'virtual';
OVERRIDE     : 'override';
OVERRIDING   : 'overriding';
STATIC       : 'static';
CONST        : 'const';
VOLATILE     : 'volatile';
CONSTEXPR    : 'constexpr';
CONSTEVAL    : 'consteval';
EXPLICIT     : 'explicit';
NOEXCEPT     : 'noexcept';
DELETED      : 'deleted';
DEFAULTED    : 'defaulted';
MUTABLE      : 'mutable';
WHERE        : 'where';
SELECT       : 'select';
NAMED        : 'named';
NAME         : 'name';
MATCHING     : 'matching';
GLOB         : 'glob';
REGEX        : 'regex';
IN           : 'in';
COMPONENT    : 'component';
REPOSITORY   : 'repository';
FILE         : 'file';
MUST         : 'must';
HAVE         : 'have';
NOT          : 'not';
ANY          : 'any';
OF           : 'of';
AT           : 'at';
LEAST        : 'least';
MOST         : 'most';
EXACTLY      : 'exactly';
MORE_WORD    : 'more';
THAN         : 'than';
FEWER        : 'fewer';
EVERY        : 'every';
IS           : 'is';
ARE          : 'are';
NAVIGATE     : 'navigate';
TO           : 'to';
THEIR        : 'their';
INCLUDE      : 'include';
WITH         : 'with';
SHOW         : 'show';
UP           : 'up';
LEVELS       : 'levels';
DIRECT       : 'direct';
DIRECTLY     : 'directly';
CHILD        : 'child';
CHILDREN     : 'children';
DESCENDANT   : 'descendant';
DESCENDANTS  : 'descendants';
PARENT       : 'parent';
PARENTS      : 'parents';
ANCESTOR     : 'ancestor';
ANCESTORS    : 'ancestors';
METHOD       : 'method';
METHODS      : 'methods';
CONSTRUCTOR  : 'constructor';
CONSTRUCTORS : 'constructors';
DESTRUCTOR   : 'destructor';
DESTRUCTORS  : 'destructors';
OPERATOR     : 'operator';
OPERATORS    : 'operators';
DATA         : 'data';
MEMBER       : 'member';
MEMBERS      : 'members';
FIELD        : 'field';
FIELDS       : 'fields';
CALLERS      : 'callers';
CALLEES      : 'callees';
CALLER       : 'caller';
CALLEE       : 'callee';
CLASSES      : 'classes';
STRUCTS      : 'structs';
UNIONS       : 'unions';
FUNCTIONS    : 'functions';
COMPONENTS   : 'components';
FILES        : 'files';
INTERFACE    : 'interface';
INTERFACES   : 'interfaces';
IMPLEMENTS   : 'implements';
IMPLEMENTED  : 'implemented';
TEMPLATE     : 'template';
SPECIALIZES  : 'specializes';
INSTANTIATES : 'instantiates';
SPECIALIZATION  : 'specialization';
SPECIALIZATIONS : 'specializations';
INSTANTIATION   : 'instantiation';
INSTANTIATIONS  : 'instantiations';
ARGUMENT     : 'argument';
ARGUMENTS    : 'arguments';
TYPE         : 'type';
NON_TYPE     : 'non_type';
PACK         : 'pack';
CONVERSION   : 'conversion';
LITERAL      : 'literal';
COUNT        : 'count';
ORDER        : 'order';
BY           : 'by';
ASC          : 'asc';
DESC         : 'desc';
ASCENDING    : 'ascending';
DESCENDING   : 'descending';
LIMIT        : 'limit';
ON           : 'on';
UNKNOWN      : 'unknown';
EXCLUDE      : 'exclude';
ERROR        : 'error';
USING        : 'using';
AS           : 'as';
BACKEND      : 'backend';
LANGUAGE     : 'language';
CIDX_SQLITE_V1 : 'cidx-sqlite-v1';
CIDX_SOUFFLE_V1 : 'cidx-souffle-v1';
CIDX_KUZU_CYPHER_V1 : 'cidx-kuzu-cypher-v1';
CXX17        : 'cxx17';
CXX20        : 'cxx20';
CXX23        : 'cxx23';
SIGNATURE    : 'signature';
QUALIFIED_NAME : 'qualified_name';
KIND         : 'kind';
LINE         : 'line';
COLUMN       : 'column';
EVIDENCE     : 'evidence';
TRUTH        : 'truth';
TEMPLATE_ENTITY_KIND : 'template_entity_kind';
TEMPLATE_ARGUMENTS : 'template_arguments';
EXPLICIT_TEMPLATE_ARGUMENTS : 'explicit_template_arguments';
TEMPLATE_ORIGIN : 'template_origin';
TEMPLATE_TARGET : 'template_target';
AND          : 'and';
OR           : 'or';
COMMA        : ',';
DOT          : '.';
LPAREN       : '(';
RPAREN       : ')';
LBRACK       : '[';
RBRACK       : ']';
STRING       : '"' ( '\\' . | ~["\\] )* '"';
INTEGER      : [0-9]+;
IDENTIFIER   : [a-z_] [a-z0-9_]*;
WS           : [ \t\r\n]+ -> skip;
COMMENT      : '#' ~[\r\n]* -> skip;
```

The grammar is accompanied by parse/format/round-trip fixtures for every
example in this document. `ALL OF` is represented by the two-token phrase
`ALL OF`; the formatter emits `all of [ ... ]` with explicit brackets and
commas. Semantic validation resolves keyword/token collisions before building
the IR.

`interpolatedProjection` deliberately reuses the outer `string` token. After
normal string decoding, the interpolation scanner applies the placeholder
grammar from the interpolation section and produces typed segments; a quoted
projection with no placeholder fails with `E_INTERPOLATION`. This keeps `$`
and braces out of the general lexer and makes string escaping a single,
ordered two-stage contract.

`templateArgumentPattern` is a predicate grammar, so `ANY` is legal there but
not in stored argument values or canonical projections. Semantic validation
also enforces that `SPECIALIZES` applies only to specialization targets,
`INSTANTIATES` only to instantiation targets, direct implementation has depth
one, and every transitive implementation or implemented-interface related set
has an explicit finite `depthClause`.

`signatureClause` is an explicit grammar handoff: the outer CIDX `string`
token is decoded once, and the resulting payload is parsed as the complete
`signature` rule by the versioned `CidxSignature.g4` grammar. The payload is
decoded into a typed `SignatureConstraint`; the canonical signature formatter
serializes that typed value, and the outer formatter re-encodes the resulting
literal. It is never retained as an opaque string. Escaping at the outer query
layer and escaping inside the decoded signature payload are therefore
validated independently.

## Typed Skill/Search IR

```text
SearchQuery {  // the root typed Skill IR value
  target: TargetSet,
  target_name: optional NamePredicate,
  target_traits: [Trait],
  predicate: optional Predicate,  // absent means true
  navigation: [NavigationStep],
  projection: [Projection],
  options: SearchOptions
}

Predicate := And([Predicate])
           | Or([Predicate])
           | Not(Predicate)
           | TraitTest(Trait)
           | NameEquals(Binding, String)
           | NameMatches(Binding, Pattern)
           | Inheritance(Relation, Reference, DepthRange)
           | InterfaceImplementation(InterfaceRequirement)
           | TemplateRelation(TemplateSemanticRelation, TemplateReference)
           | TemplateArgumentsMatch(TemplateArgumentPredicate)
           | Exists(RelatedSetSpec)
           | NotExists(RelatedSetSpec)
           | Cardinality(RelatedSetSpec, Comparison, Integer)
           | ForAll(MemberSelector, TraitExpression)
           | Scope(ScopeReference)

Projection := FieldProjection | AggregateProjection | InterpolatedProjection

TargetKind := Class | Struct | Union | Method | Function
            | Constructor | Destructor | Operator | DataMember
            | Component | File | TemplateSpecialization
            | TemplateInstantiation

TargetSet {
  kinds: Set<TargetKind>,
  semantic_filter: optional Predicate
}

The surface `record`/`records` target is normalized immediately to
`TargetSet { kinds: {Class, Struct} }`. `Record` is not a `TargetKind` and
never appears in canonical Skill IR, Logical Query IR, or result identities.

Canonical target normalization fixture:

```text
RecordTargetFixture {
  surface: "Find all records",
  canonical_target: TargetSet { kinds: {Class, Struct} }
}
```

NamePredicate := NameEquals(String) | NameMatches(Pattern)

Pattern {
  kind: Exact | Glob | Regex,
  text: String,
  case_sensitive: Boolean = true
}

FieldProjection {
  binding: optional Binding,
  field: ProjectionField,
  alias: optional Identifier
}

AggregateProjection {
  set: RelatedSetSpec,
  measure: Count,
  alias: Identifier
}

InterpolatedProjection {
  segments: [InterpolationSegment],
  alias: optional Identifier,
  key: ProjectionKey,
  conversion_profile: ConversionProfileV1
}

InterpolationSegment := Text(UTF8NFCString) | Field(FieldRef)

The IR stores only typed `FieldRef` segments; it does not retain whether an
unqualified field was written as `$field` or `${field}`. Canonical formatting
derives the braced form from segment adjacency and always braces a qualified
reference. Backend adapters therefore receive neither placeholder spelling nor
string-scanner state.

RelatedSetSpec {
  source_binding: Binding,
  relation: SemanticRelation,
  target: TargetSet,
  predicate: optional Predicate,
  depth: DepthRange,
  identity: IdentityPolicy
}

ProjectionField := Name | QualifiedName | Kind | Signature | Component
                 | File | Line | Column | Evidence | Truth
                 | TemplateEntityKind | TemplateArguments
                 | ExplicitTemplateArguments | TemplateOrigin | TemplateTarget

SemanticRelation := DirectChild | Child | DirectParent | Parent
                  | DeclaredMethods | DeclaredDataMembers
                  | ComponentRecords | ComponentMethods
                  | Caller | Callee | ImplementedInterfaces
                  | Overrides | Specializes | Instantiates | TemplateOf

NavigationRelation := DirectChildren | Children | Descendants
                    | DirectParents | Ancestors | Methods | DataMembers
                    | Callers | Callees

MemberRequirement {
  kind: Method | Constructor | Destructor | Operator | DataMember,
  name: optional Pattern,
  operator: optional OperatorIdentity,
  traits: [Trait],
  signature: optional SignatureConstraint
}

SignatureConstraint {
  name: String,
  explicit_template_arguments: [SignatureTemplateArgument],
  parameters: [CanonicalType],
  cv_qualifiers: [Const | Volatile],
  ref_qualifier: None | LValue | RValue,
  noexcept: optional NoexceptSpec,
  template_parameters: [TemplateParameter],
  constraints: optional CanonicalConstraint,
  return_type: optional CanonicalType
}

CanonicalType := UTF8NFCString

NoexceptSpec {
  constant_expression: optional UTF8NFCString
}

TemplateParameter {
  kind: Type | NonType | Template,
  declaration: UTF8NFCString
}

SignatureTemplateArgument {
  kind: Type | NonType | Template,
  value: UTF8NFCString
}

TemplateArgumentValue {
  kind: Type | NonType | Template | Pack,
  value: UTF8NFCString | [TemplateArgumentValue],
  semantic_reference: optional SemanticIdentity,
  resolution: Resolved | Dependent | Unknown
}

TemplateArgumentBinding {
  position: Integer >= 0,
  argument: TemplateArgumentValue,
  origin: SpelledExplicit | Deduced | Defaulted | Substituted
}

TemplateArgumentPattern := Any | Exact(TemplateArgumentValue)

TemplateArgumentPredicate :=
    EffectiveListEquals([TemplateArgumentPattern])
  | ExplicitListEquals([TemplateArgumentPattern])
  | EffectivePositionEquals(Integer >= 0, TemplateArgumentPattern)
  | ExplicitPositionEquals(Integer >= 0, TemplateArgumentPattern)

TemplateSemanticRelation := Specializes | Instantiates

TemplateReference {
  name: NamePredicate,
  signature: optional SignatureConstraint,
  allowed_template_kinds: Set<TemplateEntityKind>
}

TemplateEntityKind := Class | Function | Variable | Member
TemplateOrigin := ExplicitFullSpecialization | PartialSpecialization
                | ImplicitInstantiation | ExplicitInstantiationDeclaration
                | ExplicitInstantiationDefinition

InterfaceRequirement {
  source_binding: Binding,
  mode: Direct | Transitive,
  selector: NamedInterfaces([Reference]) | AllInterfaces,
  quantifier: Exists | Any | All | Cardinality(Comparison, Integer),
  depth: DepthRange
}

CanonicalConstraint := UTF8NFCString

NavigationStep {
  operation: To | Include | Show,
  source_binding: Binding,
  relation: NavigationRelation,
  target: TargetSet,
  depth: DepthRange,
  filter: optional Predicate,
  result_binding: Binding
}

Binding {
  name: Identifier,
  role: Seed | Class | Struct | Union | Method | Function
       | Constructor | Destructor | Operator | DataMember | Component | File
       | TemplateSpecialization | TemplateInstantiation
       | Child | Parent | Ancestor | Caller | Callee | Explicit
}

SearchOptions {
  unknown_policy: Exclude | Include | Error = Exclude,
  language_version: CxxVersion = Cxx23,
  order_by: [OrderTerm] = CanonicalIdentity,
  limit: Integer = 1000,
  max_depth: Integer = 32,  // fixed language constant
  backend: BackendId = CidxSqliteV1
}

BackendId := CidxSqliteV1 | CidxSouffleV1 | CidxKuzuCypherV1
CxxVersion := Cxx17 | Cxx20 | Cxx23

TruthValue := True | False | Unknown

IdentityPolicy := Declaration | Entity

CountValue {
  lower_bound: Integer,
  upper_bound: Integer | null,
  exact: Boolean,
  truncated: Boolean
}

DepthRange {
  min: Integer >= 1,
  max: Integer >= min and max <= 32
}

OrderTerm {
  key: OrderKey,
  direction: Ascending | Descending
}

OrderKey := FieldRef | ProjectionAlias | AggregateExpression

FieldRef {
  binding: optional Binding,
  field: ProjectionField
}

ProjectionAlias {
  alias: Identifier
}

AggregateExpression {
  set: RelatedSetSpec,
  measure: Count
}

TraitApplicability {
  target_kind: TargetKind,
  trait: Trait,
  valid: Boolean,
  diagnostic: optional ErrorCode
}
```

The source of truth for signature syntax and serialization is the versioned
[`docs/CidxSignature.g4`](CidxSignature.g4) artifact,
`CidxSignature` version 1. It defines the exact serialization:
`"name"<type "argument-type", non_type "argument-value", ...>("parameter-type", ...) cv/ref noexcept(...) template<...>
requires "constraint" -> "return-type"`, with each optional suffix emitted
only when present. Empty parameter lists omit the comma sequence. Parameter
names, default arguments, comments, and formatting whitespace do not
participate. Canonical types resolve aliases to fully qualified semantic types;
template parameter kinds, explicit template argument kinds and values,
constraints, cv/ref qualifiers, and `noexcept` do participate. Return type is optional: when
present it is checked against the declaration but never distinguishes overloads
by itself. A signature constraint without enough participating information to
select one declaration fails with `E_NAME_AMBIGUOUS`; malformed signature text
fails with `E_PARSE`. The signature grammar is compiled and round-tripped as a
separate conformance artifact. The query language version remains solely in
`ResolutionContext` and is not duplicated in `SignatureConstraint`.

`SignatureTemplateArgument` is the argument syntax used only to disambiguate a
callable signature. It remains distinct from both `TemplateParameter` and the
extracted `TemplateArgumentBinding` model used by template-entity search. In
particular, argument origin and structured packs are properties of searchable
template entities, not silently encoded as signature parameters.

Canonical strings use UTF-8 NFC. A literal double quote is escaped as `\"` and
a literal backslash as `\\`; no other backslash escape is accepted, and raw
control characters or line breaks are invalid. The formatter always emits
these two escapes and NFC-normalized text. Signature cv qualifiers are
canonicalized as an optional `const` followed by an optional `volatile`, with
at most one of each and an optional `&` or `&&`; duplicate or reversed
qualifiers are rejected with `E_PARSE` rather than silently normalized.

Signature grammar fixtures include these canonical values:

```text
"authorize"("int","std::string") const & noexcept -> "bool"
"compare"<type "int", non_type "3">("T") template<type "T", non_type "N"> requires "T:Comparable" -> "bool"
"run"() volatile && noexcept("true")
"quote\"name"("path\\value") const volatile &&
```

Each value is parsed by `CidxSignature.g4`, formatted canonically, and parsed
again into the same `SignatureConstraint` value.

Template-specialization ambiguity fixture:

```text
SignatureAmbiguityFixture {
  declarations:
    "foo"<type "int", non_type "3">("std::string"),
    "foo"<type "int", non_type "4">("std::string"),
  omitted_template_arguments: E_NAME_AMBIGUOUS,
  distinct_template_arguments: distinct declarations
}
```

```text
NonCanonicalSignatureFixture {
  rejected: `"run"() volatile const`, `"run"() const const`,
  diagnostic: E_PARSE
}
```

The IR must not leak Clang declaration kinds, SQLite table names, or backend
relation names into the public language.

An `ORDER BY` field reference is validated against its binding domain;
`ProjectionAlias` must resolve to exactly one selected projection; and an
`AggregateExpression` must match exactly one selected aggregate projection by
canonical `RelatedSetSpec`. A missing or duplicated reference fails with
`E_PROJECTION`. Aggregate and field projection keys share one uniqueness scope.

## Logical Query IR

The backend-neutral lowering is relational and set-oriented:

```text
Scan(Class)
→ Filter(Abstract)
→ SemiJoin(DirectDerivedFrom(resolve("BaseController")))
→ SemiJoin(HasMember(Method, name="authorize", traits=[Public, PureVirtual]))
→ SemiJoin(HasMember(DataMember, name="token", traits=[Private]))
→ Project(Name, Component)
→ Distinct
```

Required logical operators:

```text
Scan
Filter
SemiJoin
AntiSemiJoin
Union
Intersect
Project
Aggregate
Having
ForAll
Expand
OuterExpand
BindOrigin
Distinct
Sort
Limit
Evidence
ClassifyInterface
TraversePublicBases
MatchOverride
MatchTemplateRelation
MatchTemplateArguments
RenderInterpolation
```

`ClassifyInterface` consumes abstractness, declared-member, direct-base, and
pure-virtual facts and returns `TruthValue`. `TraversePublicBases` returns a
cycle-safe set of `(implementor, interface, depth, witness)` tuples within the
IR depth bound; interface cardinality aggregates distinct interface semantic
identities. `MatchOverride` consumes the semantic override relation and never
source tokens. `MatchTemplateRelation` and `MatchTemplateArguments` operate on
typed template identities, argument bindings, and origin values. None of these
operators may be lowered to formatted-name matching.

`RenderInterpolation` accepts typed field slots and ordered text/field
segments, applies `ConversionProfileV1`, and produces one `String`. It is a
logical projection operator even when physically executed in a host
post-layer. Projection keys and sorting are applied to its rendered value in
the same order on every backend.

Truth-valued operators are explicit in the Logical Query IR:

```text
Exists(RelatedSetSpec) -> TruthValue
NotExists(RelatedSetSpec) -> TruthValue
ForAll(RelatedSetSpec, TraitExpression) -> TruthValue
TruthNot(TruthValue) -> TruthValue
TruthAnd(TruthValue, TruthValue) -> TruthValue
TruthOr(TruthValue, TruthValue) -> TruthValue
CompletenessGuard(FactPartition) -> Complete | Incomplete | Unknown
```

`Exists` returns `true` as soon as a matching fact is found. It returns
`false` only when the related set is complete and empty; otherwise it returns
`unknown`. `NotExists` is the strong negation of that result. `ForAll`
returns `false` when a violating member is found, `true` only when the complete
set has no violation, and `unknown` otherwise. Runtime truncation is treated
as incomplete for every affected truth value: a witnessed positive/violation
remains decisive, but absence is not.

`Expand` implements `Navigate to` and `Include` as inner origin/related pair
expansion. `OuterExpand` implements `Show` as a left outer expansion: it emits
one pair per related entity and one pair with null related fields when the set
is empty. The distinction is observable in row cardinality and is mandatory
for every backend.

Backends remain responsible for physical planning. SQLite uses joins and
recursive CTEs; Datalog uses relations and stratified negation; Cypher-like
backends must normalize path multiplicity and duplicates.

## Backend contract

Every backend adapter implements the same logical semantics. An adapter may
choose a different physical plan, but it must not silently change:

- set identity and duplicate handling;
- inheritance closure and finite depth bounds;
- trait applicability and operator identity;
- interface classification, public implementation closure, and semantic
  override identity;
- specialization/instantiation category, selected-template relation,
  structured template arguments, and argument origins;
- aggregate count scope and lower/upper bounds;
- `true`, `false`, and `unknown` propagation;
- origin bindings and evidence association;
- deterministic projection, ordering, and limits;
- interpolation parsing, typed conversion, null/unknown markers, projection
  keys, and UTF-8 bytes.

Each adapter publishes a capability descriptor before execution. A query is
compiled only when every logical operator has a supported implementation. If
not, compilation fails with `E_BACKEND_UNSUPPORTED` naming the operator and
backend; the adapter must not approximate or partially execute the query.
The built-in capability descriptors must advertise the complete Logical Query
IR; they may not use this error for a documented feature.

The mandatory built-in profiles are:

| Adapter profile | Engine baseline | Capability schema |
|---|---|---:|
| `cidx-sqlite-v1` | SQLite 3.45 or newer, CIDX SQL profile v1 | 1 |
| `cidx-souffle-v1` | Soufflé 2.4.1, CIDX Datalog profile v1 | 1 |
| `cidx-kuzu-cypher-v1` | Kùzu 0.8.2, CIDX Cypher profile v1 | 1 |

Each built-in profile must implement the complete Logical Query IR and pass
the full conformance suite; a built-in adapter that rejects a documented
operator is non-conformant. Capability descriptors include profile name,
engine version, dialect profile, and capability schema version. New engine
versions must pass the same suite before being accepted under the profile.
`E_BACKEND_UNSUPPORTED` is reserved for optional external adapters and clearly
identifies their unsupported capability.

The conformance suite runs the same fixtures through every adapter and compares
canonical result sets, aggregate values/bounds, unknown states, evidence, and
diagnostics. Backend-specific plans are not compared; observable semantics are.

The SQLite adapter uses semi-joins, anti-semi-joins, recursive CTEs, `COUNT`,
and `HAVING`; public-base closure and interface proof remain explicit logical
steps. The Soufflé adapter uses relations, stratified negation, and explicit
aggregate relations. The Kùzu Cypher adapter must normalize variable-length
path multiplicity into the defined set semantics. All adapters consume typed
template rows and semantic override edges rather than name/token heuristics.
A host post-layer is allowed only when it preserves the same logical result
contract, including `RenderInterpolation`, and is declared in the capability
descriptor.

## Completeness and results

Every relation and property used by a query has a completeness state:

```text
complete | incomplete | unknown
```

`complete` means the selected repository/component scope was fully indexed,
all relevant translation units were available, and extraction/resolution
reported no omitted facts for that relation or property. Any failed,
unindexed, or unresolved scope is `incomplete`; unavailable metadata is
`unknown`.

Completeness is recorded per fact partition, not only per repository or
component:

```text
FactPartition {
  scope: ScopeKey,
  subject: SemanticIdentity,
  relation_or_property: SemanticName,
  state: Complete | Incomplete | Unknown,
  diagnostics: [Diagnostic]
}
```

For example, completeness for `has_method` on class `Widget` is independent of
completeness for `calls` from `Widget::run` or for another class in the same
component. A query combines all partitions touched by its Search/Logical IR.

The result contract distinguishes:

```text
true | false | unknown
```

The default `on unknown exclude` policy returns only rows whose complete
evaluation is proven `true`. `on unknown include` returns both `true` and
`unknown` rows with their `truth` value and reason. `on unknown error` rejects
an actually unknown result with `E_INCOMPLETE_FACTS`.

The same policy is applied to aggregates. Aggregate truth is determined by the
five interval rules above:

- `at least N` is `true` when the observed lower bound is at least `N`;
- `at most N` is `true` only when the count is untruncated and the upper bound
  is at most `N`;
- `exactly N` is `true` only when the count is untruncated and lower and upper
  bounds are both exactly `N`;
- `more than N` is `true` when the lower bound is greater than `N`;
- `fewer than N` is `true` when the count is untruncated and the finite upper
  bound is less than `N`;
- otherwise the aggregate is `unknown` unless its opposite is proven.

For an incomplete count projection, the result is a typed value rather than a
misleading scalar:

```text
CountValue {
  lower_bound: Integer,
  upper_bound: Integer | null,
  exact: Boolean,
  truncated: Boolean
}
```

`upper_bound = null` means unbounded. An exact count has finite equal bounds,
`truncated = false`, and `exact = true`; `truncated = true` always implies
`exact = false`. `on unknown
error` reports `E_INCOMPLETE_FACTS` only when the count comparison or requested
projection is actually unknown. `on unknown include` returns the bounds and the
facts that prevented exactness.

For `must not have`, `not`, `at most`, and `exactly`, an incomplete relevant
relation can never produce a proven positive result solely from absence.
`AntiSemiJoin` is therefore only a legal optimization after the completeness
check succeeds. `ForAll` remains executable under incompleteness: a witnessed
violation returns `false`; otherwise it returns `unknown` until completeness
is established.

Execution budgets and result truncation are part of the same contract. If a
budget truncates a partition, the affected `TruthValue` becomes `unknown`
unless a witnessed fact already proves `true` or `false`; affected
`CountValue` instances set `truncated = true`. The invariant is always
`exact = (!truncated && upper_bound != null && lower_bound = upper_bound)`;
independent bounds do not override a truncation marker.

The top-level result contract distinguishes predicate uncertainty from result
enumeration completeness:

```text
SearchResult {
  rows: [SearchRow],
  result_set_completeness: Complete | Incomplete | Unknown,
  truncated: Boolean,
  applied_limit: Integer,
  omitted: optional OmissionSummary,
  unknown_reasons: [Diagnostic],
  evidence: [EvidenceValue]
}

SearchRow {
  values: OrderedMap<ProjectionKey, TypedValue>,
  truth: TruthValue,
  evidence: [EvidenceValue]
}

ProjectionKey := String
TypedValue := String | Integer | Boolean | TruthValue | CountValue
            | TemplateArgumentListValue | TemplateOrigin | TemplateEntityKind
            | SemanticIdentity | EvidenceCollection | Null | UnknownScalar
EvidenceCollection := [EvidenceValue]

TemplateArgumentListValue := [TemplateArgumentBinding]
UnknownScalar {
  reason: Diagnostic
}

OmissionSummary {
  lower_bound: Integer,
  upper_bound: Integer | null,
  reason: Limit | Budget | IncompleteIndex | UnknownIndex | UnknownPredicate
}

EvidenceValue {
  kind: Declaration | Relation | Completeness | Omission,
  subject: SemanticIdentity,
  relation: optional SemanticRelation,
  object: optional SemanticIdentity,
  location: optional SourceLocation,
  completeness: optional FactPartition
}

SourceLocation {
  file: FileIdentity,
  line: Integer,
  column: Integer
}

FileIdentity {
  repository_relative_path: String,
  repository_id: RepositoryIdentity
}

RepositoryIdentity {
  canonical_id: String
}
```

`result_set_completeness` is relative to the selected `on unknown` policy. Every
built-in profile evaluates or otherwise proves the complete target universe
before applying a presentation `LIMIT`. A limit then sets `truncated = true`
and `applied_limit` but leaves `result_set_completeness = Complete`;
`OmissionSummary` reports the exact omitted count with `reason = Limit`.
Under `on unknown exclude`, candidates whose predicate truth is `unknown` are
omitted and the result becomes `Incomplete` with `reason = UnknownPredicate`;
under `on unknown include`, those candidates are returned with
`truth = Unknown`, so complete target enumeration remains `Complete`. Under
`on unknown error`, the query fails with `E_INCOMPLETE_FACTS`. An
implementation may stop early only when it proves the remaining-set bounds;
otherwise it must mark the result `Incomplete`, report omission bounds with
`reason = Budget`, and apply the unknown policy. Incomplete target indexing or
unknown target coverage produces `Incomplete` or `Unknown` independently of
predicate policy and cannot be repaired by `include`: a known missing target
range is `Incomplete` with `reason = IncompleteIndex`, while an unbounded
unknown target partition is `Unknown` with `reason = UnknownIndex`.

The result-policy fixture distinguishes the cases:

```text
ResultCompletenessFixture {
  complete_targets_unknown_predicate_exclude:
    { result_set_completeness: Incomplete, reason: UnknownPredicate },
  complete_targets_unknown_predicate_include:
    { result_set_completeness: Complete, row_truth: Unknown },
  incomplete_targets:
    { result_set_completeness: Incomplete, reason: IncompleteIndex },
  complete_targets_presentation_limit:
    { result_set_completeness: Complete, truncated: true, reason: Limit }
}
```

Results are deterministic, deduplicated, bounded, and may include evidence:

```text
name, qualified_name, kind, signature,
component, file, line, truth, evidence
```

Projection keys are canonical strings: an unqualified field uses its field
name (`name`), a qualified field uses `binding.field` (`class.name`), and an
aggregate uses its required alias. A field alias replaces that derived key.
Aliases and derived keys share one result-row namespace; a duplicate key fails
with `E_PROJECTION`, while aliases are required only when a field would
otherwise collide. This preserves both `name` and `class.name` without
pretending a dotted key is an `Identifier`.

Template projection fields use keys `template_entity_kind`,
`template_arguments`, `explicit_template_arguments`, `template_origin`, and
`template_target`. Argument-list values serialize as an ordered array of the
canonical `TemplateArgumentBinding` records; human-readable text uses the
canonical argument syntax defined above. `template_target` is a semantic
identity, not a display-name join. Interpolated projections use their alias or
canonical formatted template body as specified in the interpolation section.

Canonical ordering uses semantic identities, never backend-local integer IDs:
`(binding, target_kind, qualified_name, signature, component, file, line)`
with nulls last and stable lexical ordering. Backend adapters may use local
IDs internally only after applying this semantic ordering key.

Evidence values are sorted by `(kind, relation, subject, object,
repository_relative_path, line, column)` with nulls last. Source paths are
normalized before evidence serialization: repository-relative, `/` separated,
UTF-8 NFC, symlinks resolved within the repository, and with the index's
canonical case preserved rather than host-filesystem case folding. Paths that
escape the repository or cannot be normalized fail with `E_INTERNAL`. Semantic
identities and relation names are canonical strings; `RepositoryIdentity` is
the stable index-provided repository identifier, never an absolute host path.
This schema and ordering are part of byte-identical backend conformance.

Navigation results preserve the seed/related relationship. Selectable fields
may be qualified by their binding, for example `class.name`, `child.name`, or
`caller.component`. The same related entity reached from multiple seeds is
not collapsed across origins unless the query explicitly requests a distinct
projection.

`explain` must show:

1. name resolution;
2. typed Search IR;
3. logical Query IR;
4. completeness assumptions;
5. selected backend and capabilities;
6. applied limits and unknown handling.

## Normative capability fixtures

The following fixtures are shared parser, formatter, semantic, Logical IR, and
backend-conformance inputs. Canonical query text is parsed again and must yield
the same typed `SearchQuery`.

```text
InterpolationFixture {
  surface: "Find all functions\nSelect \"$filename:$lineno\"",
  canonical: "Find all functions\nSelect \"$filename:$lineno\"",
  ir: InterpolatedProjection([
        Field(File), Text(":"), Field(Line)
      ], key="$filename:$lineno"),
  row: { File: "src/a.cpp", Line: 17 },
  value: "src/a.cpp:17"
}
```

```text
BoundaryInterpolationFixture {
  surface: "Find all records\nSelect \"${name}_abstract:${record.qualified_name}\" as label",
  canonical: "Find all records\nSelect \"${name}_abstract:${record.qualified_name}\" as label",
  ir: InterpolatedProjection([
        Field(Name), Text("_abstract:"), Field(record.QualifiedName)
      ], key="label"),
  clear_boundary_surface: "Find all records\nSelect \"${name}:$filename\"",
  clear_boundary_canonical: "Find all records\nSelect \"$name:$filename\""
}
```

```text
QualifiedInterpolationFixture {
  surface: "Find all records\nShow methods AS methods\nSelect \"${records.name}:$$:${methods.qualified_name}\" as label",
  canonical: "Find all records\nShow methods AS method\nSelect \"${record.name}:$$:${method.qualified_name}\" as label",
  key: "label",
  null_related_value: "Widget:$:<null>",
  unknown_related_value: "Widget:$:<unknown>"
}
```

```text
InterpolationDiagnosticFixture {
  invalid: ["$", "$bogus", "$name_suffix", "$record.name",
            "${record}", "${record.name.extra}", "${record.evidence}",
            "${record.filename}"],
  diagnostic: E_INTERPOLATION,
  duplicate_unaliased_template_key: E_PROJECTION
}
```

```text
HierarchyFixture {
  records: [IReadable, IWritable, AbstractBoth, ConcreteBoth],
  public_bases: [AbstractBoth -> IReadable,
                 AbstractBoth -> IWritable,
                 ConcreteBoth -> AbstractBoth],
  interfaces: [IReadable, IWritable],
  query: "Find all classes\nWhere implements all of [interface \"IReadable\", interface \"IWritable\"] up to 8 levels\nSelect name",
  result: [AbstractBoth, ConcreteBoth],
  direct_query: "Find all classes\nWhere directly implements interface \"IReadable\"\nSelect name",
  direct_result: [AbstractBoth],
  distinct_interface_count_for_ConcreteBoth: 2
}
```

```text
InterfaceClassificationFixture {
  interface: { abstract: true, fields: [], methods: [pure_virtual run],
               bases: [], result: true },
  abstract_not_interface: { abstract: true, fields: [state], result: false },
  incomplete_negative_facts: { abstract: true, fields: [],
                               member_completeness: incomplete,
                               result: unknown }
}
```

```text
OverrideFixture {
  base: "Base::run() virtual",
  derived_without_keyword: "Derived::run()",
  semantic_edge: Overrides(Derived::run, Base::run),
  query: "Find all classes\nWhere must have an overriding method\nSelect name",
  result: [Derived],
  token_only_without_semantic_edge: no_match
}
```

```text
TemplateCategoryFixture {
  entities: [
    { name: "Box<type \"int\">", origin: ExplicitFullSpecialization,
      target: TemplateSpecialization },
    { name: "Box<type \"T*\">", origin: PartialSpecialization,
      target: TemplateSpecialization },
    { name: "Box<type \"long\">", origin: ImplicitInstantiation,
      target: TemplateInstantiation },
    { name: "Box<type \"double\">", origin: ExplicitInstantiationDefinition,
      target: TemplateInstantiation }
  ],
  specialization_results: ["Box<type \"int\">", "Box<type \"T*\">"],
  instantiation_results: ["Box<type \"long\">", "Box<type \"double\">"]
}
```

```text
TemplateArgumentFixture {
  parameters: [type "T", non_type "N"],
  arguments: [
    { position: 0, argument: type "int", origin: SpelledExplicit },
    { position: 1, argument: non_type "4", origin: Defaulted }
  ],
  effective_query: "Find all template instantiations\nWhere template arguments are [type \"int\", non_type \"4\"]\nSelect template_arguments",
  explicit_query: "Find all template instantiations\nWhere explicit template arguments are [type \"int\"]\nSelect explicit_template_arguments",
  parameters_never_equal_arguments: true
}
```

```text
TemplatePackFixture {
  canonical_argument: pack [type "int", non_type "4"],
  wildcard_query: "Find all template specializations\nWhere template arguments are [type \"std::tuple\", pack [any, non_type \"4\"]]\nSelect qualified_name",
  bad_pack_shape: E_TEMPLATE_ARGUMENT
}
```

```text
TemplateAmbiguityFixture {
  overloads: ["make"(int), "make"(double)],
  query_without_signature:
    "Find all template instantiations\nWhere instantiates template \"make\"\nSelect name",
  result: E_NAME_AMBIGUOUS,
  query_with_signature: resolves_one
}
```

```text
BackendNeutralCapabilityFixture {
  logical_operators: [ClassifyInterface, TraversePublicBases, MatchOverride,
                      MatchTemplateRelation, MatchTemplateArguments,
                      RenderInterpolation],
  adapters: [cidx-sqlite-v1, cidx-souffle-v1, cidx-kuzu-cypher-v1],
  required_observation: byte_identical_rows_truth_evidence_diagnostics
}
```

## Complete implementation requirements

The documented feature set is the required implementation contract. No subset
or phase distinction applies. An implementation is conformant only when it
supports the complete grammar, Skill IR, Logical Query
IR, completeness policy, navigation model, aggregation model, projections,
diagnostics, and backend contract defined here.

The implementation must provide:

- class-like records, methods, functions, constructors, destructors, operators,
  data members, components, and files;
- the finite canonical vocabulary and its explicitly declared aliases;
- exact and qualified-name resolution with deterministic overload handling;
- the complete C++ trait applicability matrix;
- direct and transitive inheritance navigation in both directions;
- CIDX interface classification, direct/transitive public implementation,
  interface quantifiers/cardinality, and semantic override matching;
- disjoint template-specialization/template-instantiation targets, direct
  template relations, structured argument/origin predicates, and projections;
- member, caller, and callee navigation with origin bindings and finite bounds;
- `must have`, `must not have`, `any of`, `all of`, and universal `every`
  requirements;
- cardinality constraints and typed count projections;
- explicit boolean negation with three-valued logic;
- aggregate, navigation, evidence, and origin-qualified projections;
- interpolated string projections with typed field segments, stable escaping,
  null/unknown conversion, canonical keys, and backend-neutral rendering;
- deterministic set semantics, ordering, limits, and duplicate policy;
- completeness-aware execution and exact unknown behavior;
- capability-negotiated backend adapters;
- canonical formatting, stable diagnostics, and `explain` output;
- grammar, semantic, IR, backend-conformance, and acceptance tests.

## Normative acceptance criteria

1. **Grammar and formatting:** every fenced query example and every syntax form
   in this document parses using the versioned ANTLR grammar, formats to one
   canonical representation, and round-trips without semantic change. Fixtures
   explicitly cover `named`, pre-member traits, `their`, bracketed `any of` and
   `all of`, `must have a`/`must not have an`, signatures, count projections
   including `count(classes)`, `count(class.methods)`, ordering, limits,
   `Show`, exact/glob/regex names, every canonical binding token, and direct,
   conversion, literal, allocation, deallocation, and `co_await` operators.
   Fixtures parse and format all three backend profile IDs and `cxx17`, `cxx20`,
   and `cxx23`, both template target spellings, every interface quantifier, and
   simple `$field`, boundary-sensitive `${field}`, qualified
   `${binding.field}`, and escaped-dollar interpolation forms.
2. **Semantic IR isolation:** canonical Skill IR contains only public concepts
   (`Class`, `Struct`, `Union`, `Method`, `Constructor`, `DataMember`,
   `DirectChild`, and so on), never an independent `Record` kind, Clang
   declaration names, SQLite tables, raw edge kinds, or backend operators.
   A canonical-IR fixture proves `record` normalizes to
   `TargetSet{kinds={Class, Struct}}`; template parameters and argument values
   occupy distinct IR types and cannot be substituted for each other.
3. **Trait legality:** every valid/invalid combination in the Class, Struct,
   Union, Method, Function, Constructor, Destructor, Operator, and DataMember
   columns has a golden test. `Record` dispatches to Class/Struct and never
   creates an independent declaration kind. Invalid combinations fail before
   backend planning with `E_TRAIT_APPLICABILITY` and identify the target and
   trait.
4. **Name resolution:** exact qualified names resolve deterministically;
   ambiguous single-declaration references fail with `E_NAME_AMBIGUOUS` and
   list candidates; signature-qualified references select one declaration;
   repeated binding names fail with `E_BINDING_COLLISION` rather than
   shadowing. Pattern predicates preserve exact/glob/regex kind and
   case-sensitive C++ matching. Conflicting declaration language provenance
   fails with `E_LANGUAGE_CONFLICT`.
5. **Operators:** symbol, conversion, and literal operators are represented by
   a structured `OperatorIdentity` containing form, token/target, and
   `Any | Member | NonMember` memberness. Language-version validation uses
   `ResolutionContext`. Fixtures cover `==`, `()`, `[]`, allocation and
   deallocation, conversion operators, literal operators, ambiguous
   conversions, C++23 static `operator()` and `operator[]`, and declarations
   split across translation units with different language-version contexts.
   The linked CIDX signature grammar is compiled separately and its canonical
   `SignatureConstraint` values round-trip.
6. **Inheritance:** direct relations use depth 1, transitive relations use a
   finite depth range, self is excluded, cycles terminate, and both directions
   produce the documented origin-qualified results. Direct and member
   relations supplied with any depth other than exactly 1 fail with `E_DEPTH`.
7. **Quantifiers:** `must have`, `must not have`, `any of`, `all of`, and `every`
   match their formal definitions. `every` is true for an empty complete set
   and unknown for an incomplete set unless a violation is proven.
8. **Negation and completeness:** `not unknown` is unknown; incomplete
   anti-joins never become positive matches; `on unknown error` emits
   `E_INCOMPLETE_FACTS`; `on unknown include` returns the unknown reason and
   scope. Fixtures cover incomplete `ForAll`, negative predicates, runtime
   budget truncation, incomplete target enumeration, result-set completeness,
   and the distinction between predicate truth and omitted rows. Recursive and
   mutually recursive callers/callees never return the origin callable.
9. **Aggregations:** count predicates use the five lower/upper-bound interval
   rules when facts are incomplete; count projections return `CountValue` with
   `truncated`; exact counts are emitted only when `exact = true`. Fixtures
   cover all five comparisons, qualified related sets, repeated aliases,
   `ORDER BY` aggregate aliases, and prove that unknown mode rejects only
   unknown results.
10. **Projection and ordering:** `OrderKey` distinguishes field references,
    projection aliases, and aggregate expressions. Missing or duplicated
    aliases fail with `E_PROJECTION`; Truth projections serialize as
    `TruthValue` and Evidence projections as ordered `EvidenceCollection`.
    Duplicate derived keys such as repeated `class.name` are rejected unless
    an explicit unique field alias resolves the collision. Interpolated
    projections validate bindings/fields, use canonical template-body keys or
    aliases, render every typed scalar/null/unknown case, escape literal dollars,
    reject unbraced qualified/boundary-sensitive references, and sort/distinct
    on the rendered string at the specified logical stage.
11. **Navigation:** navigation preserves origin bindings, creates and
   collision-checks repeated bindings, applies member filters to related
   entities, enforces the language-wide maximum depth of 32, and never exposes
   raw paths or storage edge names. Fixtures cover every target/relation
   binding, cycles, repeated navigation bindings, nested-class access, static
   data members, inherited-member navigation, and `Show` left-outer null rows.
12. **Backend conformance:** `cidx-sqlite-v1`, `cidx-souffle-v1`, and
    `cidx-kuzu-cypher-v1` each run every shared fixture and produce identical
    canonical rows, counts/bounds, unknown
    states, evidence, ordering, and diagnostics. Profile and capability-schema
    versions are recorded. A built-in adapter cannot
    reject a documented operation, including hierarchy classification,
    structured template matching, and interpolation rendering.
    `E_BACKEND_UNSUPPORTED` is permitted only
    for an optional external adapter and must fail before execution.
13. **Determinism:** repeated execution over the same index and inputs produces
    byte-identical canonical output, including row order, projection order,
    diagnostics, and explain output.
14. **Vocabulary boundary:** undeclared synonyms fail with `E_UNKNOWN_TERM`;
    adding an alias requires a grammar/catalog version change and new parser
    and formatter fixtures.
15. **Evidence:** every returned relationship or declaration can identify its
   origin, related entity, source location when available, and completeness
   assumptions in the result or explain output. Evidence values use the
   specified schema and canonical ordering.
16. **Relation catalog:** every grammar related-set atom resolves through the
    single normative relation catalog; source domains, heterogeneous callable
    target sets, depth, completeness partitions, default bindings, component
    scopes, and direct/member depth validation are covered by golden tests.
17. **Result contract:** every execution returns the specified `SearchResult`
    metadata. A known presentation limit is distinguishable from incomplete
    target enumeration, and `on unknown error` rejects the latter before
    returning rows. Fixtures cover complete-enumeration presentation limiting,
    early-stop incomplete bounds, unknown-predicate exclusion versus inclusion,
    and all `OmissionSummary` reasons.
18. **Portability:** exact/glob/regex fixtures cover Unicode NFC, glob escapes,
    wildcard semantics, invalid patterns, and ECMAScript regex parity across
    all built-in profiles.
19. **Signatures and language contexts:** canonical signature fixtures cover
    aliases, parameter names/defaults, return types, cv/ref qualifiers,
    `noexcept`, template parameters and explicit template arguments,
    constraints, whitespace, quoted-string escaping, non-canonical cv
    rejection, specialization ambiguity, and round trips. Compatible
    C++20/C++23 provenance merges; incompatible classification or operator
    legality fails with `E_LANGUAGE_CONFLICT`. The `docs/CidxSignature.g4`
    artifact is the compiled signature source of truth.
20. **Composition and identity:** fixtures cover two multi-valued Include/Show
    expansions, Cartesian/left-outer row composition, an empty Show side, and
    prove that EvidenceSite is unreachable from the controlled-English grammar
    while Declaration/Entity counts remain deterministic.
21. **Evidence paths:** every evidence fixture uses normalized repository-
    relative `FileIdentity` values and verifies separator, symlink, Unicode,
    and case-preservation rules.
22. **Interfaces and overriding:** fixtures distinguish abstract classes from
    CIDX interfaces, public from non-public bases, direct from bounded
    transitive implementation, diamond-path identity, `any of`/`all of` and
    cardinality truth under incompleteness. A method overriding without the
    keyword matches; a token without a semantic override edge does not.
23. **Templates:** fixtures cover full/partial specializations, implicit and
    both explicit-instantiation origins, selected partial-specialization
    patterns, class/function/member entity kinds, structured type/non-type/
    template/pack arguments, explicit-vs-deduced/defaulted origins, positions,
    arity, wildcards, dependent/unknown arguments, name ambiguity, canonical
    names/serialization, evidence, and round trips.
24. **Affected round trips:** the normative `CidxSearch` and `CidxSignature`
    grammars compile without warnings treated as errors. Every affected query
    in `Normative capability fixtures` parses, formats canonically, reparses to
    the same typed IR, and produces identical observable results on all three
    built-in profiles.

## Relationship to the existing query API

[`docs/query-plan.md`](query-plan.md) specifies the implemented low-level
QueryPlan IR and its SQLite executor. This design sits above it. The SQLite
adapter may lower Search IR into QueryPlan, but Search IR and its semantics
remain the language contract.
