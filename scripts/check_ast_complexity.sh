#!/bin/sh
# Scoped clang-tidy complexity gate for src/ast (refactoring.md Phase 4).
# Uses the installed Clang toolchain; needs the CMake build dir for
# compile_commands.json. Fails on any readability-function-size /
# readability-function-cognitive-complexity finding.
set -eu
build="${1:-build}"
tidy="$(command -v clang-tidy || true)"
if [ -z "$tidy" ]; then
  for c in /opt/homebrew/opt/llvm/bin/clang-tidy /usr/local/opt/llvm/bin/clang-tidy; do
    [ -x "$c" ] && tidy="$c" && break
  done
fi
[ -n "$tidy" ] || { echo "clang-tidy not found" >&2; exit 2; }
[ -f "$build/compile_commands.json" ] || {
  echo "missing $build/compile_commands.json (configure with CMake first)" >&2
  exit 2
}
extra=""
if [ "$(uname)" = "Darwin" ] && command -v xcrun >/dev/null 2>&1; then
  # Homebrew clang-tidy has no default macOS sysroot.
  extra="--extra-arg-before=-isysroot --extra-arg-before=$(xcrun --show-sdk-path)"
fi
# Only files with a compile entry: sources of optional tool targets that are
# disabled in this configure have no flags and would fail on includes.
files=""
for f in src/ast/*.cpp; do
  if grep -q "$f" "$build/compile_commands.json"; then
    files="$files $f"
  fi
done

# Pass 1 — general limits from src/ast/.clang-tidy (statements <= 40,
# nesting <= 4, cognitive complexity <= 25) over every function.
# shellcheck disable=SC2086
"$tidy" -p "$build" --quiet --warnings-as-errors='*' $extra $files

# Pass 2 — the tighter visitor-callback limits (refactoring.md §Complexity
# limits): Visit* <= 25 statements, Traverse* <= 20 statements. clang-tidy has
# no per-name thresholds, so re-run at each tighter threshold and fail only on
# findings whose function name matches the callback family.
callback_gate() {
  threshold="$1"
  pattern="$2"
  # shellcheck disable=SC2086
  hits=$("$tidy" -p "$build" --quiet \
      --config="{Checks: '-*,readability-function-size', CheckOptions: {readability-function-size.StatementThreshold: '$threshold'}}" \
      $extra $files 2>/dev/null \
    | grep -E "function '$pattern[A-Za-z]*'" || true)
  if [ -n "$hits" ]; then
    echo "callback over the $threshold-statement limit ($pattern*):" >&2
    echo "$hits" >&2
    return 1
  fi
}
callback_gate 25 Visit
callback_gate 20 Traverse
