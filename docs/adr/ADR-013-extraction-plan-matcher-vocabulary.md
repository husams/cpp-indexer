# ADR-013: ExtractionPlan matcher vocabulary — reuse Clang's dynamic AST matcher registry

- Status: accepted
- Date: 2026-07-25
- Scope: HSE-64 declarative AST extraction DSL and `ExtractionPlan` IR
- Related: HSE-57, HSE-61, HSE-63, HSE-65, HSE-66

## Decision

The `ExtractionPlan` matcher vocabulary is Clang's own dynamic AST matcher
registry (`clang::ast_matchers::dynamic::Registry` /
`clang::ast_matchers::dynamic::Parser`, the same engine behind `clang-query`),
not a bespoke CIDX matcher grammar/engine. CIDX wraps the registry with three
layers the registry does not provide on its own:

1. **An explicit allow-list catalog.** A rule's matcher tree is rejected before
   Clang ever sees it unless every matcher id and narrowing predicate it uses
   appears in a CIDX-declared allow-list (`src/extract/matcher_catalog.*`).
   The allow-list is deliberately narrower than the registry's full surface:
   it admits declaration/expression/type matchers and bounded structural
   narrowing (`hasAncestor`, `hasDescendant`, `hasType`, `callee`, ...) needed
   for fact emission, and excludes matcher families with no CIDX identity or
   evidence story (e.g. raw `stmt()`/`expr()` catch-alls with no bound parent
   scope, matchers that only make sense inside clang-query's REPL).
2. **A binding-to-identity boundary.** The registry's native result is a bound
   `clang::ast_type_traits::DynTypedNode`, i.e. a process-local AST handle.
   CIDX never stores or serializes that handle. Every binding is converted at
   match time, before an emit operation runs, into one of the safe identity
   primitives (declaration USR, source anchor, owner/position key, type key,
   or a deterministic composition of those) required by the outcome
   statement. A raw AST pointer or its address can never reach a plan's
   identity or an emitted fact.
3. **Its own traversal/budget wrapper.** The registry evaluates a matcher
   against whatever `ASTContext::getTraversalKind()` a session already has;
   CIDX sets `TK_AsIs` or `TK_IgnoreUnlessSpelledInSource` explicitly per plan
   scope and meters visits/binds/emits through the same `PassBudget`/
   `PassMetrics` contract the HSE-63 pass registry already uses, so a rule
   cannot silently inherit whatever mode the last consumer left behind.

## Rationale

- The registry is mature, versioned with the exact Clang release CIDX already
  links, and is exercised continuously by `clang-query` upstream. Reimplementing
  parsing, overload resolution between matcher argument kinds (integer,
  string, matcher-of-kind, boolean), and narrowing-matcher semantics from
  scratch would duplicate hundreds of matcher definitions and drift from
  upstream Clang on every LLVM upgrade — a maintenance cost with no
  corresponding safety benefit, since the registry itself performs no I/O,
  spawns no process, and has no callback hook for arbitrary code.
- Engineers who already know `clang-query`/Clang AST Matchers transfer that
  knowledge directly; CIDX's syntax is the same textual matcher expression
  grammar, just validated against a smaller, explicit vocabulary and require
  named bindings on every node an emit operation references.
- The registry's failure mode for an unknown matcher/argument name is already
  a structured `Diagnostics` object (id + argument index + message); CIDX
  captures that and folds it into the same `ValidationError` produced by the
  allow-list/binding/budget checks, so "unknown matcher" and "matcher not on
  the CIDX allow-list" surface through one validation contract regardless of
  which layer rejected it.
- The alternative — a restricted CIDX-only parser covering a hand-picked
  matcher subset — was considered and rejected for this iteration. It removes
  none of the required safety work (the allow-list, identity boundary, and
  traversal/budget wrapper above are unavoidable either way) while adding a
  second grammar/AST-shape mapping to hand-maintain and re-verify on every
  Clang upgrade. It remains available as a future fallback if a CIDX-specific
  extension to the vocabulary (identity-aware combinators, for example)
  outgrows what the registry's argument model can express; nothing in the
  `ExtractionPlan` IR below assumes registry-specific storage, so swapping the
  construction step later does not change the IR or its consumers.

## Consequences

- CIDX's supported matcher surface tracks upstream Clang AST Matchers,
  intersected with the CIDX allow-list; growing the vocabulary is an
  allow-list change plus a binding/identity rule, not a grammar change.
- `ExtractionPlan` validation runs in two passes: syntactic/registry parsing
  (can this expression construct a matcher at all) and CIDX policy validation
  (is every matcher/property on the allow-list, are bindings well-typed against
  declared endpoint domains, is identity stable, is scope/budget bounded).
  Both must pass before a plan is ever run against a `FrontendSession`; neither
  pass touches source, spawns a process, or loads a shared library.
- Because bound nodes are converted to safe identities immediately and the
  registry is read-only, "rules cannot execute arbitrary SQL, shell, Python,
  shared libraries, or user C++ callbacks" is a structural property of this
  design, not solely a validation-time check.
