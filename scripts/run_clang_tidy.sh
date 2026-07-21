#!/bin/sh
# Run the repository's clang-tidy policies over project-owned C++ sources.
set -eu

build="${1:-build}"

find_tool() {
  for candidate in "$@"; do
    if command -v "$candidate" >/dev/null 2>&1; then
      command -v "$candidate"
      return 0
    fi
  done
  return 1
}

tidy="${CLANG_TIDY:-}"
if [ -z "$tidy" ]; then
  tidy="$(find_tool clang-tidy clang-tidy-22 clang-tidy-21 clang-tidy-20 \
    clang-tidy-19 clang-tidy-18 clang-tidy-17 clang-tidy-16 || true)"
fi
if [ -z "$tidy" ]; then
  for candidate in /opt/homebrew/opt/llvm/bin/clang-tidy \
      /usr/local/opt/llvm/bin/clang-tidy; do
    if [ -x "$candidate" ]; then
      tidy="$candidate"
      break
    fi
  done
fi

runner="${RUN_CLANG_TIDY:-}"
if [ -z "$runner" ]; then
  runner="$(find_tool run-clang-tidy run-clang-tidy-22 run-clang-tidy-21 \
    run-clang-tidy-20 run-clang-tidy-19 run-clang-tidy-18 \
    run-clang-tidy-17 run-clang-tidy-16 || true)"
fi
if [ -z "$runner" ]; then
  for candidate in /opt/homebrew/opt/llvm/bin/run-clang-tidy \
      /usr/local/opt/llvm/bin/run-clang-tidy; do
    if [ -x "$candidate" ]; then
      runner="$candidate"
      break
    fi
  done
fi

apply_replacements="${CLANG_APPLY_REPLACEMENTS:-}"
if [ -z "$apply_replacements" ]; then
  apply_replacements="$(find_tool clang-apply-replacements \
    clang-apply-replacements-22 clang-apply-replacements-21 \
    clang-apply-replacements-20 clang-apply-replacements-19 \
    clang-apply-replacements-18 clang-apply-replacements-17 \
    clang-apply-replacements-16 || true)"
fi
if [ -z "$apply_replacements" ]; then
  for candidate in /opt/homebrew/opt/llvm/bin/clang-apply-replacements \
      /usr/local/opt/llvm/bin/clang-apply-replacements; do
    if [ -x "$candidate" ]; then
      apply_replacements="$candidate"
      break
    fi
  done
fi

[ -n "$tidy" ] || {
  echo "clang-tidy not found; install the LLVM clang tools" >&2
  exit 2
}
[ -n "$runner" ] || {
  echo "run-clang-tidy not found; install the LLVM clang tools" >&2
  exit 2
}
[ -f "$build/compile_commands.json" ] || {
  echo "missing $build/compile_commands.json; configure with CMake first" >&2
  exit 2
}

source_filter="${CIDX_TIDY_SOURCE_FILTER:-.*/src/.*\.cpp$}"

set -- -p "$build" -clang-tidy-binary "$tidy" -quiet -hide-progress \
  -source-filter="$source_filter"

if [ -n "${CIDX_TIDY_CHECKS:-}" ]; then
  set -- "$@" -checks="$CIDX_TIDY_CHECKS"
fi

if [ "${CIDX_TIDY_WARNINGS_AS_ERRORS:-0}" = 1 ]; then
  set -- "$@" -warnings-as-errors='*'
fi

if [ "${CIDX_TIDY_FIX:-0}" = 1 ]; then
  [ -n "$apply_replacements" ] || {
    echo "clang-apply-replacements not found; install the LLVM clang tools" >&2
    exit 2
  }
  set -- "$@" -clang-apply-replacements-binary "$apply_replacements" -fix
fi

if [ "$(uname)" = Darwin ] && command -v xcrun >/dev/null 2>&1; then
  sdk="$(xcrun --show-sdk-path)"
  set -- "$@" -extra-arg-before=-isysroot -extra-arg-before="$sdk"
fi

exec "$runner" "$@" "$source_filter"
