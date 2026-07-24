---------------------------- MODULE CidxProtected ----------------------------

EXTENDS CidxTypes

(*
 * This module is hand-authored policy.  Generated specifications may consume
 * these predicates but must never rewrite this file or weaken its operators.
 *)

ProtectedStatuses(records) ==
    \A record \in records : record.status \in ResultStatuses

ProtectedGenerationStatuses(records) ==
    \A record \in records : record.status \in GenerationStatuses

ProtectedEvidence(records) ==
    \A evidence \in records :
        evidence.kind # "assumption"
            \/ evidence.trust = "trusted-assumption"

=============================================================================
