#!/usr/bin/env bash
#
# Regression proving the QA round-2 and senior-developer round-3 fixes to
# check-proofs.sh's F1 theorem/invariant binding check: the round-1 fix
# required manifest.json's declared theorem to exist as a THEOREM in the
# checked module, and each declared invariant's name to appear literally
# somewhere as "[]<Invariant>" -- but that grep ran over the WHOLE module
# file, comments included, with no requirement that the match sit inside a
# theorem that actually depends on the declared theorem. A vacuous
# `THEOREM <declared-theorem> == TRUE  OBVIOUS`, preceded by a plain TLA+
# block comment containing the literal invariant-name text, still satisfied
# "All N obligations proved." (for a real, if vacuous, TLAPS run) and the
# invariant grep, and check-proofs.sh exited 0.
#
# The round-2 fix strips all TLA+ comments before matching, finds each
# top-level THEOREM's own statement text, and only accepts an invariant as
# proved if it appears in the STATEMENT of a theorem in the BY-citation
# closure of the declared theorem (the declared theorem itself, or a
# corollary that transitively cites it) -- but round-2's "appears in" test
# was still a bare substring search for "[]<Invariant>" anywhere in that
# statement, with no requirement on shape or polarity. The round-3 fix
# (senior-developer acceptance review) proved that gap live against real
# TLAPS: a module whose declared theorem is the vacuous `TRUE OBVIOUS` above,
# plus a second theorem `Negated == (~([]SharedResultTypeInvariant) /\\
# ~([]TrustedOutcomeInvariant)) => TRUE BY <declared-theorem>` -- citing the
# declared theorem so it enters the closure, and containing both invariant
# names, but only inside a negated antecedent that proves nothing about
# either invariant actually holding -- still produced
# `TLA_PROOF_STATUS=PASS ... invariants=SharedResultTypeInvariant,TrustedOutcomeInvariant`.
#
# The round-3 fix requires a theorem's statement to structurally match
# `<Spec> => []<Invariant>` for a real, module-defined Spec operator, not
# merely contain that text as a substring -- but round 3's own resolution of
# "real, module-defined Spec operator" scanned every .tla file
# check-proofs.sh had copied into its scratch work directory, including the
# untrusted proof module itself. A proof module defining a fresh,
# module-local `WeakSpec == Init /\ FALSE` (a contradiction) was accepted as
# a real Spec operator, making every corollary built on it vacuously true:
# PROVEN end-to-end against real tlapm 1.5.0, offline, in the acceptance
# review that rejected round 3. The round-4 fix (this round) resolves the
# candidate Spec operator only from manifest.json's "extends" entry and its
# transitive EXTENDS chain, resolved only from modules/conformance/protected
# (never proofs/), and requires the real `Init /\ [][Next]_vars` idiom, not
# a bare "Init" prefix -- see check-proofs-binding.sh for the fix itself and
# check-proofs-binding-unit-test.sh for a fast, tlapm-free pin of both halves
# of that fix. This script proves the same defect end to end, against a
# real tlapm run:
#
#   1. The vacuous module + decoy comment is REJECTED by the fixed
#      check-proofs.sh with reason=proves-invariant-not-found (round-1/2
#      defect confirmed absent).
#   2. The negated-antecedent module (round-3's own repro) is REJECTED with
#      the same reason (round-3 defect confirmed absent).
#   3. A weakened-antecedent module (statement literally
#      `FALSE => []<Invariant>`, so the invariant text is present, unnegated,
#      and directly implied -- but the antecedent is not this module's real
#      Spec operator) is also REJECTED.
#   4. The real, unmodified proofs/CidxResultProof.tla from this checkout
#      still PASSES (the fix does not regress the legitimate case, whose
#      declared theorem's own statement does not literally contain either
#      invariant name -- only its two derived corollary theorems do, each
#      structurally `Spec => []<Invariant>`).
#   5. A module-local, fabricated `WeakSpec == Init /\ FALSE` operator
#      (round-4's own repro, the live bypass that defeated round 3) is
#      REJECTED.
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

ASSUME QueryIdWellFormed == QueryId \in QueryIds
ASSUME ResultIdWellFormed == ResultId \in ResultIds
ASSUME EvidenceIdWellFormed == EvidenceId \in EvidenceIds

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

# --- Test 2: negated-antecedent decoy (senior-developer round-3 acceptance
# repro), must be REJECTED. Both invariant names appear, unnegated text is
# present, and the citing theorem is genuinely in the BY-citation closure --
# but only inside a negation that proves nothing. ---

NEGATION_SEED="$WORK/negation-seed.tla"
cat >"$NEGATION_SEED" <<'TLA'
----------------------------- MODULE CidxResultProof -----------------------------
EXTENDS CidxResult, TLAPS

ASSUME QueryIdWellFormed == QueryId \in QueryIds
ASSUME ResultIdWellFormed == ResultId \in ResultIds
ASSUME EvidenceIdWellFormed == EvidenceId \in EvidenceIds

THEOREM ResultInvarianceTheorem == TRUE OBVIOUS

THEOREM Negated == (~([]SharedResultTypeInvariant) /\ ~([]TrustedOutcomeInvariant)) => TRUE
  BY ResultInvarianceTheorem

=============================================================================
TLA

result="$(run_check "$NEGATION_SEED")"
negation_status="$(sed -n '1p' <<<"$result")"
negation_output="$(sed -n '2,$p' <<<"$result")"

if [[ "$negation_status" -eq 0 ]]; then
  printf '%s\n' "$negation_output" >&2
  die "negated-antecedent-decoy-passed"
fi
if ! grep -q "reason=proves-invariant-not-found" <<<"$negation_output"; then
  printf '%s\n' "$negation_output" >&2
  die "negated-antecedent-decoy-rejected-for-unexpected-reason"
fi
if ! grep -q "SharedResultTypeInvariant" <<<"$negation_output" \
    || ! grep -q "TrustedOutcomeInvariant" <<<"$negation_output"; then
  printf '%s\n' "$negation_output" >&2
  die "negated-antecedent-rejection-did-not-name-both-missing-invariants"
fi
echo "TLA_PROOF_BINDING_REGRESSION_STATUS=PASS check=negated-antecedent-decoy-rejected"

# --- Test 3: weakened-antecedent decoy, must be REJECTED. The invariant text
# is present, unnegated, and directly implied by a bare implication -- but
# the antecedent (FALSE) is not this module's real Spec operator, so the
# claim is vacuously true and proves nothing about the actual specification.
# ---

WEAK_ANTECEDENT_SEED="$WORK/weak-antecedent-seed.tla"
cat >"$WEAK_ANTECEDENT_SEED" <<'TLA'
----------------------------- MODULE CidxResultProof -----------------------------
EXTENDS CidxResult, TLAPS

ASSUME QueryIdWellFormed == QueryId \in QueryIds
ASSUME ResultIdWellFormed == ResultId \in ResultIds
ASSUME EvidenceIdWellFormed == EvidenceId \in EvidenceIds

THEOREM ResultInvarianceTheorem == TRUE OBVIOUS

THEOREM WeakCorollary1 == FALSE => []SharedResultTypeInvariant
  BY ResultInvarianceTheorem

THEOREM WeakCorollary2 == FALSE => []TrustedOutcomeInvariant
  BY ResultInvarianceTheorem

=============================================================================
TLA

result="$(run_check "$WEAK_ANTECEDENT_SEED")"
weak_status="$(sed -n '1p' <<<"$result")"
weak_output="$(sed -n '2,$p' <<<"$result")"

if [[ "$weak_status" -eq 0 ]]; then
  printf '%s\n' "$weak_output" >&2
  die "weakened-antecedent-decoy-passed"
fi
if ! grep -q "reason=proves-invariant-not-found" <<<"$weak_output"; then
  printf '%s\n' "$weak_output" >&2
  die "weakened-antecedent-decoy-rejected-for-unexpected-reason"
fi
if ! grep -q "SharedResultTypeInvariant" <<<"$weak_output" \
    || ! grep -q "TrustedOutcomeInvariant" <<<"$weak_output"; then
  printf '%s\n' "$weak_output" >&2
  die "weakened-antecedent-rejection-did-not-name-both-missing-invariants"
fi
echo "TLA_PROOF_BINDING_REGRESSION_STATUS=PASS check=weakened-antecedent-decoy-rejected"

# --- Test 4: a real proof module with an added top-level ASSUME FALSE must be
# rejected by the end-to-end checker after TLAPS has accepted the vacuous
# obligations. This is load-bearing: a regression that only checks theorem
# shape, or never invokes check_proof_assumptions(), would let this exact seed
# exit successfully. ---

ASSUME_FALSE_SEED="$WORK/assume-false-seed.tla"
sed '/^ASSUME QueryIdWellFormed/i ASSUME FALSE' \
  "$REPO_ROOT/spec/tla/proofs/CidxResultProof.tla" >"$ASSUME_FALSE_SEED"

result="$(run_check "$ASSUME_FALSE_SEED")"
assume_false_status="$(sed -n '1p' <<<"$result")"
assume_false_output="$(sed -n '2,$p' <<<"$result")"

if [[ "$assume_false_status" -eq 0 ]]; then
  printf '%s\n' "$assume_false_output" >&2
  die "assume-false-proof-passed-end-to-end"
fi
if ! grep -q "reason=vacuous-proof-assumption:FALSE" <<<"$assume_false_output"; then
  printf '%s\n' "$assume_false_output" >&2
  die "assume-false-proof-rejected-for-unexpected-reason"
fi
echo "TLA_PROOF_BINDING_REGRESSION_STATUS=PASS check=assume-false-proof-rejected-end-to-end"

# --- Test 5: the real, unmodified proof module must still PASS. ---

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

# --- Test 6: minted module-local pseudo-Spec decoy (QA + senior-developer
# round-2 acceptance review repro), must be REJECTED. `WeakSpec` is defined
# ONLY inside this untrusted proof module, its body starts with "Init" (so
# the round-3 substring-prefix resolution accepted it), but it is a bare
# contradiction (`Init /\ FALSE`), not this module's real Spec -- and it is
# not defined anywhere in modules/CidxResult.tla's trusted EXTENDS chain. ---

WEAK_SPEC_SEED="$WORK/weak-spec-seed.tla"
cat >"$WEAK_SPEC_SEED" <<'TLA'
----------------------------- MODULE CidxResultProof -----------------------------
EXTENDS CidxResult, TLAPS

ASSUME QueryIdWellFormed == QueryId \in QueryIds
ASSUME ResultIdWellFormed == ResultId \in ResultIds
ASSUME EvidenceIdWellFormed == EvidenceId \in EvidenceIds

WeakSpec == Init /\ FALSE

THEOREM ResultInvarianceTheorem == TRUE OBVIOUS

THEOREM WeakCorollary1 == WeakSpec => []SharedResultTypeInvariant
  BY ResultInvarianceTheorem, PTL DEF WeakSpec

THEOREM WeakCorollary2 == WeakSpec => []TrustedOutcomeInvariant
  BY ResultInvarianceTheorem, PTL DEF WeakSpec

=============================================================================
TLA

result="$(run_check "$WEAK_SPEC_SEED")"
weak_spec_status="$(sed -n '1p' <<<"$result")"
weak_spec_output="$(sed -n '2,$p' <<<"$result")"

if [[ "$weak_spec_status" -eq 0 ]]; then
  printf '%s\n' "$weak_spec_output" >&2
  die "minted-weak-spec-decoy-passed"
fi
if ! grep -q "reason=proves-invariant-not-found" <<<"$weak_spec_output"; then
  printf '%s\n' "$weak_spec_output" >&2
  die "minted-weak-spec-decoy-rejected-for-unexpected-reason"
fi
if ! grep -q "SharedResultTypeInvariant" <<<"$weak_spec_output" \
    || ! grep -q "TrustedOutcomeInvariant" <<<"$weak_spec_output"; then
  printf '%s\n' "$weak_spec_output" >&2
  die "minted-weak-spec-rejection-did-not-name-both-missing-invariants"
fi
echo "TLA_PROOF_BINDING_REGRESSION_STATUS=PASS check=minted-weak-spec-decoy-rejected"

echo "TLA_PROOF_BINDING_REGRESSION_STATUS=PASS"
