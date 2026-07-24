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

(* These predicates are the protected review boundary for implementation
 * changes that claim conformance with the behavioral specification. *)
NoPartialPublication(indexState, publicationState, artifactState, currentGeneration) ==
    publicationState = "current"
        => /\ indexState = "current"
           /\ artifactState \in {"published", "derived"}
           /\ currentGeneration > 0

ReadOnlyQueries(queryWrites) == queryWrites = 0

PreservePublishedGeneration(storageState, currentGeneration, migrationBaseline) ==
    storageState \in {"migrating", "recovery-required"}
        => currentGeneration = migrationBaseline

HonestPartialResults(queryResultState, queryTruncated) ==
    queryResultState = "partial" => queryTruncated

=============================================================================
