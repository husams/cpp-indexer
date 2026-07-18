# cidx-diff contract (v1)

Status: implemented (M0–M2 slice) — C++ `src/diff/`, executable `cidx-diff`.
Design source: wiki `pages/planning/cidx-semantic-diff` (syntax-aware and
semantic source diff). This tool is C++-only; the Python tree has no parallel
implementation.

`cidx-diff` compares two C/C++ source targets — whole file, class/struct/union,
free function, or method — using the existing SQLite `index.db` **read-only**
as its configuration and selection authority. Each side is reparsed with
Clang LibTooling under the exact compile options, driver, and include aliases
recorded in the index. The index is never written; the database is
byte-unchanged after every comparison.

Three modes:

- `syntax` — typed, source-mapped AST edit operations (whitespace and comments
  ignored), not line edits;
- `semantic` — normalized behavior/API summaries with a tri-state
  `equivalent | different | unknown` verdict;
- `both` (default) — how the syntax changed, and whether a behavioral
  consequence could be established.

`unknown` is a normal successful result. Two snippets are never called
semantically equivalent merely because a normalized AST happens to match;
every `equivalent` verdict names its evidence level and assumptions.

## CLI

```text
cidx-diff file  LEFT_FILE RIGHT_FILE [options]

cidx-diff symbol LEFT_FILE RIGHT_FILE
  --left  LEFT_SELECTOR --right RIGHT_SELECTOR
  [--kind class|struct|union|function|method] [options]
```

Options (both subcommands unless noted):

```text
--mode syntax|semantic|both     default: both
--db PATH                       one index for both sides
                                (default: $INDEXER_CACHE or ~/.cache/cidx, + /index.db)
--left-db PATH --right-db PATH  separate indexes/workspaces (override --db per side)
--left-tu PATH --right-tu PATH  registered TU providing compile context for a header
--match strict|heuristic        default: heuristic (file scope pairing)
--json                          machine-readable report (deterministic)
--context N                     source-context lines in text output (default 0)
-h/--help, --version            shared cidx kVersion
```

Exit codes follow the repo contract: `0` success (including `different` /
`unknown` verdicts), `2` usage error (unknown flags, missing arguments,
via `UsageError`, message printed verbatim), `1` operational error
(`error: <msg>` on stderr): unregistered file, unsupported schema version,
parse failure, ambiguous or unmatched selector. A parse that emits any
error-level diagnostic fails with
`error: cannot parse <file>: <first diagnostic>` — verdicts are never
derived from recovery ASTs. Clang diagnostics are captured into the error
message rather than printed with source context; only ClangTool's one-line
`Error while processing <file>.` notice may still appear on stderr.

### Configuration resolution (per side)

1. Open the side's `index.db` **read-only** (`Storage` read-only mode:
   `SQLITE_OPEN_READONLY`, no `migrate()`, no schema script, no mkdir). The
   stored `schema_version` must equal the built `kSchemaVersion`; otherwise
   error (a read-only open cannot migrate).
2. Resolve the source to its registered `file` row via
   `pathutil::abspath` + `Storage::get_file`. Unregistered → error naming the
   path and DB.
3. Options pipeline, exactly as `cidx-astgraph`:
   `CompileDb::resolve_options(CompileDb::sanitize(rec->compile_options.value_or({})), get_alias)`.
4. Stored `driver` + `Toolchain::toolchain_flags(is_cpp, driver)` +
   `-ferror-limit=0`; `-resource-dir CIDX_CLANG_RESOURCE_DIR` injected first.
5. A header with no stored compile options requires `--left-tu`/`--right-tu`
   naming a registered TU; the TU's options are used and the diff scope is
   restricted to declarations spelled in the header. A header without options
   and without an explicit TU → error.
6. Symbol selectors and extents resolve in the configured parse (not the DB).

### Selectors

A selector matches, in priority order (first tier with ≥1 candidate wins):

1. exact USR;
2. exact qualified signature — `qual_name(params) quals` as printed by the
   tool (whitespace-insensitive comparison);
3. exact qualified name (plus `--kind` filter when given);
4. `line:N` — the entity whose extent contains source line N (innermost).

More than one candidate in the winning tier → error listing each candidate's
kind, qualified signature, USR, and extent. The tool never silently picks an
overload.

## Matching (file scope)

Top-level entities (and members within a matched class) are paired:

- `usr` — identical USR (confidence 100);
- `signature` — identical kind + qualified signature (confidence 95);
- `name` — identical kind + qualified name, unique on both sides
  (confidence 85);
- `fingerprint` — `--match heuristic` only: same kind, unique identical
  structural fingerprint on both sides — a rename (confidence 70).

`--match strict` uses only `usr` and `signature`. Every pair carries its
match tier and confidence in the report; unpaired entities are
`added`/`removed`. A whole-file comparison never claims translation-unit
equivalence merely because all indexed top-level declarations matched.

## Syntax model

Each side's target subtree is lowered to a typed node tree: node kind
(Clang AST class or decl kind), salient label (name, operator spelling,
literal value, resolved callee, cast kind, type spelling), source range, and
a deterministic structural fingerprint (SHA-1 over kind/label/children).
Whitespace and comments are invisible to this tree. Within a matched pair the
edit script is produced by recursive descent with longest-common-subsequence
alignment of child sequences by fingerprint.

Enum nodes carry scoped-ness (`enum` vs `enum class`) and the explicit-or-
computed underlying type in their label; every enumerator label carries its
computed value, so an implicit renumbering is a visible change. Unnamed-type
spellings drop the file path everywhere a type is printed (labels,
signatures, profile rows): `(unnamed struct at <path>:<l>:<c>)` becomes
`(unnamed struct at <l>:<c>)`, so identical declarations in differently
named files compare equal. Hidden friends lower to their wrapped
declaration's subtree (never an empty `Friend` node).

Edit operations (`op` values): `added`, `removed`, `replaced`, `changed`
(same node kind, differing label — e.g. callee `reserve` → `resize`, literal
or operator change), `renamed` (declarations). Each op reports the node kind,
a human `detail`, and left/right source ranges. Alignment is bounded: child
sequences longer than 512 fall back to position-wise pairing, and the report
sets `truncated: true` past 1000 ops. The op cap is shared across the whole
report and bounds only the listed operations, never the truth: each pair's
`syntax.status` comes from its structural fingerprints (`changed` iff the
subtrees differ, even when its ops were suppressed by the cap — the pair then
sets its own `truncated: true` and may list fewer or zero edits), and the
aggregate `syntax.status` is `changed` iff any pair's subtrees differ.

## Semantic model

### Callables

Both bodies are lowered through `clang::CFG` into a normalized textual
Behaviour IR (the seed of the shared Behaviour IR in the behavioural-proof
plan): CFG blocks in CFG order with successor lists, per-element typed
operations with canonical types, resolved call/constructor/destructor
targets (USR), reads/writes, returns, throws, allocations. Normalizations
applied are only those whose side conditions always hold:

- alpha-renaming: parameters `%p0…`, locals `%l0…` in first-appearance order
  (shadowing preserved by distinct ids; a rename that changes type or
  capture is not erased);
- non-semantic wrapper erasure: `ParenExpr`, `ExprWithCleanups` framing,
  no-op implicit casts (`NoOp`, `LValueToRValue` positions are kept in the
  operation stream); value-changing casts keep their cast kind;
- nothing else. No commutative reordering, no loop rewriting (M3+).

Constructs outside the supported subset add an `unsupported` marker with a
source range instead of being approximated: inline assembly, `volatile` or
atomic accesses, `try`/`catch`, coroutines, `goto` (conservatively, any goto),
statement expressions, dependent (uninstantiated template) code, lambdas,
`static`/`thread_local` locals, varargs.

Callable verdict:

- `equivalent` — identical canonical signature and identical normalized IR
  with **zero** unsupported markers on either side; evidence
  `identical-source-and-config` when the raw source slices and the effective
  configuration are identical, else `normalized-ir`;
- `different` — deterministic observable-contract contradiction: return
  type, parameter count/types, default arguments (presence or normalized
  expression), cv/ref qualifiers, `noexcept`, resolved virtual-ness, storage
  class (`static`/`extern`), spelled `inline`, constexpr kind
  (`constexpr`/`consteval`/`constinit`), `[[noreturn]]`,
  static-vs-instance method, or — for methods — member access
  (`public`/`protected`/`private`), `explicit`, `= delete`, `= default`,
  `final`, pure-virtual, or `override` differ (evidence
  `summary-contradiction`);
- `unknown` — IR differs without a contradiction, or any unsupported marker
  (evidence `unsupported-or-incomplete`). Reported as
  `unknown (no behavioral difference established)` — never as equivalent.

A callable's body binds through its TU-wide definition: an out-of-line
member body defined elsewhere in the parsed file is compared even when the
selected class only declares it. A matched pair with no body on either side
stays `equivalent` when the signatures match, but says so transparently with
detail `declaration only (no body in this translation unit)` (identical
slices under an identical configuration keep `identical-source-and-config`
evidence).

### Classes / structs / unions

A class semantic profile — an observable structural contract, not a proof:
ordered bases (access, virtual), ordered fields (name, canonical type,
bitfield width, mutable), method set (canonical signature, access, virtual /
override / final / pure, static, deleted/defaulted), polymorphism and
abstractness, template parameters, nested records, plus a sorted declaration
set: friend rows (`friend <signature>` for functions, `friend <type>` for
types), class-scope using-declarations (`using <qualified name>`), type
aliases (`alias <name> = <type>`), static data members
(`static <name> : <type>`), `static_assert <fingerprint>` rows, a
`layout size:<N> align:<M>` row for complete non-dependent records, and a
`final` row. A class-scope member declaration kind the profile cannot
express attaches an `unhandled member declaration kind <K>` unsupported
marker, degrading the class verdict to `unknown` instead of a silent
`equivalent`. Hidden friends with bodies are also compared as callable
member entities. Profile contradiction →
`different` (`summary-contradiction`). Identical profile → matched method
bodies compared individually; all equivalent and nothing unsupported →
`equivalent`, else `unknown`.

### Files

Aggregates entity matching and per-entity verdicts. `different` if any
entity was added/removed or any matched entity is `different` (API surface
contradiction). `equivalent` only when every entity matched, every matched
pair is `equivalent`, and no unsupported markers exist anywhere (formatting-
and comment-only edits land here). Otherwise `unknown`.

When both sides collected zero entities (macro- or pragma-only files, empty
files), the trees prove nothing: the pair is `equivalent`
(`identical-source-and-config`) only when the two files' raw bytes are
identical and the configuration delta is identical; otherwise `unknown`
(`unsupported-or-incomplete`, detail `no indexed entities to compare`).
`identical-source-and-config` is never emitted unless the compared raw bytes
are identical.

Every semantic result records: verdict, evidence level, assumptions
(`same-standard-library`, `no-undefined-behavior`, …), unsupported constructs
with ranges, and the configuration delta.

Evidence levels (fixed vocabulary): `identical-source-and-config`,
`normalized-ir`, `summary-contradiction`, `bounded-counterexample`,
`proved-under-assumptions`, `unsupported-or-incomplete`. The last three are
reserved for M3+/M4 but are part of the frozen vocabulary.

### Configuration delta

The report always includes a compact delta of semantic-affecting flags
between the two sides: language standard, target triple, driver, macro
definitions (`-D`/`-U`), include search order (`-I`/`-isystem`/`-iquote`/`-F`),
and remaining options (left-only / right-only after normalization).
`-D`/`-U` are compared as an **ordered** sequence: two sides that share the
same multiset of definitions but in a different order (e.g. `-DX=1 -UX` vs
`-UX -DX=1`, which leave `X` defined vs undefined) are **not** identical —
`definitions_reordered` is set and the configuration is treated as differing.
Identical source under different configurations is not automatically
identical behavior: any configuration delta downgrades a whole-file or
class `equivalent` to `unknown` with the delta as the reason (callable
verdicts from identical IR remain `equivalent` with evidence `normalized-ir`).

## Report

### JSON (`--json`)

Deterministic: fixed key order (as below), entities sorted by
(kind, qualified name, USR), edits in structural traversal order — parents
before children, siblings in source order, so left ranges and right ranges
are each non-decreasing (one-sided ops interleave at their aligned position),
LF line endings, `json_out::dumps_indent2` + trailing newline. No floats.

```json
{
  "tool": "cidx-diff",
  "version": "<kVersion>",
  "report_version": 1,
  "mode": "both",
  "scope": "file" | "symbol",
  "match": "strict" | "heuristic",
  "left":  { "file": "<abs>", "db": "<abs>", "tu": "<abs>"|null,
             "driver": "<str>"|null, "std": "<str>"|null, "target": "<str>"|null },
  "right": { ... same keys ... },
  "config_delta": { "identical": true|false,
                    "std": ["<l>","<r>"]|null, "target": [..]|null, "driver": [..]|null,
                    "definitions_added": [..], "definitions_removed": [..],
                    "definitions_reordered": true|false,
                    "includes_changed": true|false,
                    "options_left_only": [..], "options_right_only": [..] },
  "entities": [
    { "kind": "function"|"method"|"class"|"struct"|"union"|"enum"|"variable"|"namespace"|"typedef"|"other",
      "name": "<qualified name>",
      "signature": "<qualified signature>"|null,
      "status": "matched"|"added"|"removed"|"renamed",
      "match": "usr"|"signature"|"name"|"fingerprint"|null,
      "confidence": 100|95|85|70|null,
      "left_usr": "<usr>"|null, "right_usr": "<usr>"|null,
      "left_range": {"line":1,"col":1,"end_line":9,"end_col":2}|null,
      "right_range": {...}|null,
      "syntax": { "status": "unchanged"|"changed", "edit_count": 0, "truncated": false,
                  "edits": [ { "op": "...", "node": "<kind>", "detail": "<str>",
                               "left_range": {...}|null, "right_range": {...}|null } ] } | null,
      "semantic": { "verdict": "...", "evidence": "...",
                    "detail": "<str>",
                    "changes": [ "<profile/signature change>" ],
                    "unsupported": [ { "what": "<str>", "side": "left"|"right",
                                       "range": {...} } ] } | null
    } ],
  "syntax":   { "status": "unchanged"|"changed", "edit_count": 0, "truncated": false } | null,
  "semantic": { "verdict": "...", "evidence": "...",
                "assumptions": [..], "unsupported_count": 0,
                "detail": "<str>" } | null
}
```

`syntax`/`semantic` blocks are `null` when excluded by `--mode`. For
`scope: "symbol"` the `entities` array holds the single selected pair (plus
nested member rows for a class target).

### Text (default)

Deterministic, same ordering as JSON. Shape:

```text
cidx-diff: symbol  mode: both  match: heuristic
left:  /abs/old/cart.cpp :: Cart::total() const
right: /abs/new/cart.cpp :: Cart::total() const
config: identical
syntax: changed (2 edits)
  changed  CallExpr  callee reserve -> resize  L12:5-12:24 R12:5-12:23
  added    ReturnStmt  L- R18:3-18:12
semantic: unknown (normalized IR differs; no behavioral difference established)
```

With `--context N`, each edit is followed by the affected source lines.
A syntax change with no established semantic consequence is always shown as
`semantic: unknown (...)`, never as `equivalent`.

## Module boundary

```text
src/diff/
  main.cpp          thin CLI entry (exit-code contract)
  driver.hpp/.cpp   clang-free: option parsing + orchestration + run() for tests
  target.hpp/.cpp   clang-free: read-only DB open, file row, options pipeline,
                    config classification and delta
  syntax_ir.hpp     clang-free data: SynNode, Entity, SrcRange, fingerprints
  behaviour_ir.hpp  clang-free data: BehaviourIR, ClassProfile, Unsupported
  analyze.hpp/.cpp  clang-facing (OBJECT lib cidx_diff_ast, -fno-rtti,
                    -isystem LLVM headers): parse a side, build syntax trees,
                    Behaviour IR, class profiles, selector resolution
  compare.hpp/.cpp  clang-free: matching, edit script, verdicts, evidence
  report.hpp/.cpp   clang-free: deterministic text and JSON rendering
```

`cidx_diff_ast` objects fold into `cidx_core` (same pattern as
`cidx_astgraph_ast`); `cidx-diff` is a thin executable linking `cidx_core`
with the standard RPATH block. Tests drive `diff::run()` in-process.

Storage gains a read-only open mode (`Storage(path, Storage::OpenMode)`
routing to `SQLITE_OPEN_READONLY`, skipping directory creation, migration,
schema script, and backfills, and enforcing `schema_version ==
kSchemaVersion`). No schema bump; `index.db` contents are unchanged.

## Fixture matrix (tests/diff_test.cpp, ctest label `clang`)

| fixture | expectation |
|---|---|
| formatting-only edit | syntax unchanged; semantic `equivalent` |
| comment-only edit | syntax unchanged; semantic `equivalent` |
| local variable rename | syntax `changed` (renamed decl); semantic `equivalent` (`normalized-ir`) |
| local rename + type change | semantic **not** equivalent |
| callee change (`reserve`→`resize`) | syntax `changed` op on the call; semantic `unknown` |
| return-value literal change | syntax `changed`; semantic `unknown` |
| signature change (param type) | semantic `different` (`summary-contradiction`) |
| class field reorder / type change / access change / virtual added | semantic `different` with the profile change listed |
| overloaded selector without signature | error listing candidates |
| unsupported construct (inline asm / volatile) | semantic `unknown` (`unsupported-or-incomplete`) with marker |
| separate `--left-db` / `--right-db` | resolves both configs (two independent indexes) |
| header via `--left-tu` / `--right-tu` | TU provides compile context; scope restricted to header decls |
| config delta (`-DX` on one side) | delta reported; identical source ≠ auto-equivalent |
| `--context N` | edits followed by the affected source lines |
| edit-cap truncation (hermetic, label `default`) | > 1000 ops: `truncated: true`, count capped |
| cap starvation (hermetic, label `default`) | entity after a huge pair still reports `changed`, never `equivalent` |
| hidden friend body change | syntax `changed`; semantic **not** equivalent |
| class-scope `using` removed | class `different`, `declaration removed: using ...` listed |
| default-argument change | `different` (`summary-contradiction`) |
| `static` added to a file-scope function | `different`, `storage class` change listed |
| enum scoped-ness / underlying type / enumerator value shift | syntax `changed`; semantic **not** equivalent |
| `alignas` on a record | class `different` (layout row) |
| parse error (deleted header) | exit 1, `error: cannot parse <file>: <diagnostic>`, no report |
| volatile `++`/compound assignment | `unknown` with `volatile access` marker |
| out-of-line member body change | class semantic **not** equivalent |
| identical declaration-only pair | `equivalent`, detail `declaration only (no body in this translation unit)` |
| `--match strict` on a signature change | added+removed (no name tier) |
| zero-entity pair (macro-only) | differing bytes `unknown`; identical bytes `equivalent` |
| identical unnamed struct in two files | unchanged / `equivalent` (no path in spellings) |
| determinism | two runs byte-identical (text and JSON) |
| read-only | `index.db` bytes identical before/after; older-schema DB rejected; non-database file reports the real SQLite failure |

## Out of scope (v1)

Checked equivalence rewrites (iterator-loop ↔ range-for, commutativity) —
M3; solver-backed proofs/counterexamples — M4; caching — M5; comment
channel diffing (`--include-comments`); Python implementation.
