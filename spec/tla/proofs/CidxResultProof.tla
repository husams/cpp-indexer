----------------------------- MODULE CidxResultProof -----------------------------
(***************************************************************************)
(* TLAPS-checked inductive invariance proof for CidxResult.tla.  This is    *)
(* the assurance level above finite TLC model checking: TLC only proves    *)
(* SharedResultTypeInvariant, SharedStatusInvariant, and                    *)
(* TrustedOutcomeInvariant for the one concrete singleton instance in       *)
(* models/CidxResultSmoke.cfg.  This module proves the same claim, plus the *)
(* shared record-type invariant, for every constant instantiation that      *)
(* satisfies the well-formedness ASSUMEs below -- i.e. for arbitrary        *)
(* (possibly infinite) QueryIds/ResultIds/EvidenceIds domains, not just the *)
(* one finite sample TLC explored.  See spec/tla/ASSURANCE.md for why this  *)
(* invariant was chosen for TLAPS and what TLC-only coverage does not show. *)
(***************************************************************************)
EXTENDS CidxResult, TLAPS

(* The smoke model config always chooses a witness element for each of      *)
(* these constants; this module states that requirement explicitly so the   *)
(* proof holds for every constant choice consistent with the finite models, *)
(* not only the specific literal identifiers already checked by TLC.        *)
ASSUME QueryIdWellFormed == QueryId \in QueryIds
ASSUME ResultIdWellFormed == ResultId \in ResultIds
ASSUME EvidenceIdWellFormed == EvidenceId \in EvidenceIds

(* One inductive invariant carrying both the shared record-type shape and   *)
(* the trusted-outcome safety claim through every step. *)
Invariant == IsResult(result) /\ TrustedOutcomeInvariant

LEMMA InitImpliesInvariant == Init => Invariant
  BY QueryIdWellFormed, ResultIdWellFormed, EvidenceIdWellFormed
  DEF Init, Invariant, IsResult, TrustedOutcomeInvariant, ResultStatuses

LEMMA NextPreservesInvariant == Invariant /\ [Next]_vars => Invariant'
  <1> SUFFICES ASSUME Invariant, [Next]_vars
               PROVE Invariant'
    OBVIOUS
  <1>1. [Next]_vars <=>
          \/ Observe        \/ Infer            \/ BoundedVerify
          \/ ProveUnderAssumptions \/ Refute     \/ MarkUnknown
          \/ Terminal        \/ UNCHANGED vars
    BY DEF Next, Advance
  <1>2. CASE Observe
    BY <1>2, EvidenceIdWellFormed
    DEF Observe, Invariant, IsResult, TrustedOutcomeInvariant, ResultStatuses
  <1>3. CASE Infer
    BY <1>3 DEF Infer, Invariant, IsResult, TrustedOutcomeInvariant, ResultStatuses
  <1>4. CASE BoundedVerify
    BY <1>4
    DEF BoundedVerify, Invariant, IsResult, TrustedOutcomeInvariant, ResultStatuses
  <1>5. CASE ProveUnderAssumptions
    BY <1>5, EvidenceIdWellFormed
    DEF ProveUnderAssumptions, Invariant, IsResult, TrustedOutcomeInvariant,
        ResultStatuses
  <1>6. CASE Refute
    BY <1>6, EvidenceIdWellFormed
    DEF Refute, Invariant, IsResult, TrustedOutcomeInvariant, ResultStatuses
  <1>7. CASE MarkUnknown
    BY <1>7
    DEF MarkUnknown, Invariant, IsResult, TrustedOutcomeInvariant, ResultStatuses
  <1>8. CASE Terminal
    BY <1>8 DEF Terminal, Invariant, IsResult, TrustedOutcomeInvariant
  <1>9. CASE UNCHANGED vars
    BY <1>9 DEF vars, Invariant, IsResult, TrustedOutcomeInvariant
  <1>10. QED
    BY <1>1, <1>2, <1>3, <1>4, <1>5, <1>6, <1>7, <1>8, <1>9

THEOREM ResultInvarianceTheorem == Spec => []Invariant
  BY InitImpliesInvariant, NextPreservesInvariant, PTL DEF Spec

(* Read off the two normative smoke-checked invariants from the proved      *)
(* inductive invariant, so the theorem is stated in terms the manifest and  *)
(* TLC models already name. *)
THEOREM ResultTypeAlwaysHolds == Spec => []SharedResultTypeInvariant
  BY ResultInvarianceTheorem, PTL DEF Invariant, SharedResultTypeInvariant

THEOREM TrustedOutcomeAlwaysHolds == Spec => []TrustedOutcomeInvariant
  BY ResultInvarianceTheorem, PTL DEF Invariant

=============================================================================
