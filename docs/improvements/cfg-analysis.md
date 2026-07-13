# Control-flow analysis via `clang::CFG`

`clang::CFG` builds an intra-procedural control-flow graph from any statement
body: free functions, methods, constructors/destructors (including member
init and implicit destructor cleanup), lambdas, and blocks. It can also be
built over a standalone `Stmt` (e.g. a global variable's initializer). There
is no whole-program CFG — functions are linked through the existing call
graph.

## Recommendation

Do not build a CFG during normal indexing yet.  Introduce it as an opt-in
`cidx analyze` facility, beginning with per-function metrics and one narrowly
specified call-site fact (for example, whether a call is control-dependent on
a branch).  Measure wall time and peak RSS on a representative corpus before
making any CFG work default.

Defer persistent basic blocks, dominance trees, generic unreachable-code
facts, and the `must-call` relation.  They require a precise contract for
exceptions, non-termination, indirect calls, and unknown callees; without
that contract they can look stronger than they are.  Model implicit destructor
invocations separately from source-level `calls` so callers can distinguish
cleanup effects from explicit calls.

See the complete priority order in [README.md](README.md).

## What it gives us

- **Basic blocks and edges** — statements grouped into blocks with
  successor/predecessor edges, including branch conditions.
- **Implicit destructor calls** — CFG elements include automatic destructor
  invocations at scope exit, making cleanup order explicit.
- **Reachability** — unreachable statements and never-taken branches.
- **Dominance / post-dominance** — "must A execute before B" queries
  (via `CFGDominatorTree`).
- **Dataflow foundations** — liveness, uninitialized reads, use-after-move
  style analyses (Clang ships `LiveVariables`, `UninitializedValues`).
- **Exception / early-exit paths** — which calls can be skipped by a throw or
  early return.

## Proposed graph improvements

1. **Call-site context flags** — annotate CALLS edges with CFG-derived
   context: conditional (not on every path), in-loop, on-error-path
   (throw/early-return branch). Cheap to compute during indexing; makes
   "who calls X unconditionally" queries possible.
2. **Implicit destructor CALLS edges** — emit calls to destructors invoked at
   scope exit; today these are invisible in the graph.
3. **Unreachable-code facts** — mark symbols/statement extents proven
   unreachable within their function (feeds `cidx analyze` dead-code rules).
4. **Function-level metrics** — per-function block count, cyclomatic
   complexity, max loop depth as symbol attributes for complexity queries.
5. **Must-call relation (later)** — dominance-based "f always calls g" edges
   for stronger reachability reasoning in Souffle analyses.

## Cost and scoping notes

- CFG construction is per-function and allocation-heavy; build it on demand
  during the visitor pass and discard immediately — do not retain CFGs.
  Validate RSS stays bounded on the llvm-project corpus before landing.
- Items 1–2 are edge/attribute additions (schema bump + migration +
  `index.db` regeneration); 3–4 are symbol attributes; 5 needs design work.
- Start with `CFG::BuildOptions` defaults plus `AddImplicitDtors`; enable
  more elements (`AddTemporaryDtors`, lifetime) only when a rule needs them.

See also: [libtooling-graph-improvements.md](libtooling-graph-improvements.md).
