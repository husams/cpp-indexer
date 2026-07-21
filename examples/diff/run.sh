#!/usr/bin/env bash
# examples/diff/run.sh — a self-contained, runnable cidx-diff demo.
#
# cidx-diff reuses index.db (read-only) purely for compile context: each side
# is reparsed with Clang under the exact options recorded for that file. So the
# demo first builds a throwaway index that registers before.cpp and after.cpp,
# then diffs them in every mode.
#
# Usage:  examples/diff/run.sh          # uses ./build/cidx{,-diff}
#         BUILD=/path/to/build examples/diff/run.sh
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
build="${BUILD:-$repo/build}"
cidx="$build/cidx"
cidxdiff="$build/cidx-diff"

for bin in "$cidx" "$cidxdiff"; do
  [ -x "$bin" ] || { echo "missing $bin — build first: cmake --build $build --target cidx cidx-diff" >&2; exit 1; }
done

# Throwaway workspace: copy the two sources in and register them in a fresh index.
work="$(mktemp -d "${TMPDIR:-/tmp}/cidx_diff_example_XXXXXX")"
trap 'rm -rf "$work"' EXIT
cp "$here/before.cpp" "$here/after.cpp" "$work/"

cat > "$work/compile_commands.json" <<JSON
[
  {"directory": "$work", "command": "c++ -std=c++17 -c before.cpp", "file": "$work/before.cpp"},
  {"directory": "$work", "command": "c++ -std=c++17 -c after.cpp",  "file": "$work/after.cpp"}
]
JSON

export INDEXER_CACHE="$work"
echo "== building a throwaway index for compile context =="
"$cidx" import --db "$work" --name diff-example >/dev/null
"$cidx" index >/dev/null
echo "index: $(sqlite3 "$work/index.db" "SELECT COUNT(*) FROM file;" 2>/dev/null || echo '?') files, schema $(sqlite3 "$work/index.db" "SELECT value FROM meta WHERE key='schema_version';" 2>/dev/null || echo '?')"
echo

run() { echo "\$ cidx-diff $*"; "$cidxdiff" "$@" || true; echo; }

echo "===== whole-file diff, default mode (both) ====="
run file "$work/before.cpp" "$work/after.cpp"

echo "===== syntax only (typed AST edits, whitespace/comments ignored) ====="
run file "$work/before.cpp" "$work/after.cpp" --mode syntax

echo "===== semantic only (behavior verdict per symbol) ====="
run file "$work/before.cpp" "$work/after.cpp" --mode semantic

echo "===== one symbol: the behavioral change in combine() ====="
run symbol "$work/before.cpp" "$work/after.cpp" --left combine --right combine --kind function

echo "===== JSON report (deterministic, machine-readable) ====="
run file "$work/before.cpp" "$work/after.cpp" --json
