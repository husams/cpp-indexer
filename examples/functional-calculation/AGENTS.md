# Agent instructions for the functional-calculation example

This directory is a controlled C++23 corpus for evaluating automated purity and
effect proofs. Treat its intentionally pure, effectful, and ambiguous behavior
as test material rather than incidental code to clean up.

## Proof standard

A function is proven pure only when:

1. Its result depends exclusively on explicit immutable inputs or explicitly
   declared read dependencies.
2. It performs no observable mutation.
3. Every possible transitive callee is proven pure.
4. It has no undeclared dependency on mutable global state, time, randomness,
   external services, or the environment.
5. No reachable operation has unresolved effects.

Signatures, `const`, private visibility, and the absence of pointer parameters
are not purity proofs. Treat `this` as an implicit argument, preserve internal
and `mutable` cache writes in summaries, and require every possible dynamic
target to satisfy the proof standard.

## Oracle-first workflow

Before changing the analyzer or this corpus, record the expected effect summary
for each selected function:

- reads and result/control dependencies;
- writes and abstract memory regions;
- pointer/reference aliases and escapes;
- allocation, deallocation, construction, and destruction;
- exceptions and other control effects;
- synchronization, atomics, volatile access, and shared memory;
- external effects and environmental inputs;
- possible callees, unresolved operations, and assumptions.

Classify each result as:

- physically pure;
- read-only with declared dependencies;
- observationally pure under stated assumptions;
- effectful;
- unknown.

Do not alter expected classifications merely to match analyzer output. Any
change to the oracle requires an explicit semantic justification.

## First-phase boundary

The initial automated proof phase may support scalar/value inputs, locals,
branches, returns, implicit `this`, simple uniquely resolved pointer/reference
regions, available constructors/destructors, and fully resolved direct
non-recursive calls.

Virtual or indirect dispatch, callbacks, recursion, unavailable implementations,
unresolved aliases, concurrency, external services, inline assembly, dynamic
loading, undefined behavior, and unsupported C++ semantics must return
`unknown` until soundly modeled. Never approximate them as effect-free.

The primary acceptance criterion is zero false-pure conclusions. Reduced
coverage or an `unknown` result is preferable to an unsound purity claim.

## Corpus changes

- Keep examples small and deterministic.
- Add focused positive, negative, and unresolved cases in pairs where practical.
- Preserve source clarity so each expected effect has an obvious source site.
- Do not remove an intentional side effect as a refactor.
- Keep runtime tests separate from static proof claims: passing tests or traces
  are evidence, not proof of absent effects.
- State whether each conclusion is unconditional, assumption-dependent,
  heuristic, or runtime-only.

This directory refines the behavioural-proof discussion documented in
`~/workspace/wiki/pages/planning/cidx-purity-effect-proof-pilot.md`.
