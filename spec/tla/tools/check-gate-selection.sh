#!/usr/bin/env bash
#
# Regression test for select-changed-gates.sh's changed-path -> gate mapping
# (HSE-89 round-3 critic). select-changed-gates.sh resolves its own repo root
# from $BASH_SOURCE[0] (two directories up from itself), so this copies the
# selector + ci-dependency-map.json into a throwaway git repo at the same
# relative layout (spec/tla/tools/select-changed-gates.sh,
# spec/tla/ci-dependency-map.json) rather than hand-building a changed-file
# list -- the exact code path .github/workflows/verification.yml runs is
# exercised, not a reimplementation of it.
#
# Round-3 critic repros (both previously selected zero gates for the flow
# that actually exercises the changed path):
#   - src/application/conformance_recorder.cpp selected cpp-default,
#     tla-conformance but not tla-sidecar-conformance, even though this file
#     emits and replays sidecar.publish observations against sidecar_schema_.
#   - spec/tla/tools/export-counterexample.sh selected cpp-default,
#     tla-syntax-and-model but not tla-conformance, even though
#     .github/workflows/verification.yml's tla-conformance job is the one
#     that actually runs the exporter and diffs it against the golden file
#     (tla-syntax-and-model never invokes this script at all).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/cidx-tla-gate-selection.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/spec/tla/tools"
cp "$ROOT/ci-dependency-map.json" "$WORK/spec/tla/ci-dependency-map.json"
cp "$ROOT/tools/select-changed-gates.sh" "$WORK/spec/tla/tools/select-changed-gates.sh"
chmod +x "$WORK/spec/tla/tools/select-changed-gates.sh"
SELECTOR="$WORK/spec/tla/tools/select-changed-gates.sh"

git -C "$WORK" init --quiet --initial-branch=main
git -C "$WORK" config user.email "test@example.com"
git -C "$WORK" config user.name "test"
git -C "$WORK" add -A
git -C "$WORK" commit --quiet -m base

# run_case <name> <comma-separated-gates-expected-true> <comma-separated-gates-expected-false> <changed-path>...
run_case() {
  local name="$1" expect_true_csv="$2" expect_false_csv="$3"
  shift 3
  local paths=("$@")

  git -C "$WORK" checkout --quiet -B "case-$name" main
  local path
  for path in "${paths[@]}"; do
    mkdir -p "$WORK/$(dirname "$path")"
    printf 'changed\n' >>"$WORK/$path"
    git -C "$WORK" add "$path"
  done
  git -C "$WORK" commit --quiet -m "case-$name"

  local output
  output="$("$SELECTOR" main "case-$name")"

  local gate
  IFS=',' read -ra expect_true <<<"$expect_true_csv"
  for gate in "${expect_true[@]:-}"; do
    [[ -z "$gate" ]] && continue
    if ! grep -q "^run_${gate}=true$" <<<"$output"; then
      echo "TLA_GATE_SELECTION_STATUS=FAIL case=$name reason=missing-true-$gate" >&2
      printf '%s\n' "$output" >&2
      exit 1
    fi
  done
  IFS=',' read -ra expect_false <<<"$expect_false_csv"
  for gate in "${expect_false[@]:-}"; do
    [[ -z "$gate" ]] && continue
    if ! grep -q "^run_${gate}=false$" <<<"$output"; then
      echo "TLA_GATE_SELECTION_STATUS=FAIL case=$name reason=missing-false-$gate" >&2
      printf '%s\n' "$output" >&2
      exit 1
    fi
  done
  echo "TLA_GATE_SELECTION_STATUS=PASS case=$name"

  git -C "$WORK" checkout --quiet main
}

# Round-3 repro 1: conformance_recorder.cpp is mapped to
# index-generation-publication-and-queryplan (tla-conformance) but must also
# select tla-sidecar-conformance, since the recorder both derives and
# replays sidecar.publish observations against sidecar_schema_.
run_case conformance-recorder-selects-sidecar-gate \
  "tla_conformance,tla_sidecar_conformance,cpp_default" \
  "tla_syntax_and_model,tla_proofs,tla_policy" \
  "src/application/conformance_recorder.cpp"

run_case conformance_schema-selects-both-conformance-gates \
  "tla_conformance,tla_sidecar_conformance,cpp_default" \
  "tla_syntax_and_model,tla_proofs,tla_policy" \
  "src/application/conformance_schema.hpp"

# Round-3 repro 2: export-counterexample.sh must select tla-conformance --
# the job that actually runs it (verification.yml's tla-conformance job) --
# not tla-syntax-and-model, which never invokes this script.
run_case export-counterexample-selects-conformance-gate \
  "tla_conformance,cpp_default" \
  "tla_syntax_and_model,tla_proofs,tla_policy,tla_sidecar_conformance" \
  "spec/tla/tools/export-counterexample.sh"

# An unmapped path must fail closed: every gate runs rather than silently
# selecting none (the round-1 finding this map's fallback exists to fix).
run_case unmapped-path-runs-every-gate \
  "tla_syntax_and_model,tla_proofs,tla_conformance,tla_sidecar_conformance,tla_policy,cpp_default" \
  "" \
  "src/some/unmapped_new_file.cpp"

# A genuinely narrow, already-correctly-mapped path must still select only
# its own flow's gates -- the fallback must not fire when every changed path
# resolves to a flow.
run_case mapped-path-stays-narrow \
  "tla_policy" \
  "tla_syntax_and_model,tla_proofs,tla_conformance,tla_sidecar_conformance,cpp_default" \
  ".github/CODEOWNERS"

echo "TLA_GATE_SELECTION_STATUS=PASS"
