---
name: cidx-modern-cpp
description: Enforce cidx's C++23 modernization and clean-code policy with scoped clang-tidy, modernize checks, clang-format, safe automatic fixes, and diagnostic baselines. Use for every change to project-owned C++ source or headers, CMake clang-tidy integration, enum/constant cleanup, RAII modernization, or investigation of clang-tidy findings.
---

# cidx modern C++

Use the checked-in `.clang-tidy`, CMake compile database, and project runner.
Use this skill alongside `cidx-build-and-test`; this skill owns static analysis,
while that skill owns build and test gates.

## Required workflow

1. Confirm the working tree and preserve unrelated changes.
2. Configure before editing so `build/compile_commands.json` is current:

   ```bash
   cmake -S . -B build
   ```

3. Run clang-tidy on each affected translation unit before editing and save the
   output under `/tmp`. Check headers through representative `.cpp` consumers
   from the compile database.
4. Make the smallest C++23 change. Run clang-format only on explicitly changed
   project files.
5. Rerun the identical scoped analysis. Allow no new findings; fix findings
   introduced by the change. Use the pre-edit output to prove legacy debt.
6. Run the gates selected by `cidx-build-and-test` and report exact checks and
   skips.

## Commands

Run the full advisory project scan:

```bash
make tidy
```

Run one translation unit strictly. `CIDX_TIDY_SOURCE_FILTER` is a regular
expression matched against compile-database paths:

```bash
CIDX_TIDY_SOURCE_FILTER='.*/src/query/plan[.]cpp$' \
CIDX_TIDY_WARNINGS_AS_ERRORS=1 \
scripts/run_clang_tidy.sh build
```

Run only the clang-tidy modernizer on an explicit translation unit and apply
available safe fixes:

```bash
CIDX_TIDY_SOURCE_FILTER='.*/src/query/plan[.]cpp$' \
CIDX_TIDY_CHECKS='-*,modernize-*' \
CIDX_TIDY_FIX=1 \
scripts/run_clang_tidy.sh build
```

To attach clang-tidy to compilation:

```bash
cmake -S . -B build -DCIDX_ENABLE_CLANG_TIDY=ON
cmake --build build -j
```

Add `-DCIDX_CLANG_TIDY_WARNINGS_AS_ERRORS=ON` only when the selected target has
a clean baseline or when deliberately paying down all of its findings.

## Modern C++ policy

- Treat clang-tidy `modernize-*` as the modernizer; do not seek a separate
  `clang-modernize` executable.
- Use `enum class` for closed sets. Keep raw integral/string values only at
  persistence, SQL, serialization, C, or ABI boundaries; convert explicitly
  with `std::to_underlying` where appropriate.
- Prefer RAII, value semantics, `std::span`, `std::string_view`, ranges, and
  other standard C++23 facilities when they simplify ownership or intent.
- Do not modernize for novelty. Preserve deterministic output, database values,
  public behavior, and LLVM-version portability.

## Fix safety

- Automatic fixes are authorized without extra approval only for explicitly
  selected project-owned files in scope.
- Never blanket-fix the repository. Never run fixes over `third_party/`, build
  trees, generated Souffle sources, vendored code, or unrelated user changes.
- Inspect the diff after every fix pass. Remove or hand-edit only unsafe fixer
  hunks that affect semantics, ordering, formatting, schema, or ABI behavior.
- Do not suppress findings with `NOLINT` or weaken `.clang-tidy` without an
  explicit user-approved false-positive rationale.
