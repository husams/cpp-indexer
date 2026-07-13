# Recommended LibTooling improvement roadmap

The LibTooling port is now the sole indexing path.  The next work should favour
facts with a clear query consumer and a small, independently testable storage
contract; it should not turn normal indexing into an analysis framework.

## Recommendation

1. **First: complete high-confidence extraction fidelity.**  Ship precise
   template arguments and dependent/overload-call targets where the typed AST
   supplies an exact answer.  Add preprocessor callbacks for the include and
   macro-expansion graph only after agreeing the file-level and macro identity
   model.  Each change should be separately schema-versioned, migrated, tested
   in C++, and followed by an `index.db` refresh.
2. **Second: add the Clang Index API as a test oracle.**  Compare the
   declarations and references it reports with cidx's extracted facts on
   fixtures.  This detects coverage regressions without making its coarser
   relation model or ordering part of production indexing.  Defer persisted
   `SymbolInfo` columns until a query or rule needs a specific field.
3. **Third: make CFG analysis an opt-in `cidx analyze` capability.**  Start
   with per-function metrics and a narrowly defined call-site conditional flag.
   Do not persist basic blocks, dominance trees, generic unreachable-statement
   facts, or a must-call relation until a named analysis consumes them and its
   soundness rules are specified.  Implicit destructor calls need a separate
   edge-model decision so they cannot be mistaken for source-level calls.
4. **Do not adopt `clang::cross_tu` now.**  The stored USR graph already
   satisfies cross-translation-unit *identity* queries.  Reconsider CTU only
   for a concrete body-sensitive rule that has passed the intra-procedural CFG
   stage, as an opt-in prepass with explicit disk, import, and memory budgets.

## Required gates

- Preserve deterministic output for unchanged facts; add focused C++ fixtures
  for every new fact and its negative cases.
- For a schema change: bump both storage schema versions, provide a migration
  and old-database test, then regenerate and verify the committed `index.db`.
- Measure indexing wall time and peak RSS on a representative corpus before
  enabling an analysis by default.  CFG and CTU work remain opt-in unless that
  measurement demonstrates an acceptable default cost.

See [libtooling-graph-improvements.md](libtooling-graph-improvements.md),
[index-api-symbol-enrichment.md](index-api-symbol-enrichment.md),
[cfg-analysis.md](cfg-analysis.md), and
[crosstu-analysis.md](crosstu-analysis.md).
