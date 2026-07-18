# ADR-010 — include hygiene: the zero-symbol-reference rule and separate execution safety levels

Date: 2026-07-17 · Status: accepted · Version: cidx 0.53.0 (schema v31)

## Context

`cidx include` reports duplicate and unused `#include` directives and can remove
them. The whole feature turns on one question: **what does "unused" mean, and
what licenses actually deleting the line?**

Getting this wrong is not a cosmetic failure. The tool edits source, and the
dangerous outcome is not a crash — it is confidently deleting an `#include` that
was doing something the symbol graph cannot see, in a tree that still compiles
afterwards. A wrong answer here is silent.

Two tempting definitions were rejected:

1. **"Unused means removing it still compiles."** This is the definition most
   existing tools use, and it is wrong in both directions. It calls a transitive
   provider *used* (it compiles only because of it) while calling an X-macro's
   first include *unused* (it compiles fine without the declarations it was
   there to produce). It also makes the answer depend on which configurations
   happen to be indexed.
2. **"Unused means no symbol from the header appears in the file's text."** Text
   search cannot see ADL, implicit conversions, template instantiation, or a
   type reached through an alias.

## Decision

### 1. The rule is a set intersection over the persisted graph

For a direct include edge `S → H`:

```
unused(S, H) := Refs(Owners(S)) ∩ Symbols(H) = ∅
```

`Symbols(H)` counts only what `H` declares or defines **itself**. `Refs` spans
every persisted `edge_kind` with no kind filter — so a relation the indexer
learns later is covered automatically rather than silently missed — plus the v30
signature tier's type closure, which is what makes a body-less `void f(const
Foo&);` count as using `Foo`.

### 2. Compilation is an apply-safety gate, NOT the definition

The two are separate stages producing separate states, and they are never
conflated:

- `unused_by_reference` — a true claim about references.
- `validated_for_apply` — also compiles, in every affected TU under every
  recorded configuration.
- `manual_review` — a real finding with no sound automatic proof; never applied.

This separation is the ADR's core. It lets `check` report a true finding that
`apply` will nonetheless refuse to execute (the transitive-provider case), and
it forces the tool to state what it has and has not proven.

### 3. Compile success is explicitly not behavioral equivalence

A header removed for a static registration, a configuration-changing macro, or a
pragma can compile perfectly and still change the program. Every plan carries a
`limitations` field saying so in plain language, so a reviewer sees the caveat
without reading these docs. The tool must never report compile success as
absolute safety.

### 4. Guardedness, not compilation, gates re-inclusion

When an **unguarded** header is included more than once in a file, every one of
its directives is load-bearing — **including the first**. This was found by
testing, not by design: the first include of an X-macro header has zero symbol
references and compiles away cleanly, so the reference rule and the compile gate
had a hole in exactly the same shape. Neither could catch it. Only
`#pragma once` / include-guard status can.

### 5. An empty include graph refuses rather than reports zero

Preprocessing facts cannot be backfilled, so a database upgraded to v31 has an
empty include graph until `cidx index` reruns. Reporting "no unused includes"
there would be a vacuous truth that reads exactly like a clean bill of health —
the most dangerous possible answer. Every verb refuses instead.

### 6. Plans are immutable snapshots with no force path

A plan records source hashes, configuration digests, and the schema version.
Any drift and `apply` refuses. There is deliberately no `--force`: the artifact
describes byte-level edits to source, and a stale one describes edits to a file
that no longer exists as reviewed.

`apply` revalidates from scratch, proves the **combined** set rather than each
edit alone, and proves the exact **post-format** bytes it will write — a proof
of different bytes is not a proof.

### 7. The plan is data, never code

Nothing in a plan is executed; no field is interpreted as a command. It is read
with a strict parser (`util/json_read`) that rejects rather than guesses.

## Consequences

- **`check` reports findings `apply` will refuse.** This is intended, and the
  report says why. A transitive provider is genuinely unreferenced.
- **The tool is conservative by construction.** Macro-only headers, X-macros,
  conditional regions, system headers, unowned targets, `#include_next`, and
  `#import` are all report-only. Users will see `manual_review` often. That is
  the correct failure direction: a missed cleanup costs nothing, a wrong
  deletion costs a broken build or a silent behavior change.
- **Coverage is bounded by the index.** A configuration cidx has never indexed
  is not covered, and the plan says so rather than implying whole-program
  proof.
- **A new `edge_kind` is picked up automatically** by the unfiltered `Refs`
  query, but a new *type* relation needs `symbols_named_by_types` extended.

## Alternatives considered

- **Compile-based unused detection** (include-what-you-use style). Rejected per
  Context; it conflates the claim with the gate and cannot express
  "unreferenced but required".
- **A single `unused` state.** Rejected: it forces the tool to either
  under-report (only what it can prove removable) or over-promise (report
  findings as safe). Three states let it be both complete and honest.
- **`sortIncludes` on apply.** Rejected: rewriting a never-sorted block would
  bury a one-line edit under an unrelated diff. Deleting whole lines cannot
  unsort an already-sorted block, so the style is honored without it.
- **SHA-256 file hashes** (as the planning page specified). Rejected in favor of
  the already-vendored `sha1_hex`, since the planning page also requires no new
  third-party dependency. The hash detects drift and `apply` revalidates
  everything regardless, so collision resistance is not load-bearing.
