#!/usr/bin/env bash
#
# Fast, tlapm-free regression for check-proofs-binding.sh's
# theorem_invariant_binding() -- the structural theorem/invariant binding
# logic check-proofs.sh uses to reject a vacuous TLAPS proof. Unlike
# check-proofs-vacuous-comment-regression.sh (which requires a real,
# materialized tlapm 1.5.0 install and SKIPs otherwise), this script only
# exercises the binding function directly against synthetic fixtures: it
# needs no tlapm, no network, and no C toolchain, so it always runs.
#
# It exists specifically to pin the round-4 fix (QA + senior-developer round
# 2 acceptance review): the round-3 structural binding check resolved its
# candidate "<Spec>" operator name by scanning EVERY .tla file
# check-proofs.sh had copied into its own scratch work directory -- which
# includes the untrusted proofs/ tree a PR under review controls -- for any
# operator whose body merely started with the token "Init". A proof module
# defining a fresh, module-local `WeakSpec == Init /\ FALSE` (a contradiction)
# was accepted as a real "module-defined Spec operator", making every
# corollary built on it vacuously true. PROVEN end-to-end against real tlapm
# 1.5.0, offline, in the acceptance review that rejected round 3.
#
# The round-4 fix resolves the candidate operator name ONLY from
# manifest.json's "extends" entry for this proof and that module's own
# transitive EXTENDS chain, each hop resolved by searching only
# modules/conformance/protected (never proofs/), and additionally requires
# the candidate's definition to match the full temporal-specification idiom
# `Init /\ [][Next]_vars`, not merely start with the bare token "Init". This
# script pins both halves of that fix independently.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
# shellcheck source=check-proofs-binding.sh
source "$REPO_ROOT/spec/tla/tools/check-proofs-binding.sh"

die() {
  echo "TLA_PROOF_BINDING_UNIT_STATUS=FAIL reason=$1" >&2
  exit 1
}

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cidx-tla-proof-binding-unit.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/modules" "$WORK/proofs"

write_manifest() {
  local extends="$1"
  # $2, if given, is a JSON array literal (already quoted/escaped by the
  # caller) for proofs[0].trustedAssumptions; defaults to empty since none
  # of cases 1-6 below put any ASSUME in the proof module.
  local trusted_assumptions="${2:-[]}"
  cat >"$WORK/manifest.json" <<JSON
{
  "proofs": [
    {
      "module": "proofs/FooProof.tla",
      "extends": "$extends",
      "theorem": "T",
      "provesInvariants": ["Inv"],
      "trustedAssumptions": $trusted_assumptions
    }
  ]
}
JSON
}

# --- Case 1: the real, well-formed idiom is accepted. ---
cat >"$WORK/modules/Foo.tla" <<'TLA'
----------------------------- MODULE Foo -----------------------------
VARIABLES x
vars == <<x>>
Init == x = 0
Next == x' = x
Fairness == WF_vars(Next)
Spec == Init /\ [][Next]_vars /\ Fairness
Inv == TRUE
=============================================================================
TLA
cat >"$WORK/proofs/FooProof.tla" <<'TLA'
----------------------------- MODULE FooProof -----------------------------
EXTENDS Foo, TLAPS
THEOREM T == Spec => []Inv
  PROOF OBVIOUS
=============================================================================
TLA
write_manifest "modules/Foo.tla"
result="$(theorem_invariant_binding "$WORK/manifest.json" FooProof "$WORK/proofs/FooProof.tla" "$WORK")"
[[ "$result" == "OK:T:Inv" ]] || die "legit-case-not-accepted:$result"
echo "TLA_PROOF_BINDING_UNIT_STATUS=PASS check=legit-real-spec-idiom-accepted"

# --- Case 2: a minted pseudo-Spec defined INSIDE the untrusted proof file
# itself, whose body starts with "Init" but is a contradiction, must never be
# treated as a trusted spec operator -- both because it is not sourced from
# modules/, and because its shape (Init /\ FALSE) does not match the real
# idiom even if it were. This is the exact live bypass proven against real
# tlapm 1.5.0 in the round-3 acceptance review. ---
cat >"$WORK/proofs/FooProof.tla" <<'TLA'
----------------------------- MODULE FooProof -----------------------------
EXTENDS Foo, TLAPS
WeakSpec == Init /\ FALSE
THEOREM T == WeakSpec => []Inv
  BY DEF WeakSpec
=============================================================================
TLA
result="$(theorem_invariant_binding "$WORK/manifest.json" FooProof "$WORK/proofs/FooProof.tla" "$WORK")"
[[ "$result" == INVARIANT-NOT-FOUND:* ]] \
  || die "minted-weak-spec-in-proof-file-accepted:$result"
echo "TLA_PROOF_BINDING_UNIT_STATUS=PASS check=minted-weak-spec-in-proof-file-rejected"

# --- Case 3: a correctly-shaped Spec operator (real "Init /\ [][Next]_vars"
# idiom, not a contradiction) defined ONLY in the untrusted proof file, never
# in a trusted module, must still be rejected -- proves the "never
# proofs/" restriction is load-bearing independent of the shape check. ---
cat >"$WORK/proofs/FooProof.tla" <<'TLA'
----------------------------- MODULE FooProof -----------------------------
EXTENDS Foo, TLAPS
VARIABLES y
yvars == <<y>>
YInit == y = 0
YNext == y' = y
LocalSpec == YInit /\ [][YNext]_yvars
THEOREM T == LocalSpec => []Inv
  BY DEF LocalSpec
=============================================================================
TLA
result="$(theorem_invariant_binding "$WORK/manifest.json" FooProof "$WORK/proofs/FooProof.tla" "$WORK")"
[[ "$result" == INVARIANT-NOT-FOUND:* ]] \
  || die "well-shaped-spec-in-proof-file-accepted:$result"
echo "TLA_PROOF_BINDING_UNIT_STATUS=PASS check=well-shaped-spec-defined-only-in-proof-file-rejected"

# --- Case 4: a decoy "Init"-prefixed but wrongly-shaped operator placed in a
# TRUSTED module that is NOT reachable via the declared proof's "extends"
# entry (a sibling module, not in the EXTENDS chain) must not be picked up
# either -- proves discovery is scoped to the extends chain, not "any file
# under modules/". ---
mkdir -p "$WORK/modules"
cat >"$WORK/modules/Unrelated.tla" <<'TLA'
----------------------------- MODULE Unrelated -----------------------------
FakeSpec == Init /\ FALSE
=============================================================================
TLA
cat >"$WORK/proofs/FooProof.tla" <<'TLA'
----------------------------- MODULE FooProof -----------------------------
EXTENDS Foo, TLAPS
THEOREM T == FakeSpec => []Inv
  PROOF OBVIOUS
=============================================================================
TLA
result="$(theorem_invariant_binding "$WORK/manifest.json" FooProof "$WORK/proofs/FooProof.tla" "$WORK")"
[[ "$result" == INVARIANT-NOT-FOUND:* ]] \
  || die "unreachable-sibling-module-decoy-accepted:$result"
echo "TLA_PROOF_BINDING_UNIT_STATUS=PASS check=decoy-in-unreachable-sibling-module-rejected"

# --- Case 5: the real idiom defined two EXTENDS hops away (extends entry ->
# Middle -> Base, Base defines the real Spec) must be discovered
# transitively, proving the chain walk is not limited to a single hop. ---
cat >"$WORK/modules/Base.tla" <<'TLA'
----------------------------- MODULE Base -----------------------------
VARIABLES z
zvars == <<z>>
Init == z = 0
Next == z' = z
Fairness == WF_zvars(Next)
Spec == Init /\ [][Next]_zvars /\ Fairness
=============================================================================
TLA
cat >"$WORK/modules/Middle.tla" <<'TLA'
----------------------------- MODULE Middle -----------------------------
EXTENDS Base
=============================================================================
TLA
cat >"$WORK/proofs/FooProof.tla" <<'TLA'
----------------------------- MODULE FooProof -----------------------------
EXTENDS Middle, TLAPS
THEOREM T == Spec => []Inv
  PROOF OBVIOUS
=============================================================================
TLA
write_manifest "modules/Middle.tla"
result="$(theorem_invariant_binding "$WORK/manifest.json" FooProof "$WORK/proofs/FooProof.tla" "$WORK")"
[[ "$result" == "OK:T:Inv" ]] || die "transitive-extends-chain-not-resolved:$result"
echo "TLA_PROOF_BINDING_UNIT_STATUS=PASS check=transitive-extends-chain-resolved"

# --- Case 6: a decoy `Init /\ FALSE` operator placed INSIDE the actual,
# extends-chain-reachable trusted module (not merely a candidate that a
# directory restriction alone would exclude) must still be rejected -- this
# isolates the shape-requirement half of the fix from the
# never-search-proofs/ half. Even a trusted, reachable module defining a
# contradiction alongside its real Spec must not let that contradiction
# stand in for the real specification. ---
cat >"$WORK/modules/Foo.tla" <<'TLA'
----------------------------- MODULE Foo -----------------------------
VARIABLES x
vars == <<x>>
Init == x = 0
Next == x' = x
Fairness == WF_vars(Next)
Spec == Init /\ [][Next]_vars /\ Fairness
FakeSpec == Init /\ FALSE
Inv == TRUE
=============================================================================
TLA
cat >"$WORK/proofs/FooProof.tla" <<'TLA'
----------------------------- MODULE FooProof -----------------------------
EXTENDS Foo, TLAPS
THEOREM T == FakeSpec => []Inv
  BY DEF FakeSpec
=============================================================================
TLA
write_manifest "modules/Foo.tla"
result="$(theorem_invariant_binding "$WORK/manifest.json" FooProof "$WORK/proofs/FooProof.tla" "$WORK")"
[[ "$result" == INVARIANT-NOT-FOUND:* ]] \
  || die "in-module-contradiction-decoy-accepted:$result"
echo "TLA_PROOF_BINDING_UNIT_STATUS=PASS check=in-module-init-and-false-decoy-rejected-by-shape-check"

# --- Case 7: the exact acceptance-review report -- `ASSUME FALSE` added to
# the REAL, checked-in proof module -- must be rejected even though the
# THEOREM itself keeps the correct `Spec => []Invariant` shape (a fixed
# shape check alone, per cases 1-6, cannot see this: the vulnerability is
# in the module's premises, not its theorem statement). Run against the
# real spec/tla/manifest.json and spec/tla/modules/CidxResult.tla (not the
# synthetic Foo fixture), proving the fix protects the actual shipped
# proof, not just a toy analogue. ---
cat >"$WORK/proofs/CidxResultProof.tla" <<'TLA'
----------------------------- MODULE CidxResultProof -----------------------------
EXTENDS CidxResult, TLAPS

ASSUME FALSE

THEOREM ResultInvarianceTheorem == Spec => []Invariant
  BY DEF Spec, Invariant, IsResult, TrustedOutcomeInvariant

THEOREM ResultTypeAlwaysHolds == Spec => []SharedResultTypeInvariant
  BY ResultInvarianceTheorem, PTL DEF Invariant, SharedResultTypeInvariant

THEOREM TrustedOutcomeAlwaysHolds == Spec => []TrustedOutcomeInvariant
  BY ResultInvarianceTheorem, PTL DEF Invariant

Invariant == IsResult(result) /\ TrustedOutcomeInvariant
=============================================================================
TLA
result="$(theorem_invariant_binding "$REPO_ROOT/spec/tla/manifest.json" CidxResultProof "$WORK/proofs/CidxResultProof.tla" "$REPO_ROOT/spec/tla")"
[[ "$result" == "ASSUMPTION-VACUOUS:FALSE" ]] \
  || die "assume-false-on-real-proof-module-accepted:$result"
echo "TLA_PROOF_BINDING_UNIT_STATUS=PASS check=assume-false-on-real-proof-module-rejected"

# --- Case 8: `FALSE` need not stand alone -- a conjunct of a larger ASSUME
# (`ASSUME SomethingElse /\ FALSE`) is exactly as vacuous and must be
# rejected the same way; the check must look at conjuncts, not merely
# whole-assumption text equality with the literal string "FALSE". ---
mkdir -p "$WORK/modules" "$WORK/proofs"
cat >"$WORK/modules/Foo.tla" <<'TLA'
----------------------------- MODULE Foo -----------------------------
VARIABLES x
vars == <<x>>
Init == x = 0
Next == x' = x
Fairness == WF_vars(Next)
Spec == Init /\ [][Next]_vars /\ Fairness
Inv == TRUE
=============================================================================
TLA
cat >"$WORK/proofs/FooProof.tla" <<'TLA'
----------------------------- MODULE FooProof -----------------------------
EXTENDS Foo, TLAPS
ASSUME Decoy == x = 0 /\ FALSE
THEOREM T == Spec => []Inv
  PROOF OBVIOUS
=============================================================================
TLA
write_manifest "modules/Foo.tla" '["ASSUME Decoy == x = 0 /\\ FALSE"]'
result="$(theorem_invariant_binding "$WORK/manifest.json" FooProof "$WORK/proofs/FooProof.tla" "$WORK")"
[[ "$result" == "ASSUMPTION-VACUOUS:FALSE" ]] \
  || die "false-conjunct-inside-larger-assumption-accepted:$result"
echo "TLA_PROOF_BINDING_UNIT_STATUS=PASS check=false-conjunct-inside-larger-assumption-rejected"

# --- Case 9: no single assumption is `FALSE`, but two assumptions are
# direct negations of each other, so the set is jointly unsatisfiable --
# must be rejected even though every individual assumption looks innocuous
# and would pass an allowlist that (mistakenly) trusted both. ---
cat >"$WORK/proofs/FooProof.tla" <<'TLA'
----------------------------- MODULE FooProof -----------------------------
EXTENDS Foo, TLAPS
ASSUME PositiveCase == x = 0
ASSUME NegativeCase == ~(x = 0)
THEOREM T == Spec => []Inv
  PROOF OBVIOUS
=============================================================================
TLA
write_manifest "modules/Foo.tla" \
  '["ASSUME PositiveCase == x = 0", "ASSUME NegativeCase == ~(x = 0)"]'
result="$(theorem_invariant_binding "$WORK/manifest.json" FooProof "$WORK/proofs/FooProof.tla" "$WORK")"
[[ "$result" == "ASSUMPTIONS-CONTRADICT:x=0" ]] \
  || die "contradictory-assumption-pair-accepted:$result"
echo "TLA_PROOF_BINDING_UNIT_STATUS=PASS check=contradictory-assumption-pair-rejected"

# --- Case 10: every assumption is individually satisfiable (no FALSE, no
# internal contradiction) but is not on manifest.json's declared
# trustedAssumptions allowlist for this proof -- must be rejected. This is
# the other half of the fix: even a plausible-looking, non-vacuous
# assumption must be pre-declared and reviewed, not merely "not obviously
# broken". ---
cat >"$WORK/proofs/FooProof.tla" <<'TLA'
----------------------------- MODULE FooProof -----------------------------
EXTENDS Foo, TLAPS
ASSUME Unreviewed == x \in {0, 1, 2}
THEOREM T == Spec => []Inv
  PROOF OBVIOUS
=============================================================================
TLA
write_manifest "modules/Foo.tla"
result="$(theorem_invariant_binding "$WORK/manifest.json" FooProof "$WORK/proofs/FooProof.tla" "$WORK")"
[[ "$result" == "ASSUMPTION-NOT-TRUSTED:ASSUME Unreviewed == x \in {0, 1, 2}" ]] \
  || die "untrusted-non-vacuous-assumption-accepted:$result"
echo "TLA_PROOF_BINDING_UNIT_STATUS=PASS check=untrusted-non-vacuous-assumption-rejected"

# --- Case 11: the identical assumption from case 10, but declared on the
# manifest's trustedAssumptions allowlist, is accepted -- proving the fix
# does not regress a legitimately reviewed, satisfiable well-formedness
# assumption (exactly the shape of the three real ASSUMEs in
# proofs/CidxResultProof.tla). ---
write_manifest "modules/Foo.tla" '["ASSUME Unreviewed == x \\in {0, 1, 2}"]'
result="$(theorem_invariant_binding "$WORK/manifest.json" FooProof "$WORK/proofs/FooProof.tla" "$WORK")"
[[ "$result" == "OK:T:Inv" ]] \
  || die "allowlisted-non-vacuous-assumption-rejected:$result"
echo "TLA_PROOF_BINDING_UNIT_STATUS=PASS check=allowlisted-non-vacuous-assumption-accepted"

# --- Case 12: `FALSE` wrapped in a single layer of parens (`ASSUME (FALSE)`)
# must be caught exactly like the bare literal -- proves
# top_level_conjuncts()'s one-layer paren unwrap actually fires, rather than
# leaving a parenthesized conjunct as opaque text no check ever inspects. ---
cat >"$WORK/proofs/FooProof.tla" <<'TLA'
----------------------------- MODULE FooProof -----------------------------
EXTENDS Foo, TLAPS
ASSUME (FALSE)
THEOREM T == Spec => []Inv
  PROOF OBVIOUS
=============================================================================
TLA
write_manifest "modules/Foo.tla"
result="$(theorem_invariant_binding "$WORK/manifest.json" FooProof "$WORK/proofs/FooProof.tla" "$WORK")"
[[ "$result" == "ASSUMPTION-VACUOUS:FALSE" ]] \
  || die "parenthesized-false-accepted:$result"
echo "TLA_PROOF_BINDING_UNIT_STATUS=PASS check=parenthesized-false-rejected"

# --- Case 13: negation without any wrapping parens (`~Flag`, not `~(Flag)`)
# must still be recognized as the negation of a plain conjunct with the same
# text -- proves negated_form()'s bare-tilde branch (not just its `~(...)`
# branch, already exercised by case 9) is load-bearing. A second, unrelated
# bare negation with no matching positive counterpart in the same module
# must NOT be flagged -- proves the check does not over-fire on every `~`
# it sees. ---
cat >"$WORK/proofs/FooProof.tla" <<'TLA'
----------------------------- MODULE FooProof -----------------------------
EXTENDS Foo, TLAPS
ASSUME PosSimple == Flag
ASSUME NegSimple == ~Flag
THEOREM T == Spec => []Inv
  PROOF OBVIOUS
=============================================================================
TLA
write_manifest "modules/Foo.tla" \
  '["ASSUME PosSimple == Flag", "ASSUME NegSimple == ~Flag"]'
result="$(theorem_invariant_binding "$WORK/manifest.json" FooProof "$WORK/proofs/FooProof.tla" "$WORK")"
[[ "$result" == "ASSUMPTIONS-CONTRADICT:Flag" ]] \
  || die "bare-tilde-contradiction-accepted:$result"
echo "TLA_PROOF_BINDING_UNIT_STATUS=PASS check=bare-tilde-contradiction-rejected"

cat >"$WORK/proofs/FooProof.tla" <<'TLA'
----------------------------- MODULE FooProof -----------------------------
EXTENDS Foo, TLAPS
ASSUME OnlyNegative == ~Flag
THEOREM T == Spec => []Inv
  PROOF OBVIOUS
=============================================================================
TLA
write_manifest "modules/Foo.tla" '["ASSUME OnlyNegative == ~Flag"]'
result="$(theorem_invariant_binding "$WORK/manifest.json" FooProof "$WORK/proofs/FooProof.tla" "$WORK")"
[[ "$result" == "OK:T:Inv" ]] \
  || die "lone-bare-negation-without-counterpart-falsely-flagged:$result"
echo "TLA_PROOF_BINDING_UNIT_STATUS=PASS check=lone-bare-negation-without-counterpart-accepted"

echo "TLA_PROOF_BINDING_UNIT_STATUS=PASS"
