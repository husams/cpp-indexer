--------------------------- MODULE CidxSemanticGraph --------------------------

EXTENDS CidxTypes, FiniteSets, Naturals, Sequences

(***************************************************************************)
(* M2 semantic contract.  The tuples below are abstract graph and query    *)
(* values.  They intentionally do not name C++ classes, SQL tables, or a   *)
(* traversal implementation.  The smoke model supplies a finite universe *)
(* and can replace one fact with a named seeded defect.                    *)
(***************************************************************************)

CONSTANTS
    GraphNodeIds,
    PathStart,
    PathTarget,
    MaxDepth,
    InitialIndexVersion,
    LeftSet,
    RightSet,
    AdversarialBound,
    Defect,
    TraceBound

GraphNodeKinds == {"workspace", "repository", "component", "source",
                   "namespace", "record", "function", "method", "field",
                   "variable", "type", "unknown"}

RelationTargetStates == {"known", "unknown"}
PlanSources == {"graph", "view"}
PlanViews == {"nodes", "relations", "evidence", "paths"}
PlanFilters == {"none", "predicate"}
PlanStreams == {"set", "path"}
PlanOperations == {"identity", "filter", "traverse", "union",
                   "intersection", "difference", "select"}
PlanSelections == {"node", "relation", "evidence", "path"}
PlanOrders == {"canonical", "none"}
CompletenessStates == {"complete", "partial", "unknown"}
ResultStates == {"none", "complete", "partial", "unknown"}
DerivedTransformStates == {"absent", "planned", "current", "stale", "failed"}
Defects == {"none", "illegal-stream", "invalid-witness", "duplicate-results",
            "query-write", "complete-truncated", "complete-unknown", "stale-transform",
            "failed-transform", "partial-transform"}

GraphNodes == {
    <<"node-1", "function">>,
    <<"node-2", "function">>,
    <<"node-3", "function">>,
    <<"record-1", "record">>,
    <<"unknown-target", "unknown">>
}

GraphRelations == {
    <<"rel-1", "calls", "node-1", "node-2", "known",
     {"evidence-1"}, <<1, 2>>>>,
    <<"rel-2", "calls", "node-2", "node-3", "known",
     {"evidence-2"}, <<1>>>>,
    <<"rel-3", "inherits", "record-1", "unknown-target", "unknown",
     {"evidence-3"}, <<>>>>
}

GraphEvidence == {
    <<"evidence-1", "rel-1", "source", TRUE>>,
    <<"evidence-2", "rel-2", "source", TRUE>>,
    <<"evidence-3", "rel-3", "model", FALSE>>
}

CyclePath == <<"node-1", "node-2", "node-1">>
CycleEdges == {<<"node-1", "node-2">>, <<"node-2", "node-1">>}
DiamondEdges == {
    <<"node-1", "node-2">>, <<"node-1", "node-3">>,
    <<"node-2", "node-3">>, <<"node-3", "node-2">>
}
FanoutTargets == {"node-1", "node-2", "node-3"}
IncompleteTargetIds == {"unknown-target"}
PartialEvidenceIds == {"evidence-3"}
CanonicalResultOrder == <<"node-1", "node-2", "node-3">>

VARIABLES
    candidatePlan,
    planValidated,
    planRejected,
    queryExecuted,
    resultReturned,
    resultStatus,
    resultItems,
    resultOrder,
    witnessPath,
    inputCompleteness,
    inputTruncated,
    inputUnknown,
    abstractIndexVersion,
    queryWrites,
    currentGeneration,
    transformState,
    transformInputComplete,
    transformOutputPublished,
    transformGeneration,
    requiredFactSet,
    outputFactSet,
    transformConsumed,
    seeded,
    trace

EdgeEndpointsValid(edges) ==
    \A e \in edges :
        /\ e[1] \in GraphNodeIds
        /\ e[2] \in GraphNodeIds

CycleAdversarialInvariant ==
    /\ CyclePath \in Seq(GraphNodeIds)
    /\ Len(CyclePath) > 2
    /\ CyclePath[1] = CyclePath[Len(CyclePath)]
    /\ Len(CyclePath) - 1 <= AdversarialBound
    /\ \A i \in 1..(Len(CyclePath) - 1) :
        <<CyclePath[i], CyclePath[i + 1]>> \in CycleEdges

DiamondAdversarialInvariant ==
    /\ EdgeEndpointsValid(DiamondEdges)
    /\ Cardinality(DiamondEdges) = 4
    /\ DiamondEdges \subseteq (GraphNodeIds \X GraphNodeIds)
    /\ Cardinality(DiamondEdges) <= AdversarialBound

FanoutAdversarialInvariant ==
    /\ FanoutTargets # {}
    /\ FanoutTargets \subseteq GraphNodeIds
    /\ Cardinality(FanoutTargets) <= AdversarialBound

IncompleteTargetAdversarialInvariant ==
    /\ IncompleteTargetIds \subseteq GraphNodeIds
    /\ "unknown-target" \in IncompleteTargetIds
    /\ \E r \in GraphRelations : r[4] \in IncompleteTargetIds

PartialEvidenceAdversarialInvariant ==
    /\ PartialEvidenceIds \subseteq EvidenceIds
    /\ PartialEvidenceIds # {}
    /\ \E e \in GraphEvidence :
        /\ e[1] \in PartialEvidenceIds
        /\ ~e[4]

SemanticVars == <<
    candidatePlan,
    planValidated,
    planRejected,
    queryExecuted,
    resultReturned,
    resultStatus,
    resultItems,
    resultOrder,
    witnessPath,
    inputCompleteness,
    inputTruncated,
    inputUnknown,
    abstractIndexVersion,
    queryWrites,
    currentGeneration,
    transformState,
    transformInputComplete,
    transformOutputPublished,
    transformGeneration,
    requiredFactSet,
    outputFactSet,
    transformConsumed,
    seeded,
    trace
>>

vars == SemanticVars

SequenceValues(s) == {s[i] : i \in 1..Len(s)}

DistinctSequence(s) ==
    Len(s) = Cardinality(SequenceValues(s))

NodeKind(id) == CHOOSE n \in GraphNodes : n[1] = id

ValidNode(n) ==
    /\ n[1] \in GraphNodeIds
    /\ n[2] \in GraphNodeKinds

ValidGraphRelation(r) ==
    /\ r[1] \in RelationIds
    /\ r[2] \in RelationKinds
    /\ r[3] \in GraphNodeIds
    /\ r[4] \in GraphNodeIds
    /\ r[5] \in RelationTargetStates
    /\ r[6] \subseteq EvidenceIds
    /\ r[7] \in Seq(Nat)

EndpointCompatible(r) ==
    \/ r[2] = "contains"
        /\ NodeKind(r[3])[2] \in {"workspace", "repository", "component",
                                         "source", "namespace", "record"}
        /\ NodeKind(r[4])[2] # "unknown"
    \/ r[2] = "declares"
        /\ NodeKind(r[3])[2] \in {"namespace", "record"}
        /\ NodeKind(r[4])[2] \in {"namespace", "record", "function",
                                         "method", "field", "variable"}
    \/ r[2] = "uses"
        /\ NodeKind(r[3])[2] \in {"function", "method", "field", "variable"}
        /\ NodeKind(r[4])[2] # "unknown"
    \/ r[2] = "calls"
        /\ NodeKind(r[3])[2] \in {"function", "method"}
        /\ NodeKind(r[4])[2] \in {"function", "method"}
    \/ r[2] = "inherits"
        /\ NodeKind(r[3])[2] = "record"
        /\ NodeKind(r[4])[2] \in {"record", "unknown"}
    \/ r[2] = "defines"
        /\ NodeKind(r[3])[2] \in {"record", "function", "method"}
        /\ NodeKind(r[4])[2] \in {"record", "function", "method"}

GraphNodeInvariant ==
    /\ GraphNodes # {}
    /\ \A n \in GraphNodes : ValidNode(n)
    /\ Cardinality({n[1] : n \in GraphNodes}) = Cardinality(GraphNodes)

GraphRelationInvariant ==
    /\ GraphRelations # {}
    /\ \A r \in GraphRelations :
        /\ ValidGraphRelation(r)
        /\ EndpointCompatible(r)
        /\ r[5] = "unknown" => NodeKind(r[4])[2] = "unknown"
        /\ r[5] = "known" => NodeKind(r[4])[2] # "unknown"
        /\ DistinctSequence(r[7])
    /\ Cardinality({r[1] : r \in GraphRelations}) = Cardinality(GraphRelations)

EvidenceOwnershipInvariant ==
    /\ \A e \in GraphEvidence :
        /\ e[1] \in EvidenceIds
        /\ e[2] \in RelationIds
        /\ e[3] \in EvidenceKinds
        /\ e[4] \in BOOLEAN
        /\ \E r \in GraphRelations :
            /\ r[1] = e[2]
            /\ e[1] \in r[6]
    /\ \A r \in GraphRelations :
        \A e \in GraphEvidence : e[1] \in r[6] => e[2] = r[1]
    /\ Cardinality({e[1] : e \in GraphEvidence}) = Cardinality(GraphEvidence)

UnknownTargetInvariant ==
    \A r \in GraphRelations :
        r[5] = "unknown"
            => /\ NodeKind(r[4])[2] = "unknown"
               /\ r[4] \in GraphNodeIds

SemanticGraphInvariant ==
    /\ GraphNodeInvariant
    /\ GraphRelationInvariant
    /\ EvidenceOwnershipInvariant
    /\ UnknownTargetInvariant

PlanValid(p) ==
    /\ p.source \in PlanSources
    /\ p.view \in PlanViews
    /\ p.filter \in PlanFilters
    /\ p.stream \in PlanStreams
    /\ p.operation \in PlanOperations
    /\ p.selection \in PlanSelections
    /\ p.order \in PlanOrders
    /\ p.depth \in Nat
    /\ p.depth <= MaxDepth
    /\ p.limit \in Nat
    /\ p.limit > 0
    /\ p.view = "paths" =>
        /\ p.stream = "path"
        /\ p.operation = "traverse"
        /\ p.selection = "path"
        /\ p.depth > 0
    /\ p.stream = "path" => p.view = "paths"
    /\ p.view = "evidence" => p.stream = "set"
    /\ p.operation \in {"union", "intersection", "difference"}
        => p.stream = "set"
    /\ p.operation = "traverse" => p.depth > 0
    /\ p.order = "none" => p.stream = "path"

PlanTransitionInvariant ==
    /\ planValidated => PlanValid(candidatePlan)
    /\ (planValidated /\ candidatePlan.stream = "path")
        => candidatePlan.view = "paths"
    /\ planRejected => ~planValidated
    /\ queryExecuted => planValidated

SetOperationResult(op, left, right) ==
    IF op = "union" THEN left \cup right
    ELSE IF op = "intersection" THEN left \cap right
    ELSE IF op = "difference" THEN left \ right
    ELSE left

SetSemanticsInvariant ==
    /\ resultItems = SetOperationResult("union", LeftSet, RightSet)
    /\ resultOrder = CanonicalResultOrder
    /\ SequenceValues(resultOrder) = resultItems
    /\ DistinctSequence(resultOrder)

PathHasEdges(path) ==
    \A i \in 1..(Len(path) - 1) :
        \E r \in GraphRelations :
            /\ r[3] = path[i]
            /\ r[4] = path[i + 1]
            /\ r[5] = "known"

WitnessPathValid(path, bound) ==
    /\ path \in Seq(GraphNodeIds)
    /\ Len(path) > 1
    /\ path[1] = PathStart
    /\ path[Len(path)] = PathTarget
    /\ Len(path) - 1 <= bound
    /\ DistinctSequence(path)
    /\ PathHasEdges(path)

WitnessInvariant ==
    resultReturned /\ candidatePlan.stream = "path"
        => WitnessPathValid(witnessPath, candidatePlan.depth)

CompletenessInvariant ==
    resultReturned /\ resultStatus = "complete"
        => /\ inputCompleteness = "complete"
           /\ ~inputTruncated
           /\ ~inputUnknown

ReadOnlyExecutionInvariant ==
    /\ queryWrites = 0
    /\ queryExecuted => abstractIndexVersion = InitialIndexVersion

TransformPublicationInvariant ==
    /\ transformState = "current"
        => /\ transformInputComplete
           /\ transformOutputPublished
           /\ transformGeneration = currentGeneration
           /\ requiredFactSet = outputFactSet
    /\ transformState \in {"stale", "failed"}
        => /\ ~transformOutputPublished
           /\ ~transformConsumed

TransformConsumptionInvariant ==
    transformConsumed
        => /\ transformState = "current"
           /\ transformOutputPublished
           /\ transformGeneration = currentGeneration
           /\ requiredFactSet = outputFactSet

TransformInvariant ==
    /\ TransformPublicationInvariant
    /\ TransformConsumptionInvariant

TypeInvariant ==
    /\ candidatePlan \in [
        source: PlanSources,
        view: PlanViews,
        filter: PlanFilters,
        stream: PlanStreams,
        operation: PlanOperations,
        selection: PlanSelections,
        order: PlanOrders,
        depth: Nat,
        limit: Nat
        ]
    /\ planValidated \in BOOLEAN
    /\ planRejected \in BOOLEAN
    /\ queryExecuted \in BOOLEAN
    /\ resultReturned \in BOOLEAN
    /\ resultStatus \in ResultStates
    /\ resultItems \subseteq GraphNodeIds
    /\ resultOrder \in Seq(GraphNodeIds)
    /\ witnessPath \in Seq(GraphNodeIds)
    /\ inputCompleteness \in CompletenessStates
    /\ inputTruncated \in BOOLEAN
    /\ inputUnknown \in BOOLEAN
    /\ abstractIndexVersion \in Nat
    /\ queryWrites \in Nat
    /\ currentGeneration \in Nat
    /\ transformState \in DerivedTransformStates
    /\ transformInputComplete \in BOOLEAN
    /\ transformOutputPublished \in BOOLEAN
    /\ transformGeneration \in Nat
    /\ requiredFactSet \in STRING
    /\ outputFactSet \in STRING
    /\ transformConsumed \in BOOLEAN
    /\ seeded \in BOOLEAN
    /\ Len(trace) <= TraceBound

Init ==
    /\ candidatePlan = [
        source |-> "graph",
        view |-> "paths",
        filter |-> "none",
        stream |-> "path",
        operation |-> "traverse",
        selection |-> "path",
        order |-> "canonical",
        depth |-> 2,
        limit |-> 10
        ]
    /\ planValidated = FALSE
    /\ planRejected = FALSE
    /\ queryExecuted = FALSE
    /\ resultReturned = FALSE
    /\ resultStatus = "none"
    /\ resultItems = SetOperationResult("union", LeftSet, RightSet)
    /\ resultOrder = CanonicalResultOrder
    /\ witnessPath = <<PathStart, "node-2", PathTarget>>
    /\ inputCompleteness = "complete"
    /\ inputTruncated = FALSE
    /\ inputUnknown = FALSE
    /\ abstractIndexVersion = InitialIndexVersion
    /\ queryWrites = 0
    /\ currentGeneration = 1
    /\ transformState = "current"
    /\ transformInputComplete = TRUE
    /\ transformOutputPublished = TRUE
    /\ transformGeneration = 1
    /\ requiredFactSet = "resolved-facts-v1"
    /\ outputFactSet = "resolved-facts-v1"
    /\ transformConsumed = FALSE
    /\ seeded = FALSE
    /\ trace = <<"Init">>

ValidatePlan ==
    /\ ~planValidated
    /\ ~planRejected
    /\ PlanValid(candidatePlan)
    /\ planValidated' = TRUE
    /\ planRejected' = FALSE
    /\ trace' = Append(trace, "ValidatePlan")
    /\ UNCHANGED <<candidatePlan, queryExecuted, resultReturned,
                    resultStatus, resultItems, resultOrder, witnessPath,
                    inputCompleteness, inputTruncated, inputUnknown,
                    abstractIndexVersion, queryWrites, currentGeneration,
                    transformState, transformInputComplete,
                    transformOutputPublished, transformGeneration,
                    requiredFactSet, outputFactSet, transformConsumed, seeded>>

ExecuteQuery ==
    /\ planValidated
    /\ ~queryExecuted
    /\ queryExecuted' = TRUE
    /\ trace' = Append(trace, "ExecuteQuery")
    /\ UNCHANGED <<candidatePlan, planValidated, planRejected, resultReturned,
                    resultStatus, resultItems, resultOrder, witnessPath,
                    inputCompleteness, inputTruncated, inputUnknown,
                    abstractIndexVersion, queryWrites, currentGeneration,
                    transformState, transformInputComplete,
                    transformOutputPublished, transformGeneration,
                    requiredFactSet, outputFactSet, transformConsumed, seeded>>

ReturnResult ==
    /\ queryExecuted
    /\ ~resultReturned
    /\ resultReturned' = TRUE
    /\ resultStatus' = "complete"
    /\ trace' = Append(trace, "ReturnResult")
    /\ UNCHANGED <<candidatePlan, planValidated, planRejected, queryExecuted,
                    resultItems, resultOrder, witnessPath, inputCompleteness,
                    inputTruncated, inputUnknown, abstractIndexVersion,
                    queryWrites, currentGeneration, transformState,
                    transformInputComplete, transformOutputPublished,
                    transformGeneration, requiredFactSet, outputFactSet,
                    transformConsumed, seeded>>

ConsumeTransform ==
    /\ transformState = "current"
    /\ transformOutputPublished
    /\ ~transformConsumed
    /\ transformConsumed' = TRUE
    /\ trace' = Append(trace, "ConsumeTransform")
    /\ UNCHANGED <<candidatePlan, planValidated, planRejected, queryExecuted,
                    resultReturned, resultStatus, resultItems, resultOrder,
                    witnessPath, inputCompleteness, inputTruncated,
                    inputUnknown, abstractIndexVersion, queryWrites,
                    currentGeneration, transformState, transformInputComplete,
                    transformOutputPublished, transformGeneration,
                    requiredFactSet, outputFactSet, seeded>>

SeedDefect ==
    /\ Defect \in Defects \ {"none"}
    /\ ~seeded
    /\ seeded' = TRUE
    /\ candidatePlan' =
        IF Defect = "illegal-stream"
        THEN [candidatePlan EXCEPT !.stream = "path", !.view = "relations"]
        ELSE candidatePlan
    /\ planValidated' = planValidated \/ Defect = "illegal-stream"
    /\ resultReturned' = resultReturned \/ Defect = "invalid-witness"
    /\ resultStatus' = IF Defect \in {"complete-truncated", "complete-unknown"}
                         THEN "complete"
                         ELSE resultStatus
    /\ resultOrder' = IF Defect = "duplicate-results"
                      THEN <<"node-1", "node-1", "node-2", "node-3">>
                      ELSE resultOrder
    /\ witnessPath' = IF Defect = "invalid-witness"
                      THEN <<PathStart, PathTarget>>
                      ELSE witnessPath
    /\ inputCompleteness' = IF Defect = "complete-truncated" THEN "partial"
                           ELSE inputCompleteness
    /\ inputTruncated' = inputTruncated \/ Defect = "complete-truncated"
    /\ inputUnknown' = inputUnknown \/ Defect = "complete-unknown"
    /\ queryExecuted' = queryExecuted \/ Defect = "query-write"
    /\ queryWrites' = IF Defect = "query-write" THEN 1 ELSE queryWrites
    /\ abstractIndexVersion' = IF Defect = "query-write"
                               THEN InitialIndexVersion + 1
                               ELSE abstractIndexVersion
    /\ transformState' = IF Defect = "stale-transform" THEN "stale"
                        ELSE IF Defect = "failed-transform" THEN "failed"
                        ELSE transformState
    /\ transformInputComplete' = IF Defect = "partial-transform" THEN FALSE
                                 ELSE transformInputComplete
    /\ transformOutputPublished' = IF Defect \in {"stale-transform",
                                                   "failed-transform"}
                                   THEN FALSE
                                   ELSE transformOutputPublished
    /\ transformConsumed' = transformConsumed \/ Defect \in {"stale-transform",
                                                               "failed-transform",
                                                               "partial-transform"}
    /\ trace' = Append(trace, Defect)
    /\ UNCHANGED <<planRejected, resultItems, currentGeneration,
                    transformGeneration, requiredFactSet, outputFactSet>>

NoOp == UNCHANGED SemanticVars

Next == ValidatePlan \/ ExecuteQuery \/ ReturnResult \/ ConsumeTransform
    \/ SeedDefect \/ NoOp

Fairness ==
    /\ WF_vars(ValidatePlan)
    /\ WF_vars(ExecuteQuery)
    /\ WF_vars(ReturnResult)
    /\ WF_vars(ConsumeTransform)
    /\ WF_vars(SeedDefect)

Spec == Init /\ [][Next]_SemanticVars /\ Fairness

SemanticLiveness ==
    [](planValidated /\ ~queryExecuted => <> queryExecuted)

SemanticInvariant ==
    /\ TypeInvariant
    /\ SemanticGraphInvariant
    /\ PlanTransitionInvariant
    /\ SetSemanticsInvariant
    /\ WitnessInvariant
    /\ CompletenessInvariant
    /\ ReadOnlyExecutionInvariant
    /\ TransformInvariant
    /\ CycleAdversarialInvariant
    /\ DiamondAdversarialInvariant
    /\ FanoutAdversarialInvariant
    /\ IncompleteTargetAdversarialInvariant
    /\ PartialEvidenceAdversarialInvariant

=============================================================================
