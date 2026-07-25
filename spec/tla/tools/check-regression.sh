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

run_semantic_seed() {
  local scenario="$1"
  local expected="$2"
  local seed_dir
  seed_dir="$(mktemp -d "${TMPDIR:-/tmp}/cidx-tla-semantic-seed.XXXXXX")"
  cp "$ROOT"/models/*.cfg "$seed_dir"/
  sed "s/^    Defect = \"none\"$/    Defect = \"$scenario\"/" \
    "$seed_dir/CidxSemanticGraphSmoke.cfg" \
    >"$seed_dir/CidxSemanticGraphSmoke.cfg.seed"
  mv "$seed_dir/CidxSemanticGraphSmoke.cfg.seed" \
    "$seed_dir/CidxSemanticGraphSmoke.cfg"

  set +e
  local seed_output
  seed_output="$(TLA_MODEL_DIR="$seed_dir" TLA_MODELS="CidxSemanticGraphSmoke" \
    "$ROOT/tools/check.sh" 2>&1)"
  local seed_status=$?
  set -e
  rm -rf "$seed_dir"

  if [[ "$seed_status" -ne 30 ]] \
      || ! grep -q "TLA_MODEL_STATUS=FAIL model=CidxSemanticGraphSmoke" \
          <<<"$seed_output" \
      || ! grep -Fqx \
          "TLA_MODEL_VIOLATION=model=CidxSemanticGraphSmoke invariant=$expected" \
          <<<"$seed_output"; then
    echo "TLA_SEMANTIC_SEED_STATUS=FAIL scenario=$scenario reason=missing-$expected" >&2
    printf '%s\n' "$seed_output" >&2
    exit 1
  fi
  echo "TLA_SEMANTIC_SEED_STATUS=PASS scenario=$scenario invariant=$expected"
}

run_semantic_seed illegal-stream PlanTransitionInvariant
run_semantic_seed illegal-source PlanTransitionInvariant
run_semantic_seed illegal-filter PlanTransitionInvariant
run_semantic_seed illegal-traverse PlanTransitionInvariant
run_semantic_seed illegal-set PlanTransitionInvariant
run_semantic_seed illegal-select PlanTransitionInvariant
run_semantic_seed illegal-order PlanTransitionInvariant
run_semantic_seed illegal-limit PlanTransitionInvariant
run_semantic_seed invalid-witness WitnessInvariant
run_semantic_seed duplicate-results SetSemanticsInvariant
run_semantic_seed query-write ReadOnlyExecutionInvariant
run_semantic_seed complete-truncated CompletenessInvariant
run_semantic_seed complete-unknown CompletenessInvariant
run_semantic_seed stale-fact-consumption TransformConsumptionInvariant
run_semantic_seed stale-transform TransformPublicationInvariant
run_semantic_seed failed-transform TransformPublicationInvariant
run_semantic_seed partial-transform TransformPublicationInvariant

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
