# `cidx include` — include hygiene and the dependency graph

Four verbs over the v31 include tier. The mutating boundary is explicit:

| command | reads | writes |
|---|---|---|
| `cidx include graph` | index | — |
| `cidx include check` | index | — |
| `cidx include plan` | index, source | the plan file |
| `cidx include apply` | index, source, plan | **source files** |

`apply` is the only command in cidx that edits source.

## Scope selection

Every read-only verb takes files positionally, from a list file, or both:

```bash
cidx include check src/ast/index_engine.cpp src/storage/storage.cpp
cidx include check --files-from changed.txt
git diff --name-only main | cidx include check --files-from -
```

A list file holds one path per line, absolute or relative to the current
directory. Blank lines and `#` comments are ignored, so a generated list can
carry its own provenance. `-` reads stdin. With no scope at all, the whole
index is analyzed.

## The rule

For a direct include edge `S → H`:

```
unused_by_reference(S, H) := Refs(Owners(S)) ∩ Symbols(H) = ∅
```

- **`Owners(S)`** — every symbol declared or defined in `S`. A reference inside
  a function body or a local initializer belongs to its enclosing callable, so
  local and global contexts are both covered.
- The verdict is **reference-only**: nothing outside this intersection may
  reclassify a zero-reference finding as `used`. The most common include in C++
  — a `.cpp` including its own header — is genuinely unused *by reference*:
  `int f();` in `u.hpp` and `int f() {…}` in `u.cpp` are **one symbol**, in both
  `Owners(S)` and `Symbols(H)`, and it never references itself. That is a true
  finding but **not** an automatically removable one: removing the directive
  still *compiles* (a definition does not need its declaration), so the compile
  gate cannot catch it either. The declaration/definition overlap
  `Owners(S) ∩ Symbols(H) ≠ ∅` is therefore a **safety caveat** that downgrades
  the finding to `manual_review` — it never makes the include `used`.
- **`Symbols(H)`** — every symbol declared or defined **directly** in `H`.
  Symbols from headers that `H` itself includes are **not** counted. This is
  what makes "used through a transitive provider" not count as using the direct
  include.
- **`Refs`** — three sources unioned:
  1. the targets of every persisted semantic edge (calls, uses, inherits,
     overrides, construct-\*, destroy, friend, specializes, instantiates,
     field_of, method_of, dispatch_calls);
  2. the `def_edge` targets of every body **defined in this file** (see
     [USR collapse](#usr-collapse-and-def_edge) below);
  3. the signature tier's type closure: return types, declared types, underlying
     types, parameter types, and the structural closure over them. That closure
     is why `void f(const Foo&);` — a declaration with no body and no call —
     counts as using `Foo`, and why `vector<Foo>`, `Foo*`, and an alias of `Foo`
     all do too.

### USR collapse and `def_edge`

`symbol.usr` is `UNIQUE`, so symbols sharing a USR across translation units
collapse onto **one row**. `int main(int, char**)` is `c:@F@main#I#**C#` in
every TU that defines it, so a project with eight `main`s keeps one — and
re-indexing each TU deletes the previous TU's edges and writes its own, last
writer wins.

A file whose symbols and edges were collapsed away has no owners and no
references, which makes **every one of its includes look unused**. cidx's own
`src/main.cpp` was exactly this case.

`Owners(S)` therefore also unions the v27 `definition` table, and `Refs`
unions the v27 `def_edge` table — both are keyed per **body**, not per symbol,
so they survive the collapse. `def_edge` only carries body calls/uses, so the
`edge` query is still needed for declaration relations (inherits, method_of,
field_of, friend, specializes); `Refs` uses both.

For a collapsed symbol the `edge` half may contribute *another* TU's edges. That
over-approximation is deliberate and safe: it reports **fewer** unused includes,
so a cleanup is missed rather than a working include deleted.

The exact list is recorded in every plan as `reference_kinds_searched`, so a
zero-reference claim always states the search space it was made against.

## Compilation is a separate gate, not the definition

A zero-reference result is a claim about the symbol graph. It is **not** a proof
that removing the directive is safe. `cidx include` therefore keeps three states
strictly apart, and never conflates them:

| state | meaning |
|---|---|
| `unused_by_reference` | `Refs(Owners(S)) ∩ Symbols(H) = ∅`. A true claim about references. |
| `validated_for_apply` | Also compiles, in every affected TU under every recorded configuration. |
| `manual_review` | A real finding with no sound automatic proof. **Never applied.** |

### What compilation cannot prove

Compile success is **not** behavioral equivalence. A header removed for a static
registration, a configuration-changing macro, or a pragma can compile perfectly
and still change the program. Those stay `manual_review`. Every plan states this
in its own `limitations` field, so a reviewer reads it without consulting these
docs.

Coverage is also bounded by what was indexed: a build configuration cidx has
never seen is not covered by anything here.

### Known false positives

The reference rule is not perfect, and the validation gate is what catches the
remainder. A **catch-clause type** (`catch (const CidxError &)`) is not recorded
as an edge, so a header supplying only an exception type reads as unused. On
cidx's own tree that finding is produced — and correctly `rejected`, because
removing the header does not compile. This is the two gates working as intended:
the reference rule proposes, the validator disposes.

## What is never removed automatically

These are the cases where a removal looks safe and is not:

- **Macro-only providers.** A header supplying only a macro has zero symbol
  references and compiles away cleanly — it looks exactly like an unused
  include. The recorder tracks macro expansions back to their defining header,
  and any such dependency forces `manual_review`.
- **X-macro headers.** When an **unguarded** header is included more than once
  in a file, every one of its directives is load-bearing — including the first.
  The first occurrence's declarations are typically referenced by nothing and
  the file compiles without it, so neither the reference rule nor the compile
  gate catches it. Guardedness is the only sound gate, and a repeat of an
  unguarded header disables automatic removal for all of its occurrences.
- **Conditional regions.** An include inside `#if` is only ever proven for the
  configuration that lexed it. Clang never lexes untaken branches, so cidx has
  no facts about them at all.
- **Transitive providers.** `middle.hpp` can be genuinely unused by reference
  and still be the only thing that drags in `leaf.hpp`. The finding is true; the
  validator refuses the edit.
- **System and unowned headers.** Their internals are not indexed, so
  `Symbols(H)` is empty and every include of them would look unused.
- **`#include_next` / `#import`.** Search-path- and contract-sensitive.

## Duplicates

A repeated directive is an automatic duplicate candidate only when the same
source file, configuration, **and conditional region** already reached the same
target, *and* the target is multiple-include guarded (`#pragma once` or a
recognized include guard). The first directive is always the one kept.

Different `#if` branches are never duplicates of each other.

## Plans

```bash
cidx include plan --output cleanup.json      # --output is required
cidx include apply cleanup.json --dry-run
cidx include apply cleanup.json
cidx include apply cleanup.json --only 3f2a91bc4d0e,7c11de92aa03
```

A plan is an **immutable snapshot**. It records the exact bytes to remove, the
evidence behind each verdict, and the identity of everything it derives from:
source content hashes, compile-configuration digests, and the index schema
version. `apply` refuses a plan whose world has moved — **there is no
force-through-staleness path**. Regenerate and review again.

Candidate ids are stable across runs and machines (`sha1(repo-relative path,
byte offset)`), which is what makes `--only` reviewable in a PR.

A plan is **data, never code**. Nothing in it is executed and no field is ever
interpreted as a command; it is parsed with a strict JSON reader that rejects
rather than guesses.

### How a plan is proven

1. Each candidate is validated against an overlay that already contains every
   previously accepted removal. That sequencing is what makes "two individually
   redundant providers, but not both" come out right.
2. The whole accepted set is then proven **together**. If the combined proof
   fails, every accepted item is demoted rather than shipping a plan whose
   combined proof failed.
3. At `apply` time everything is revalidated from scratch. The plan's recorded
   validations are evidence of what *was* true, never a substitute for proving
   it now.
4. `apply` proves the **exact post-format bytes** it is about to write, not the
   pre-format bytes it planned — a proof of different bytes is not a proof.
5. Nothing is written until every buffer has passed. Writes are then staged
   through a temporary sibling and renamed, so a crash mid-run cannot leave a
   half-written source file.

### Formatting

`apply` discovers the nearest `.clang-format` and runs
`cleanupAroundReplacements` over the deletion, which tidies what the deletion
itself left behind (stranded blank lines).

It deliberately does **not** run the include sorter or a general reformat.
Deleting whole directive lines cannot unsort an already-sorted include block or
disturb any line it leaves behind, so the style is already honored; running the
sorter over a file that was never sorted would rewrite the whole block and bury
a one-line edit under an unrelated diff.

### After applying

Edited files and everything that includes them are marked pending. Run
`cidx index` to refresh the index. `apply` never commits, stages, or pushes.

## Graph queries

```bash
cidx include graph                                   # every direct edge
cidx include graph src/cli/commands.cpp              # what it includes
cidx include graph src/storage/storage.hpp --reverse # who includes it
cidx include graph src/x.hpp --reverse --transitive  # impact set of a change
cidx include graph --cycles                          # include cycles (SCCs)
cidx include graph --format dot | dot -Tsvg > deps.svg
cidx include graph --system                          # keep system targets
```

`--depth N` bounds `--transitive`. Text, JSON, and DOT output are all stably
ordered.

## The stored graph (schema v31)

Preprocessing facts live in their own file domain — `edge` is symbol→symbol and
cannot hold a file→file relation.

| table | grain |
|---|---|
| `include_config` | one normalized compile configuration per TU; `digest` is the freshness identity |
| `include_edge` | one collapsed row per (source, resolved target, configuration) |
| `include_site` | one row per directive: position, exact byte range, spelling, angled, kind, conditional-region fingerprint, guard status |
| `include_macro_use` | a macro expanded in a source and the header that defines it |

Extraction is C++-only (LibTooling `PPCallbacks`); Python owns the schema and
migration.

**An upgraded database has an empty include graph until `cidx index` reruns** —
preprocessing facts cannot be backfilled from stored rows. Every `cidx include`
verb refuses outright in that state rather than reporting zero findings, because
"no unused includes" would be a vacuous truth that reads exactly like a clean
bill of health.

## Deviations from the plan

- **SHA-1, not SHA-256**, for file hashes. `sha1_hex` is the house hashing
  primitive and is already vendored; the plan also requires no new third-party
  dependency. The hash detects drift, and `apply` revalidates everything
  regardless, so collision resistance is not the property being relied on.
- **`include_macro_use` is a fourth table** beyond the three the plan lists. The
  plan's own algorithm (step 6) requires macro dependencies, and they are the
  single most important reason a zero-reference finding must not be executed.
