# Symbol enrichment via the Clang Index API (`clang::index`)

`clang::index` is the C++ indexing library behind clangd and Apple's
IndexStore. We do **not** adopt its traversal (`IndexDataConsumer`) — our
visitors are richer and validated at scale. Instead we use it narrowly, as a
classification library called from the existing symbol-record path.

## Recommendation

Adopt proposal 3 first: use `clang::index` as a fixture-level coverage oracle
for declarations and references.  It provides a low-risk regression signal
without changing indexing order, the on-disk schema, or the richer cidx edge
semantics.

Defer persisted sub-kind/property columns until a concrete query needs them.
`getSymbolInfo()` is cheap to call later, but every stored field becomes a
schema and compatibility commitment.  Likewise, treat macro identity as part
of a dedicated preprocessor graph design: `generateUSRForMacro` is useful only
once `PPCallbacks` supplies definitions and expansions to store.  It should
not be introduced as an isolated symbol change.

See the project-wide ordering and gates in [README.md](README.md).

## What it gives us

- **`index::getSymbolInfo(Decl)`** — one call returns symbol kind, sub-kind,
  property bitset, and language; the mapping is maintained upstream by the
  Clang project, so it tracks new C++ features for free.
- **Fine-grained kinds** — distinguishes constructor / destructor /
  conversion-function, static method, class vs instance property, enum
  constant, type alias vs typedef — finer than a plain `Decl::getKind()`
  switch.
- **Sub-kinds** — `CXXCopyConstructor` / `CXXMoveConstructor`, accessor
  getter/setter, and similar distinctions we would otherwise detect by hand.
- **Property bitset** — ready-made symbol attributes: `Generic` (is a
  template), `TemplateSpecialization`, `TemplatePartialSpecialization`,
  `Local`, `UnitTest`.
- **Language tag** — C vs C++ vs Objective-C per symbol, useful for mixed
  codebases.
- **USR generation** — `generateUSRForDecl` / `generateUSRForMacro` for
  stable cross-TU symbol identity, including macros.

## Proposed changes

1. **Enrich symbol records** — in the symbol-record path, call
   `index::getSymbolInfo()` and store sub-kind and properties as new columns
   (schema bump + migration + old-database tests + `index.db` regeneration).
2. **Macro USRs** — use `generateUSRForMacro` to give macros first-class,
   stable identity in the graph.
3. **Test oracle (no schema impact)** — in the C++ test suite, run a
   `clang::index` pass over fixture TUs and assert our graph is a superset of
   its declaration/reference occurrences; catches silently dropped symbols.

## Why not adopt it as the indexer

- Its relation set (call-of, base-of, override-of, contained-by) is coarser
  than our edge schema (e.g. USES_NAMESPACE, USES_DECLARATION,
  INSTANTIATES vs SPECIALIZES).
- Roadmap items (CFG facts, overload-resolution detail, expression-level
  info) require the raw AST — the Index API would be a ceiling, not a
  foundation.
- Its occurrence ordering differs from our traversal, so switching would
  churn `index.db` output for no semantic gain.

See also: [libtooling-graph-improvements.md](libtooling-graph-improvements.md).
