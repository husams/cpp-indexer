# Graph improvements enabled by LibTooling (Clang C++ API)

Candidate improvements to the semantic graph now that indexing runs on
LibTooling with full `clang::ASTContext` access (libclang removed). Each item
is information the old libclang C API could not expose (or exposed only
partially).

## Recommendation and order

Prioritize **exact extraction facts with an immediate graph consumer**:
template arguments, dependent/overload call resolution, then a separately
modelled include/macro graph.  These are natural extensions of the existing
visitor and are easy to exercise with focused C++ fixtures.

Defer broad control/data-flow facts until `cidx analyze` has a named rule that
consumes them.  In particular, do not add all implicit calls, full expression
nodes, or generic data-flow edges merely because the AST makes them visible:
they expand the graph substantially and blur the current source-level `calls`
meaning.  Any implicit call family should first define its edge kind, site
provenance, and query semantics.

Use the Clang Index API as a **test oracle**, not as the production traversal;
see [index-api-symbol-enrichment.md](index-api-symbol-enrichment.md).  See the
full delivery order and gates in [README.md](README.md).

## Call graph

- **Implicit calls** — emit CALLS edges for implicitly invoked constructors,
  destructors, conversion operators, and defaulted/deleted members that never
  appear in source text.
- **Overload resolution results** — record the exact function chosen at each
  call site, including implicit conversion sequences applied to arguments.
- **Dependent-call resolution** — resolve calls inside template instantiations
  to concrete symbols (previously uncapturable with libclang).
- **Virtual dispatch precision** — complete override chains and
  final/devirtualizable call-site classification for dynamic-dispatch queries.

## Templates

- **Instantiated bodies** — index symbol uses inside template instantiations,
  not just the primary template's spelling.
- **Full template argument lists** — exact resolved arguments per
  specialization/instantiation via `FunctionDecl::getTemplateSpecializationArgs()`
  and `ClassTemplateSpecializationDecl::getTemplateArgs()`, including deduced
  and defaulted arguments for function/method templates.
- **Specialization enumeration** — walk
  `FunctionTemplateDecl::specializations()` /
  `ClassTemplateDecl::specializations()` for complete SPECIALIZES /
  INSTANTIATES coverage.
- **C++20 concepts** — constraint (requires-clause) edges from templates to
  the concepts they use.

## Control and data flow

- **Per-function CFG** — build `clang::CFG` for reachability, dominance, and
  intra-procedural dataflow edges (including implicit destructor calls at
  scope exit).
- **Constant evaluation** — compile-time values of constexpr/enum expressions
  via `Expr::EvaluateAs*`.
- **Full expression AST** — implicit casts, materialized temporaries, and
  default-argument expressions as first-class graph facts.

## Preprocessor

- **Macro graph** — `PPCallbacks`-based macro definition → expansion edges.
- **Include graph** — precise file-level INCLUDES edges from the same
  callbacks.
- **Conditional compilation** — record skipped `#if`/`#ifdef` regions per TU
  configuration.

## Declarations and types

- **Lambda captures** — by-value / by-ref / init-capture lists as USES edges.
- **Type layout** — sizes, alignments, and field offsets from `ASTContext`;
  distinguish sugared vs canonical types.
- **Exception specs** — computed noexcept-ness per function.
- **Full attribute coverage** — all attributes (e.g. `[[deprecated]]`,
  `[[nodiscard]]`), not the small cursor-exposed subset.
- **Coroutines** — mark coroutine functions and their transformed await/yield
  call edges.

## Notes

- Items are independent; each can land as its own scoped change with C++
  tests, a schema bump where new node/edge kinds are added, and an `index.db`
  regeneration in the same change.
