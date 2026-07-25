#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/cidx-tla-regression.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/models"
mkdir -p "$WORK/modules"
cp "$ROOT"/models/*.cfg "$WORK/models"/
cp "$ROOT"/modules/*.tla "$WORK/modules"/
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
run_seed configuration-invalidation InvalidationInvariant
run_seed toolchain-invalidation InvalidationInvariant
run_seed catalog-invalidation InvalidationInvariant

run_storage_seed() {
  local scenario="$1"
  local expected="$2"
  local storage_seed_dir
  storage_seed_dir="$(mktemp -d "${TMPDIR:-/tmp}/cidx-tla-storage-seed.XXXXXX")"
  cp "$ROOT"/models/*.cfg "$storage_seed_dir"/
  sed "s/^    Scenario = \"valid\"$/    Scenario = \"$scenario\"/" \
    "$storage_seed_dir/CidxStorageLifecycleSmoke.cfg" \
    >"$storage_seed_dir/CidxStorageLifecycleSmoke.cfg.seed"
  mv "$storage_seed_dir/CidxStorageLifecycleSmoke.cfg.seed" \
    "$storage_seed_dir/CidxStorageLifecycleSmoke.cfg"

  set +e
  local storage_seed_output
  storage_seed_output="$(TLA_MODEL_DIR="$storage_seed_dir" \
    TLA_MODELS="CidxStorageLifecycleSmoke" \
    "$ROOT/tools/check.sh" 2>&1)"
  local storage_seed_status=$?
  set -e
  rm -rf "$storage_seed_dir"

  if [[ "$storage_seed_status" -ne 30 ]] \
      || ! grep -q "TLA_INVARIANT_STATUS=FAIL model=CidxStorageLifecycleSmoke invariant=$expected" \
          <<<"$storage_seed_output"; then
    echo "TLA_REGRESSION_STATUS=FAIL scenario=$scenario reason=unexpected-invariant" >&2
    printf '%s\n' "$storage_seed_output" >&2
    exit 1
  fi
  echo "TLA_SEEDED_VIOLATION_STATUS=PASS scenario=$scenario invariant=$expected"
}

run_storage_seed cross-file-atomicity CrossFileAtomicityInvariant

multi_generation_dir="$(mktemp -d "${TMPDIR:-/tmp}/cidx-tla-multi-generation.XXXXXX")"
cp "$ROOT"/models/*.cfg "$multi_generation_dir"/
sed 's/^    Scenario = "valid"$/    Scenario = "multi-generation"/' \
  "$multi_generation_dir/CidxStorageLifecycleSmoke.cfg" \
  >"$multi_generation_dir/CidxStorageLifecycleSmoke.cfg.seed"
mv "$multi_generation_dir/CidxStorageLifecycleSmoke.cfg.seed" \
  "$multi_generation_dir/CidxStorageLifecycleSmoke.cfg"
set +e
multi_generation_output="$(TLA_MODEL_DIR="$multi_generation_dir" \
  TLA_MODELS="CidxStorageLifecycleSmoke" \
  "$ROOT/tools/check.sh" 2>&1)"
multi_generation_status=$?
set -e
rm -rf "$multi_generation_dir"
if [[ "$multi_generation_status" -ne 0 ]] \
    || ! grep -q "TLA_CHECK_STATUS=PASS models=CidxStorageLifecycleSmoke" \
        <<<"$multi_generation_output"; then
  echo "TLA_REGRESSION_STATUS=FAIL scenario=multi-generation reason=model-did-not-pass" >&2
  printf '%s\n' "$multi_generation_output" >&2
  exit 1
fi
echo "TLA_MULTI_GENERATION_STATUS=PASS scenario=multi-generation"

negative_storage_modules="$(mktemp -d "${TMPDIR:-/tmp}/cidx-tla-storage-negative.XXXXXX")"
cp "$ROOT"/modules/*.tla "$negative_storage_modules"/
sed 's/readOnlyWrites |-> 0,/readOnlyWrites |-> 1,/' \
  "$ROOT/modules/CidxStorageLifecycle.tla" \
  >"$negative_storage_modules/CidxStorageLifecycle.tla"
negative_storage_dir="$(mktemp -d "${TMPDIR:-/tmp}/cidx-tla-storage-negative-model.XXXXXX")"
cp "$ROOT"/models/*.cfg "$negative_storage_dir"/
sed 's/^    Scenario = "valid"$/    Scenario = "cross-file-atomicity"/' \
  "$negative_storage_dir/CidxStorageLifecycleSmoke.cfg" \
  >"$negative_storage_dir/CidxStorageLifecycleSmoke.cfg.seed"
mv "$negative_storage_dir/CidxStorageLifecycleSmoke.cfg.seed" \
  "$negative_storage_dir/CidxStorageLifecycleSmoke.cfg"
set +e
negative_storage_output="$(TLA_MODULE_DIR="$negative_storage_modules" \
  TLA_MODEL_DIR="$negative_storage_dir" \
  TLA_MODELS="CidxStorageLifecycleSmoke" \
  "$ROOT/tools/check.sh" 2>&1)"
negative_storage_status=$?
set -e
rm -rf "$negative_storage_modules" "$negative_storage_dir"
if [[ "$negative_storage_status" -ne 30 ]] \
    || ! grep -q "TLA_INVARIANT_STATUS=FAIL model=CidxStorageLifecycleSmoke invariant=TypeInvariant" \
        <<<"$negative_storage_output" \
    || grep -q "invariant=CrossFileAtomicityInvariant" <<<"$negative_storage_output"; then
  echo "TLA_REGRESSION_STATUS=FAIL reason=storage-oracle-accepted-unrelated-invariant" >&2
  printf '%s\n' "$negative_storage_output" >&2
  exit 1
fi
echo "TLA_ORACLE_REGRESSION_STATUS=PASS mutation=unrelated-storage-invariant"

storage_liveness_modules="$(mktemp -d "${TMPDIR:-/tmp}/cidx-tla-storage-liveness.XXXXXX")"
cp "$ROOT"/modules/*.tla "$storage_liveness_modules"/
sed -e 's#\\/ PrepareStagedArtifact##' \
  -e '\|ValidateStagedArtifact \\/ PublishCoreGeneration \\/ InterruptPublication|d' \
  -e '/WF_vars(PrepareStagedArtifact)/d' \
  -e '/WF_vars(ValidateStagedArtifact)/d' \
  -e '/WF_vars(PublishCoreGeneration)/d' \
  -e '/WF_vars(InterruptPublication)/d' \
  "$ROOT/modules/CidxStorageLifecycle.tla" \
  >"$storage_liveness_modules/CidxStorageLifecycle.tla"
set +e
storage_liveness_output="$(TLA_MODULE_DIR="$storage_liveness_modules" \
  TLA_MODELS="CidxStorageLifecycleSmoke" \
  "$ROOT/tools/check.sh" 2>&1)"
storage_liveness_status=$?
set -e
rm -rf "$storage_liveness_modules"
if [[ "$storage_liveness_status" -ne 30 ]] \
    || ! grep -q "TLA_MODEL_STATUS=FAIL model=CidxStorageLifecycleSmoke" \
        <<<"$storage_liveness_output" \
    || ! grep -q "StorageEventuallySettles" <<<"$storage_liveness_output"; then
  echo "TLA_LIVENESS_REGRESSION_STATUS=FAIL reason=storage-property-did-not-fail-closed" >&2
  printf '%s\n' "$storage_liveness_output" >&2
  exit 1
fi
echo "TLA_LIVENESS_REGRESSION_STATUS=PASS mutation=removed-storage-publication"

progress_modules="$(mktemp -d "${TMPDIR:-/tmp}/cidx-tla-progress.XXXXXX")"
cp "$ROOT"/modules/*.tla "$progress_modules"/
sed 's#\\/ MakeCurrent##' \
  "$progress_modules/CidxWorkspaceLifecycle.tla" \
  >"$progress_modules/CidxWorkspaceLifecycle.tla.mutated"
mv "$progress_modules/CidxWorkspaceLifecycle.tla.mutated" \
  "$progress_modules/CidxWorkspaceLifecycle.tla"

set +e
progress_output="$(TLA_MODULE_DIR="$progress_modules" \
  TLA_MODELS="CidxWorkspaceLifecycleSmoke" \
  "$ROOT/tools/check.sh" 2>&1)"
progress_status=$?
set -e
rm -rf "$progress_modules"

if [[ "$progress_status" -ne 30 ]] \
    || ! grep -q "TLA_MODEL_STATUS=FAIL model=CidxWorkspaceLifecycleSmoke" \
        <<<"$progress_output" \
    || ! grep -q "RebuildEventuallySettles" <<<"$progress_output"; then
  echo "TLA_REGRESSION_STATUS=FAIL reason=progress-property-did-not-fail-closed" >&2
  printf '%s\n' "$progress_output" >&2
  exit 1
fi
echo "TLA_PROGRESS_REGRESSION_STATUS=PASS mutation=removed-MakeCurrent"

echo "TLA_REGRESSION_STATUS=PASS mutation=missing-ProtectedInvariant seeded=7"

awk '
index($0, "queryState") && index($0, "\"complete\"") {
  sub(/"complete"/, "\"running\"")
}
{ print }
' "$WORK/modules/CidxBehavior.tla" >"$WORK/modules/CidxBehavior.tla.mutated"
mv "$WORK/modules/CidxBehavior.tla.mutated" "$WORK/modules/CidxBehavior.tla"

set +e
liveness_output="$(JAVA_BIN="${JAVA_BIN:-}" \
  TLA_MODEL_DIR="$ROOT/models" \
  TLA_MODULE_DIR="$WORK/modules" \
  "$ROOT/tools/check.sh" 2>&1)"
liveness_status=$?
set -e

if [[ "$liveness_status" -ne 30 ]]; then
  echo "TLA_LIVENESS_REGRESSION_STATUS=FAIL reason=unexpected-exit-$liveness_status" >&2
  printf '%s\n' "$liveness_output" >&2
  exit 1
fi
if ! grep -q "TLA_MODEL_STATUS=FAIL model=CidxBehaviorSmoke" <<<"$liveness_output"; then
  echo "TLA_LIVENESS_REGRESSION_STATUS=FAIL reason=missing-model-failure" >&2
  printf '%s\n' "$liveness_output" >&2
  exit 1
fi
if grep -q "TLA_CHECK_STATUS=PASS" <<<"$liveness_output"; then
  echo "TLA_LIVENESS_REGRESSION_STATUS=FAIL reason=false-pass" >&2
  printf '%s\n' "$liveness_output" >&2
  exit 1
fi

echo "TLA_LIVENESS_REGRESSION_STATUS=PASS mutation=missing-QueryCompletion"

EDGE_WORK="$WORK/edge"
mkdir -p "$EDGE_WORK/models" "$EDGE_WORK/modules"
cp "$ROOT"/models/*.cfg "$EDGE_WORK/models"/
cp "$ROOT"/modules/*.tla "$EDGE_WORK/modules"/
sed 's/TraceBound = 9/TraceBound = 4/' \
  "$EDGE_WORK/models/CidxBehaviorSmoke.cfg" \
  >"$EDGE_WORK/models/CidxBehaviorSmoke.cfg.mutated"
mv "$EDGE_WORK/models/CidxBehaviorSmoke.cfg.mutated" \
  "$EDGE_WORK/models/CidxBehaviorSmoke.cfg"
sed '/^ProgressTraceAvailable ==/! s/ProgressTraceAvailable/TraceAvailable/g' \
  "$EDGE_WORK/modules/CidxBehavior.tla" \
  >"$EDGE_WORK/modules/CidxBehavior.tla.mutated"
mv "$EDGE_WORK/modules/CidxBehavior.tla.mutated" \
  "$EDGE_WORK/modules/CidxBehavior.tla"

set +e
edge_output="$(JAVA_BIN="${JAVA_BIN:-}" \
  TLA_MODEL_DIR="$EDGE_WORK/models" \
  TLA_MODULE_DIR="$EDGE_WORK/modules" \
  "$ROOT/tools/check.sh" 2>&1)"
edge_status=$?
set -e

if [[ "$edge_status" -ne 30 ]]; then
  echo "TLA_BOUND_EDGE_REGRESSION_STATUS=FAIL reason=unexpected-exit-$edge_status" >&2
  printf '%s\n' "$edge_output" >&2
  exit 1
fi
if ! grep -q "TLA_MODEL_STATUS=FAIL model=CidxBehaviorSmoke" <<<"$edge_output"; then
  echo "TLA_BOUND_EDGE_REGRESSION_STATUS=FAIL reason=missing-model-failure" >&2
  printf '%s\n' "$edge_output" >&2
  exit 1
fi
if grep -q "TLA_CHECK_STATUS=PASS" <<<"$edge_output"; then
  echo "TLA_BOUND_EDGE_REGRESSION_STATUS=FAIL reason=false-pass" >&2
  printf '%s\n' "$edge_output" >&2
  exit 1
fi

echo "TLA_BOUND_EDGE_REGRESSION_STATUS=PASS mutation=unbounded-pending-state"
