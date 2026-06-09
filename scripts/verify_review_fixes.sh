#!/usr/bin/env bash
# Verification gate for the 2026-06-09 full-code-review fixes.
# Runs format, lint, full test suite, and targeted regression checks.
# All steps must pass. DB-backed integration tests auto-skip unless
# NEO4J_URI / INDRADB_ENDPOINT are set.
set -euo pipefail

cd "$(dirname "$0")/.."

# libclang is required for visitor tests on macOS (build/lint work without it).
if [[ "$(uname)" == "Darwin" ]]; then
  export LIBCLANG_PATH="${LIBCLANG_PATH:-/Library/Developer/CommandLineTools/usr/lib}"
  export DYLD_LIBRARY_PATH="${DYLD_LIBRARY_PATH:-$LIBCLANG_PATH}"
fi

step() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }
fail=0

step "cargo fmt --check"
cargo fmt --all -- --check

step "cargo clippy (all targets, deny warnings)"
cargo clippy --all-targets -- -D warnings

step "cargo nextest run (full suite)"
if command -v cargo-nextest >/dev/null 2>&1; then
  cargo nextest run --no-fail-fast
else
  cargo test --no-fail-fast
fi

step "regression: reset.rs must validate repo_name as a single path component"
grep -q "remove_dir_all" src/api/reset.rs
if ! grep -Eq "components|is_single_component|validate_repo_name" src/api/reset.rs; then
  echo "FAIL: no path-component validation found in src/api/reset.rs"; fail=1
fi

step "regression: shallow.rs must not clone the NodeRecord on push"
if grep -q "self.nodes.push(node.clone())" src/visit/shallow.rs; then
  echo "FAIL: per-node NodeRecord clone still present in src/visit/shallow.rs"; fail=1
fi

step "regression: synthetic-node dedup must not linear-scan self.nodes"
if grep -v '^\s*//' src/visit/shallow.rs | grep -q 'self.nodes.iter().any(|n| n.usr == usr)'; then
  echo "FAIL: O(n^2) dedup scan still present in src/visit/shallow.rs"; fail=1
fi

step "regression: phase-2 shards must use the shared writer properties (magic + compression)"
if grep -v '^\s*//' src/visit/decorate.rs | grep -q 'WriterProperties::builder()'; then
  echo "FAIL: src/visit/decorate.rs still uses bare WriterProperties::builder()"; fail=1
fi

step "regression: parquet row groups must be capped"
if ! grep -q "set_max_row_group_size" src/stage/schema.rs; then
  echo "FAIL: no set_max_row_group_size in src/stage/schema.rs"; fail=1
fi

step "regression: sinks must not clone the chunk on the first attempt"
# The inverted pattern is `let body = if <not-last-attempt> { payload.clone() }`,
# which clones on attempt 0. After the fix, no per-attempt clone may remain in
# the retry loops (test code may clone freely).
for f in src/sink/neo4j.rs src/sink/indradb.rs; do
  if grep -v '^\s*//' "$f" | grep -A3 'let body = if' | grep -Eq '\b(rows|items)\.clone\(\)'; then
    echo "FAIL: eager retry-clone (clone on first attempt) still present in $f"; fail=1
  fi
done

if [[ "$fail" -ne 0 ]]; then
  echo; echo "VERIFICATION FAILED"; exit 1
fi
echo; echo "ALL VERIFICATION STEPS PASSED"
