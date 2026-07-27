#!/usr/bin/env bash
#
# Regression proving the QA round-2 fix to check-proofs.sh's F1
# theorem/invariant binding check (acceptance-review finding): the round-1
# fix required manifest.json's declared theorem to exist as a THEOREM in the
# checked module, and each declared invariant's name to appear literally
# somewhere as "[]<Invariant>" -- but that grep ran over the WHOLE module
# file, comments included, with no requirement that the match sit inside a
# theorem that actually depends on the declared theorem. A vacuous
# `THEOREM <declared-theorem> == TRUE  OBVIOUS`, preceded by a plain TLA+
# block comment containing the literal invariant-name text, still satisfied
# "All N obligations proved." (for a real, if vacuous, TLAPS run) and the
# invariant grep, and check-proofs.sh exited 0.
#
# The fix (this PR, round 2) strips all TLA+ comments before matching, finds
# each top-level THEOREM's own statement text, and only accepts an invariant
# as proved if it appears in the STATEMENT of the declared theorem or of a
# theorem that (transitively) cites the declared theorem in a `BY` clause --
# i.e. the declared theorem itself or one of its derived corollaries, never
# a decoy comment or an unrelated theorem.
#
# This script builds a throwaway proofs/ directory containing only a
# deliberately vacuous CidxResultProof.tla (the real invariant names appear
# ONLY inside a decoy comment, never inside any theorem statement) and reuses
# this checkout's real modules/ and manifest.json unmodified, so the only
# thing under test is check-proofs.sh's binding logic itself. It proves:
#
#   1. The vacuous module + decoy comment is REJECTED by the fixed
#      check-proofs.sh with reason=proves-invariant-not-found (defect
#      confirmed absent).
#   2. The real, unmodified proofs/CidxResultProof.tla from this checkout
#      still PASSES (the fix does not regress the legitimate case, whose
#      declared theorem's own statement does not literally contain either
#      invariant name -- only its two derived corollary theorems do).
#
# check-proofs.sh's tlapm toolchain is x86_64-linux-gnu only and this
# checkout's convention (see check-verification-tamper-regression.sh) is not
# to require it for local/macOS runs: if a working tlapm is not already
# materialized at TLA_PROOFS_PREFIX (or the default /tmp/tlaps-1.5.0), this
# script prints a SKIP status and exits 0 rather than attempting an install
# or reporting a false pass. It never reports PASS without having actually
# exercised the real TLAPS toolchain end to end.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
CHECK_PROOFS_SH="${CIDX_CHECK_PROOFS_SH:-$REPO_ROOT/spec/tla/tools/check-proofs.sh}"
TOOLS_VERSION="1.5.0"
TLAPS_PREFIX="${TLA_PROOFS_PREFIX:-${TMPDIR:-/tmp}/tlaps-${TOOLS_VERSION}}"

die() {
  echo "TLA_PROOF_BINDING_REGRESSION_STATUS=FAIL reason=$1" >&2
  exit 1
}

if [[ ! -x "$TLAPS_PREFIX/bin/tlapm" ]]; then
  echo "TLA_PROOF_BINDING_REGRESSION_STATUS=SKIP reason=tlapm-not-materialized-at-$TLAPS_PREFIX"
  exit 0
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cidx-tla-proof-binding-regression.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

PROOF_DIR="$WORK/proofs"
mkdir -p "$PROOF_DIR"

run_check() {
  local seed_file="$1"
  cp "$seed_file" "$PROOF_DIR/CidxResultProof.tla"
  set +e
  local output
  output="$(
    TLA_MODULE_DIR="$REPO_ROOT/spec/tla/modules" \
    TLA_PROOF_DIR="$PROOF_DIR" \
    TLA_MANIFEST="$REPO_ROOT/spec/tla/manifest.json" \
    TLA_PROOFS_PREFIX="$TLAPS_PREFIX" \
    "$CHECK_PROOFS_SH" 2>&1
  )"
  local status=$?
  set -e
  printf '%s\n%s\n' "$status" "$output"
}

# --- Test 1: vacuous theorem, invariant names present only in a decoy
# comment, must be REJECTED. ---

VACUOUS_SEED="$WORK/vacuous-seed.tla"
cat >"$VACUOUS_SEED" <<'TLA'
----------------------------- MODULE CidxResultProof -----------------------------
(* Decoy comment: a defective binding check could be fooled by this literal
   text alone, without either invariant actually being proved by anything:
   []SharedResultTypeInvariant []TrustedOutcomeInvariant *)
EXTENDS CidxResult, TLAPS

THEOREM Trivial1 == 1 = 1 OBVIOUS

THEOREM ResultInvarianceTheorem == TRUE  OBVIOUS

=============================================================================
TLA

result="$(run_check "$VACUOUS_SEED")"
vacuous_status="$(sed -n '1p' <<<"$result")"
vacuous_output="$(sed -n '2,$p' <<<"$result")"

if [[ "$vacuous_status" -eq 0 ]]; then
  printf '%s\n' "$vacuous_output" >&2
  die "vacuous-proof-with-decoy-comment-passed"
fi
if ! grep -q "reason=proves-invariant-not-found" <<<"$vacuous_output"; then
  printf '%s\n' "$vacuous_output" >&2
  die "vacuous-proof-rejected-for-unexpected-reason"
fi
if ! grep -q "SharedResultTypeInvariant" <<<"$vacuous_output" \
    || ! grep -q "TrustedOutcomeInvariant" <<<"$vacuous_output"; then
  printf '%s\n' "$vacuous_output" >&2
  die "rejection-did-not-name-both-missing-invariants"
fi
echo "TLA_PROOF_BINDING_REGRESSION_STATUS=PASS check=vacuous-proof-with-decoy-comment-rejected"

# --- Test 2: the real, unmodified proof module must still PASS. ---

result="$(run_check "$REPO_ROOT/spec/tla/proofs/CidxResultProof.tla")"
real_status="$(sed -n '1p' <<<"$result")"
real_output="$(sed -n '2,$p' <<<"$result")"

if [[ "$real_status" -ne 0 ]]; then
  printf '%s\n' "$real_output" >&2
  die "real-proof-module-unexpectedly-failed"
fi
if ! grep -q "^TLA_PROOF_STATUS=PASS module=CidxResultProof" <<<"$real_output"; then
  printf '%s\n' "$real_output" >&2
  die "real-proof-module-missing-expected-pass-line"
fi
echo "TLA_PROOF_BINDING_REGRESSION_STATUS=PASS check=real-proof-module-still-passes"

echo "TLA_PROOF_BINDING_REGRESSION_STATUS=PASS"
