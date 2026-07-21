# `cidx-diff` example

A runnable demo of the syntax-aware / semantic source diff (`cidx-diff`, see
[`docs/diff.md`](../../docs/diff.md)).

## Run

```sh
# from the repo root, after: cmake --build build --target cidx cidx-diff
examples/diff/run.sh
```

`cidx-diff` uses `index.db` **read-only**, purely to recover each file's exact
compile options, so the script first builds a throwaway index that registers
`before.cpp` and `after.cpp`, then diffs them in every mode. Nothing outside the
temp workspace is touched.

## What the two files show

`before.cpp` → `after.cpp` was crafted to hit every verdict:

| symbol           | change                              | syntax           | semantic                                   |
| ---------------- | ----------------------------------- | ---------------- | ------------------------------------------ |
| `area`           | param rename + inlined `return`     | 4 typed edits    | `unknown` (no behavioral diff established)  |
| `combine`        | `a + b` → `a - b` (real change)     | `+ -> -` edit    | `unknown` (M0–M2 stays conservative)        |
| `count_positive` | untouched                           | unchanged        | `equivalent` (identical source & config)    |
| `Point`          | method body reformatted only        | unchanged (AST)  | `equivalent` (normalized IR, same profile)  |

Two things to notice:

- **Syntax is typed and source-mapped, not line-based.** A parameter rename is
  reported as a `renamed ParmVar`, an operator swap as `changed BinaryOperator
  operator + -> -` — each with left/right ranges. Whitespace and comments are
  ignored, so `Point`'s reformatted body produces **no** syntax edits.
- **`unknown` is a normal, successful result.** The semantic layer never calls
  two snippets equivalent on a coincidental AST match, and in this M0–M2 slice it
  does not over-claim `different` either — `combine`'s behavioral change is a real
  edit in syntax but honestly reported as `unknown` semantically. `equivalent` is
  emitted only with named evidence (`identical-source-and-config` or
  `normalized-ir`).

## Modes

```sh
cidx-diff file  before.cpp after.cpp                 # both (default)
cidx-diff file  before.cpp after.cpp --mode syntax   # typed AST edits only
cidx-diff file  before.cpp after.cpp --mode semantic # behavior verdicts only
cidx-diff symbol before.cpp after.cpp --left combine --right combine --kind function
cidx-diff file  before.cpp after.cpp --json          # deterministic report
```

Exit code is `0` for any verdict (including `different` / `unknown`); `2` for a
usage error; `1` for an operational error (unregistered file, parse failure,
unmatched selector).
