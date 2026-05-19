# ADR-1: `sanitize_libclang_args` algorithm and driver-token detection

Status: accepted
Date: 2026-05-19
Stage: architect (tu-parse-fail v3)
Covers AC: AC-1, AC-2 (S1)

## Context
`src/bootstrap/compile_commands.rs::resolve_args` today returns the raw `arguments`
array from `compile_commands.json`. libclang's `parse(file, args)` API expects
**only flags** — it adds the driver, `-c`, and source file itself. Passing the
CMake-emitted `["/usr/bin/c++", "-std=c++17", "-c", "/tmp/foo.cpp"]` causes
libclang to interpret the leading driver path and the trailing source repeat as
extra input files, producing AstDeserialization errors and the silent 7/7 parse
failure documented in Issue 0001.

Forces:
- AC-1 enumerates a flag-prefix pass-through list, but the Risks table says
  "never strip tokens starting with `-`" — implies the strip predicate must be
  **deny-list** (strip a small set of known-bad tokens), not allow-list (keep a
  closed set of flag prefixes). Open question 4 in scenarios.md requires this be
  reconciled here.
- Driver detection must distinguish `/usr/bin/gcc` (a driver) from `foo.cc`
  (a source file whose name happens to end in `cc`) — see scenarios Open question
  in S1.
- Source-file repeat may appear with a relative path while `entry.file` is
  canonicalised absolute — must canonicalise both sides before equality.
- ≤5 µs/TU sanitisation budget (req NFR). No regex, no new deps.

## Decision

Add a new free function in `src/bootstrap/compile_commands.rs`:

```rust
fn sanitize_libclang_args(
    raw_args: &[String],
    canonical_file: &Path,
    directory: &Path,
) -> Vec<String>
```

called once per record inside `parse()` after `resolve_args` returns the raw
token list. The existing `resolve_args` keeps its current job (pick `arguments`
vs split `command`); the new sanitiser is the single chokepoint for strip
logic.

Algorithm (single linear scan, O(n)):

1. **Strip-leading-driver step.** Look at index 0 only. If `raw_args[0]` does
   **not** start with `-` AND its file-name basename matches the driver regex
   shape (see below), drop it. Otherwise keep it (covers the `foo.cc` first-token
   case).
2. **Pair-strip pass.** Walk the remainder with index `i`. For each token `t`:
   - If `t == "-c"` or `t == "-o"`: skip `t` and skip `raw_args[i+1]` if
     present, then advance by 2.
   - Else: keep `t`, advance by 1.
3. **Source-repeat strip.** After step 2, for each kept token `t` that does
   **not** start with `-`, compute `canonicalise(directory.join(t))`
   (best-effort: fall back to literal join if path missing). If the canonical
   form equals `canonical_file`, drop it. Apply to every non-flag token so the
   leading `foo.cpp` in CMake's "ninja-style" arg lists is also stripped.

**Driver basename match** (deny-list, basename only — NEVER path-substring):

```text
let stem = Path::new(token).file_name()?.to_str()?;
matches!(stem,
    "cc" | "c++" | "clang" | "clang++" | "gcc" | "g++"
) || stem.ends_with("-gcc") || stem.ends_with("-g++")
   || stem.ends_with("-clang") || stem.ends_with("-clang++")
```

Cross-compiler triplets like `arm-linux-gnueabi-g++` are covered by the
`*-g++` / `*-gcc` / `*-clang*` suffix rule. The hyphen guard prevents
`foo.cc` (no hyphen, stem `foo.cc` not in the literal set) from being treated
as a driver — answering scenario "Leading token is a source file ending in .cc".

**Token-prefix flags pass through unchanged.** No allow-list is hard-coded;
anything starting with `-` survives both pair-strip (except the explicit
`-c`/`-o` pair) and source-repeat (which only inspects non-`-` tokens).
That subsumes AC-1's enumerated list (`-D`, `-I`, `-std=`, `-W`, `-f`,
`-isystem`, `-include`, `-arch`, `-target`) and the broader real-world set
(`-O*`, `-m*`, `-g*`, `-pthread`, `--sysroot`, `@response`).

**Single TuEntry field.** `TuEntry.args` is replaced with the sanitised list.
The raw form is not retained (no current consumer; reduces memory). Dedup
hash is unchanged: it still hashes `args.join(NUL)` — which is now the
sanitised list, so cached entries with raw args become a cache-miss on first
run after the fix. That is acceptable (one-shot re-index; documented in
implementation notes).

**Edge cases (handled by algorithm above):**
- Empty `arguments` → empty `Vec` (step 1 short-circuits on empty slice).
- Driver-only `["clang++"]` → step 1 strips, steps 2/3 see empty tail → empty.
- `command` form (whitespace-split) → same sanitiser applies (uniform).

## Alternatives considered

| Option | Trade-off | Verdict |
| ------ | --------- | ------- |
| **A. Allow-list of flag prefixes** (`-D`, `-I`, `-std=`, …) — strip everything else | Breaks on real-world flags not in the list (`-O3`, `-pthread`, `--sysroot`, `@response`). AC-1 enumerates a *minimum*, not the closed universe; requirements Risks row explicitly says "never strip tokens starting with `-`". | rejected — too narrow |
| **B. Deny-list: strip only driver + `-c`/`-o` pair + source repeat** (chosen) | Conservative; preserves all `-`-prefixed flags by default. Driver shape is closed-form (5 literal stems + 4 suffix patterns). | **accepted** |
| **C. Run `clang -###` on the command and parse the cc1 invocation** | Most correct (matches what the real driver does), but spawns a subprocess per TU (cost blows the 5 µs budget by 4 orders of magnitude) and requires a clang binary on PATH (new runtime dep). | rejected — cost/dep |
| **D. Use `shlex`-only normalisation for the `command` form, no strip** | Doesn't fix Bug B at all — the existing `arguments` path is already pre-split, so shlex changes nothing. | rejected — does not address root cause |

## Consequences

Positive:
- Closed-form predicate fits in ~30 lines; trivially unit-testable by a table
  (S1 AC-2 Examples block enumerates 9 driver shapes).
- No new deps; no regex; pure stdlib path/string ops.
- Future driver shapes (e.g. `zig cc`) can be added by extending the basename
  match — single chokepoint.

Negative / follow-ups:
- **Cache invalidation on first deploy.** Existing `manifest.json` entries
  hash the raw (unsanitised) args; sanitised args produce a different
  `args_hash` so every TU re-parses once. Document in runbook.
- **Source-repeat strip best-effort canonicalisation.** If `directory.join(t)`
  cannot be canonicalised (file moved or synthetic test path), fall through to
  literal equality. Tests that use `/tmp/foo.cpp` synthetic paths already work
  this way (parse_valid_arguments_form in compile_commands.rs).
- **`-Xclang <arg>` two-token pair.** Today it passes through (both kept).
  libclang accepts that. Not in scope for Issue 0001; revisit if a TU regression
  surfaces.

## References
- requirements.md §S1, AC-1, AC-2; Risks table row 1
- scenarios.md Feature S1 (all scenarios), Open question 4
- /Users/husam/workspace/cpp-indexer/src/bootstrap/compile_commands.rs (resolve_args at line 92)
- docs/issues/0001-silent-tu-parse-failures.md §Root cause — Bug B
- Cognee tag: task:tu-parse-fail, role:architect
