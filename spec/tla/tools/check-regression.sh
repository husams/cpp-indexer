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

echo "TLA_REGRESSION_STATUS=PASS mutation=missing-ProtectedInvariant"
