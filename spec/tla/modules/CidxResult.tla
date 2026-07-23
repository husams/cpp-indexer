----------------------------- MODULE CidxResult -----------------------------

EXTENDS CidxTypes

CONSTANTS QueryId, ResultId, EvidenceId

VARIABLES result

vars == <<result>>

Init ==
    result = [
        id |-> ResultId,
        query |-> QueryId,
        status |-> "suggested",
        evidence |-> {},
        assumptions |-> {}
    ]

Observe ==
    /\ result.status = "suggested"
    /\ result' = [result EXCEPT
        !.status = "observed",
        !.evidence = {EvidenceId}]

Infer ==
    /\ result.status = "observed"
    /\ result' = [result EXCEPT !.status = "inferred"]

BoundedVerify ==
    /\ result.status \in {"observed", "inferred"}
    /\ result' = [result EXCEPT !.status = "bounded-verified"]

ProveUnderAssumptions ==
    /\ result.status = "bounded-verified"
    /\ result' = [result EXCEPT
        !.status = "proved-under-assumptions",
        !.assumptions = {EvidenceId}]

Refute ==
    /\ result.status \in {"suggested", "observed", "inferred"}
    /\ result' = [result EXCEPT
        !.status = "refuted",
        !.evidence = {EvidenceId}]

MarkUnknown ==
    /\ result.status \in ResultStatuses \ {"refuted"}
    /\ result' = [result EXCEPT !.status = "unknown"]

Advance == Observe \/ Infer \/ BoundedVerify \/ ProveUnderAssumptions
    \/ Refute \/ MarkUnknown

Terminal ==
    /\ result.status \in {"proved-under-assumptions", "refuted", "unknown"}
    /\ UNCHANGED result

Next == Advance \/ Terminal

Fairness == WF_vars(Advance)

Spec == Init /\ [][Next]_vars /\ Fairness

SharedResultTypeInvariant == IsResult(result)

SharedStatusInvariant == result.status \in ResultStatuses

TrustedOutcomeInvariant ==
    result.status = "proved-under-assumptions" => result.assumptions # {}

=============================================================================
