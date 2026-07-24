#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/cidx-tla-regression.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/models"
cp "$ROOT"/models/*.cfg "$WORK/models"/
sed '/^INVARIANT ProtectedInvariant$/d' \
  "$WORK/models/CidxRepositorySmoke.cfg" \
  >"$WORK/models/CidxRepositorySmoke.cfg.mutated"
mv "$WORK/models/CidxRepositorySmoke.cfg.mutated" \
  "$WORK/models/CidxRepositorySmoke.cfg"

set +e
output="$(TLA_MODEL_DIR="$WORK/models" "$ROOT/tools/check.sh" 2>&1)"
status=$?
set -e

if [[ "$status" -ne 25 ]]; then
  echo "TLA_REGRESSION_STATUS=FAIL reason=unexpected-exit-$status" >&2
  printf '%s\n' "$output" >&2
  exit 1
fi
if ! grep -q "TLA_CONFIG_STATUS=FAIL model=CidxRepositorySmoke reason=invariant-mismatch" \
    <<<"$output"; then
  echo "TLA_REGRESSION_STATUS=FAIL reason=missing-invariant-mismatch" >&2
  printf '%s\n' "$output" >&2
  exit 1
fi
if grep -q "TLA_CHECK_STATUS=PASS" <<<"$output"; then
  echo "TLA_REGRESSION_STATUS=FAIL reason=false-pass" >&2
  printf '%s\n' "$output" >&2
  exit 1
fi

run_seed() {
  local scenario="$1"
  local expected="$2"
  local seed_dir
  seed_dir="$(mktemp -d "${TMPDIR:-/tmp}/cidx-tla-seed.XXXXXX")"
  cp "$ROOT"/models/*.cfg "$seed_dir"/
  sed "s/^    Scenario = \"valid\"$/    Scenario = \"$scenario\"/" \
    "$seed_dir/CidxWorkspaceLifecycleSmoke.cfg" \
    >"$seed_dir/CidxWorkspaceLifecycleSmoke.cfg.seed"
  mv "$seed_dir/CidxWorkspaceLifecycleSmoke.cfg.seed" \
    "$seed_dir/CidxWorkspaceLifecycleSmoke.cfg"

  set +e
  local seed_output
  seed_output="$(TLA_MODEL_DIR="$seed_dir" TLA_MODELS="CidxWorkspaceLifecycleSmoke" \
    "$ROOT/tools/check.sh" 2>&1)"
  local seed_status=$?
  set -e
  rm -rf "$seed_dir"

  if [[ "$seed_status" -ne 30 ]]; then
    echo "TLA_REGRESSION_STATUS=FAIL scenario=$scenario reason=unexpected-exit-$seed_status" >&2
    printf '%s\n' "$seed_output" >&2
    exit 1
  fi
  if ! grep -q "TLA_MODEL_STATUS=FAIL model=CidxWorkspaceLifecycleSmoke" <<<"$seed_output" \
      || ! grep -q "$expected" <<<"$seed_output"; then
    echo "TLA_REGRESSION_STATUS=FAIL scenario=$scenario reason=missing-$expected" >&2
    printf '%s\n' "$seed_output" >&2
    exit 1
  fi
  echo "TLA_SEEDED_VIOLATION_STATUS=PASS scenario=$scenario invariant=$expected"
}

run_seed cross-universe-conflation ScopedSymbolIdentityInvariant
run_seed partial-publication NoPartialPublicationInvariant
run_seed missing-invalidation InvalidationInvariant
run_seed stale-as-current GenerationPublicationInvariant

echo "TLA_REGRESSION_STATUS=PASS mutation=missing-ProtectedInvariant seeded=4"
