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
# shellcheck disable=SC2086
exec "$tidy" -p "$build" --quiet --warnings-as-errors='*' $extra $files
