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
PlanViews == {"graph", "nodes", "relations", "evidence", "paths"}
PlanFilters == {"none", "predicate"}
PlanStreams == {"set", "path"}
PlanOperations == {"identity", "filter", "traverse", "union",
                   "intersection", "difference", "select"}
PlanSelections == {"node", "relation", "evidence", "path"}
PlanOrders == {"canonical", "none"}
CompletenessStates == {"complete", "partial", "unknown"}
ResultStates == {"none", "complete", "partial", "unknown"}
DerivedTransformStates == {"absent", "planned", "current", "stale", "failed"}
PlanStepOperations == {"source", "filter", "traverse", "union",
                       "intersection", "difference", "select", "order", "limit"}
TransformLifecycleStates == {"planned", "running", "published", "stale", "failed"}
TransformNames == {"resolve", "answer"}
FactSets == {"raw-facts", "resolved-facts-v1", "answer-facts-v1"}
TransformDependencyEdges == {<<"answer", "resolve">>}
TransformRequiredFacts == {<<"resolve", "raw-facts">>,
                           <<"answer", "resolved-facts-v1">>}
TransformProducedFacts == {<<"resolve", "resolved-facts-v1">>,
                           <<"answer", "answer-facts-v1">>}
Defects == {"none", "illegal-stream", "illegal-source", "illegal-filter", "illegal-traverse",
            "illegal-set", "illegal-select", "illegal-order", "illegal-limit",
            "invalid-witness", "duplicate-results", "query-write",
            "complete-truncated", "complete-unknown", "stale-transform",
            "stale-fact-consumption", "failed-transform", "partial-transform"}

PlanStepRecord == [operation: PlanStepOperations,
                   inputView: PlanViews,
                   inputStream: PlanStreams,
                   outputView: PlanViews,
                   outputStream: PlanStreams,
                   selection: PlanSelections,
                   leftView: PlanViews,
                   rightView: PlanViews,
                   depth: Nat,
                   limit: Nat]

ValidSetPlan ==
    <<[operation |-> "source", inputView |-> "graph", inputStream |-> "set",
       outputView |-> "nodes", outputStream |-> "set", selection |-> "node",
       leftView |-> "nodes", rightView |-> "nodes", depth |-> 0, limit |-> 10],
      [operation |-> "filter", inputView |-> "nodes", inputStream |-> "set",
       outputView |-> "nodes", outputStream |-> "set", selection |-> "node",
       leftView |-> "nodes", rightView |-> "nodes", depth |-> 0, limit |-> 10],
      [operation |-> "union", inputView |-> "nodes", inputStream |-> "set",
       outputView |-> "nodes", outputStream |-> "set", selection |-> "node",
       leftView |-> "nodes", rightView |-> "nodes", depth |-> 0, limit |-> 10],
      [operation |-> "select", inputView |-> "nodes", inputStream |-> "set",
       outputView |-> "nodes", outputStream |-> "set", selection |-> "node",
       leftView |-> "nodes", rightView |-> "nodes", depth |-> 0, limit |-> 10],
      [operation |-> "order", inputView |-> "nodes", inputStream |-> "set",
       outputView |-> "nodes", outputStream |-> "set", selection |-> "node",
       leftView |-> "nodes", rightView |-> "nodes", depth |-> 0, limit |-> 10],
      [operation |-> "limit", inputView |-> "nodes", inputStream |-> "set",
       outputView |-> "nodes", outputStream |-> "set", selection |-> "node",
       leftView |-> "nodes", rightView |-> "nodes", depth |-> 0, limit |-> 10]>>

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
    candidateSteps,
    planValidated,
    planRejected,
    queryExecuted,
    resultReturned,
    resultStatus,
    resultItems,
    resultOrder,
    resultRelations,
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
    transformLifecycle,
    transformReused,
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
    candidateSteps,
    planValidated,
    planRejected,
    queryExecuted,
    resultReturned,
    resultStatus,
    resultItems,
    resultOrder,
    resultRelations,
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
    transformLifecycle,
    transformReused,
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

PlanStepShapeValid(step) ==
    /\ step.operation \in PlanStepOperations
    /\ step.inputView \in PlanViews
    /\ step.inputStream \in PlanStreams
    /\ step.outputView \in PlanViews
    /\ step.outputStream \in PlanStreams
    /\ step.selection \in PlanSelections
    /\ step.leftView \in PlanViews
    /\ step.rightView \in PlanViews
    /\ step.depth \in Nat
    /\ step.depth <= MaxDepth
    /\ step.limit \in Nat
    /\ step.limit > 0

PlanStepTransitionValid(previous, step) ==
    /\ step.inputView = previous.outputView
    /\ step.inputStream = previous.outputStream
    /\ (step.operation = "source")
        => /\ step.inputView = "graph"
           /\ step.inputStream = "set"
           /\ step.outputView = "nodes"
           /\ step.outputStream = "set"
    /\ (step.operation = "filter")
        => /\ step.inputView \in {"nodes", "relations", "evidence"}
           /\ step.inputStream = "set"
           /\ step.outputView = step.inputView
           /\ step.outputStream = "set"
    /\ (step.operation = "traverse")
        => /\ step.inputView \in {"nodes", "relations"}
           /\ step.inputStream = "set"
           /\ step.outputView = "paths"
           /\ step.outputStream = "path"
           /\ step.selection = "path"
           /\ step.depth > 0
    /\ (step.operation \in {"union", "intersection", "difference"})
        => /\ step.inputView \in {"nodes", "relations", "evidence"}
           /\ step.inputStream = "set"
           /\ step.leftView = step.inputView
           /\ step.rightView = step.inputView
           /\ step.outputView = step.inputView
           /\ step.outputStream = "set"
    /\ (step.operation = "select" /\ step.selection = "node")
        => /\ step.inputView = "nodes"
           /\ step.inputStream = "set"
           /\ step.outputView = "nodes"
           /\ step.outputStream = "set"
    /\ (step.operation = "select" /\ step.selection = "relation")
        => /\ step.inputView = "relations"
           /\ step.inputStream = "set"
           /\ step.outputView = "relations"
           /\ step.outputStream = "set"
    /\ (step.operation = "select" /\ step.selection = "evidence")
        => /\ step.inputView = "evidence"
           /\ step.inputStream = "set"
           /\ step.outputView = "evidence"
           /\ step.outputStream = "set"
    /\ (step.operation = "select" /\ step.selection = "path")
        => /\ step.inputView = "paths"
           /\ step.inputStream = "path"
           /\ step.outputView = "paths"
           /\ step.outputStream = "path"
    /\ (step.operation \in {"order", "limit"})
        => /\ step.outputView = step.inputView
           /\ step.outputStream = step.inputStream

PlanStepsValid(steps) ==
    /\ steps \in Seq(PlanStepRecord)
    /\ Len(steps) > 0
    /\ steps[1].operation = "source"
    /\ \A i \in 1..Len(steps) : PlanStepShapeValid(steps[i])
    /\ \A i \in 2..Len(steps) : PlanStepTransitionValid(steps[i - 1], steps[i])

TransformDependencyInvariant ==
    /\ TransformNames # {}
    /\ \A edge \in TransformDependencyEdges :
        /\ edge[1] \in TransformNames
        /\ edge[2] \in TransformNames
        /\ edge[1] # edge[2]
    /\ \A requirement \in TransformRequiredFacts :
        /\ requirement[1] \in TransformNames
        /\ requirement[2] \in FactSets
    /\ \A product \in TransformProducedFacts :
        /\ product[1] \in TransformNames
        /\ product[2] \in FactSets

ResultRelationsForItems(items) ==
    IF items = LeftSet \cup RightSet
    THEN {"rel-1", "rel-2"}
    ELSE IF items = LeftSet \cap RightSet
    THEN {"rel-1", "rel-2"}
    ELSE {"rel-1"}

TraversedRelations(path) ==
    IF path = <<PathStart, "node-2", PathTarget>>
    THEN {"rel-1", "rel-2"}
    ELSE {}

EvidenceCompleteForRelations(relations) ==
    \A e \in GraphEvidence :
        e[2] \in relations => e[4]

UnknownTargetForRelations(relations) ==
    \E r \in GraphRelations : r[1] \in relations /\ r[5] = "unknown"

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
    /\ planValidated => /\ PlanValid(candidatePlan)
                          /\ PlanStepsValid(candidateSteps)
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
    /\ candidatePlan.operation \in {"union", "intersection", "difference"}
    /\ resultItems = SetOperationResult(candidatePlan.operation, LeftSet, RightSet)
    /\ resultOrder =
        IF candidatePlan.operation = "union" THEN CanonicalResultOrder
        ELSE IF candidatePlan.operation = "intersection" THEN <<"node-2">>
        ELSE <<"node-1">>
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
    resultReturned => WitnessPathValid(witnessPath, 2)

CompletenessInvariant ==
    resultReturned /\ resultStatus = "complete"
        => /\ inputCompleteness = "complete"
           /\ ~inputTruncated
           /\ ~inputUnknown
           /\ resultRelations = ResultRelationsForItems(resultItems)
           /\ resultRelations = TraversedRelations(witnessPath)
           /\ EvidenceCompleteForRelations(resultRelations)
           /\ ~UnknownTargetForRelations(resultRelations)

ReadOnlyExecutionInvariant ==
    /\ queryWrites = 0
    /\ queryExecuted => abstractIndexVersion = InitialIndexVersion

TransformPublicationInvariant ==
    /\ transformState = "current"
        => /\ transformInputComplete
           /\ transformOutputPublished
           /\ transformGeneration = currentGeneration
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
    /\ TransformDependencyInvariant
    /\ TransformPublicationInvariant
    /\ TransformConsumptionInvariant
    /\ transformLifecycle = "published"
        => /\ transformState = "current"
           /\ transformOutputPublished
           /\ transformInputComplete
           /\ transformGeneration = currentGeneration
    /\ transformLifecycle = "failed"
        => /\ transformState = "failed"
           /\ ~transformOutputPublished
           /\ ~transformConsumed
    /\ transformLifecycle = "stale"
        => /\ transformState = "stale"
           /\ ~transformOutputPublished
           /\ ~transformConsumed
    /\ transformReused
        => /\ transformLifecycle = "published"
           /\ transformState = "current"
           /\ transformOutputPublished
           /\ transformGeneration = currentGeneration

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
    /\ candidateSteps \in Seq(PlanStepRecord)
    /\ planValidated \in BOOLEAN
    /\ planRejected \in BOOLEAN
    /\ queryExecuted \in BOOLEAN
    /\ resultReturned \in BOOLEAN
    /\ resultStatus \in ResultStates
    /\ resultItems \subseteq GraphNodeIds
    /\ resultOrder \in Seq(GraphNodeIds)
    /\ resultRelations \subseteq RelationIds
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
    /\ transformLifecycle \in TransformLifecycleStates
    /\ transformReused \in BOOLEAN
    /\ seeded \in BOOLEAN
    /\ Len(trace) <= TraceBound

Init ==
    /\ candidatePlan = [
        source |-> "graph",
        view |-> "nodes",
        filter |-> "none",
        stream |-> "set",
        operation |-> "union",
        selection |-> "node",
        order |-> "canonical",
        depth |-> 0,
        limit |-> 10
        ]
    /\ candidateSteps = ValidSetPlan
    /\ planValidated = FALSE
    /\ planRejected = FALSE
    /\ queryExecuted = FALSE
    /\ resultReturned = FALSE
    /\ resultStatus = "none"
    /\ resultItems = SetOperationResult("union", LeftSet, RightSet)
    /\ resultOrder = CanonicalResultOrder
    /\ resultRelations = {"rel-1", "rel-2"}
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
    /\ transformLifecycle = "published"
    /\ transformReused = FALSE
    /\ seeded = FALSE
    /\ trace = <<"Init">>

ValidatePlan ==
    /\ ~planValidated
    /\ ~planRejected
    /\ PlanValid(candidatePlan)
    /\ planValidated' = TRUE
    /\ planRejected' = FALSE
    /\ trace' = Append(trace, "ValidatePlan")
    /\ UNCHANGED <<candidatePlan, candidateSteps, queryExecuted, resultReturned,
                    resultStatus, resultItems, resultOrder, resultRelations,
                    witnessPath,
                    inputCompleteness, inputTruncated, inputUnknown,
                    abstractIndexVersion, queryWrites, currentGeneration,
                    transformState, transformInputComplete,
                    transformOutputPublished, transformGeneration,
                    requiredFactSet, outputFactSet, transformConsumed,
                    transformLifecycle, transformReused, seeded>>

ExecuteQuery ==
    /\ planValidated
    /\ ~queryExecuted
    /\ queryExecuted' = TRUE
    /\ trace' = Append(trace, "ExecuteQuery")
    /\ UNCHANGED <<candidatePlan, candidateSteps, planValidated, planRejected,
                    resultReturned, resultStatus, resultItems, resultOrder,
                    resultRelations, witnessPath,
                    inputCompleteness, inputTruncated, inputUnknown,
                    abstractIndexVersion, queryWrites, currentGeneration,
                    transformState, transformInputComplete,
                    transformOutputPublished, transformGeneration,
                    requiredFactSet, outputFactSet, transformConsumed,
                    transformLifecycle, transformReused, seeded>>

ReturnResult ==
    /\ queryExecuted
    /\ ~resultReturned
    /\ resultReturned' = TRUE
    /\ resultStatus' = "complete"
    /\ trace' = Append(trace, "ReturnResult")
    /\ UNCHANGED <<candidatePlan, candidateSteps, planValidated, planRejected,
                    queryExecuted, resultItems, resultOrder, resultRelations,
                    witnessPath, inputCompleteness,
                    inputTruncated, inputUnknown, abstractIndexVersion,
                    queryWrites, currentGeneration, transformState,
                    transformInputComplete, transformOutputPublished,
                    transformGeneration, requiredFactSet, outputFactSet,
                    transformConsumed, transformLifecycle, transformReused,
                    seeded>>

ConsumeTransform ==
    /\ transformState = "current"
    /\ transformOutputPublished
    /\ ~transformConsumed
    /\ transformConsumed' = TRUE
    /\ trace' = Append(trace, "ConsumeTransform")
    /\ UNCHANGED <<candidatePlan, candidateSteps, planValidated, planRejected,
                    queryExecuted, resultReturned, resultStatus, resultItems,
                    resultOrder, resultRelations, witnessPath,
                    inputCompleteness, inputTruncated,
                    inputUnknown, abstractIndexVersion, queryWrites,
                    currentGeneration, transformState, transformInputComplete,
                    transformOutputPublished, transformGeneration,
                    requiredFactSet, outputFactSet, transformLifecycle,
                    transformReused, seeded>>

PlanTransform ==
    /\ transformLifecycle = "published"
    /\ ~transformReused
    /\ transformLifecycle' = "planned"
    /\ trace' = Append(trace, "PlanTransform")
    /\ UNCHANGED <<candidatePlan, candidateSteps, planValidated, planRejected,
                    queryExecuted, resultReturned, resultStatus, resultItems,
                    resultOrder, resultRelations, witnessPath,
                    inputCompleteness, inputTruncated, inputUnknown,
                    abstractIndexVersion, queryWrites, currentGeneration,
                    transformState, transformInputComplete,
                    transformOutputPublished, transformGeneration,
                    requiredFactSet, outputFactSet, transformConsumed,
                    transformReused, seeded>>

RunTransform ==
    /\ transformLifecycle = "planned"
    /\ transformLifecycle' = "running"
    /\ trace' = Append(trace, "RunTransform")
    /\ UNCHANGED <<candidatePlan, candidateSteps, planValidated, planRejected,
                    queryExecuted, resultReturned, resultStatus, resultItems,
                    resultOrder, resultRelations, witnessPath,
                    inputCompleteness, inputTruncated, inputUnknown,
                    abstractIndexVersion, queryWrites, currentGeneration,
                    transformState, transformInputComplete,
                    transformOutputPublished, transformGeneration,
                    requiredFactSet, outputFactSet, transformConsumed,
                    transformReused, seeded>>

PublishTransform ==
    /\ transformLifecycle = "running"
    /\ transformInputComplete
    /\ transformLifecycle' = "published"
    /\ transformState' = "current"
    /\ transformOutputPublished' = TRUE
    /\ transformGeneration' = currentGeneration
    /\ requiredFactSet' = outputFactSet
    /\ transformConsumed' = FALSE
    /\ transformReused' = FALSE
    /\ trace' = Append(trace, "PublishTransform")
    /\ UNCHANGED <<candidatePlan, candidateSteps, planValidated, planRejected,
                    queryExecuted, resultReturned, resultStatus, resultItems,
                    resultOrder, resultRelations, witnessPath,
                    inputCompleteness, inputTruncated, inputUnknown,
                    abstractIndexVersion, queryWrites, currentGeneration,
                    transformInputComplete, transformLifecycle, seeded>>

FailTransform ==
    /\ transformLifecycle = "running"
    /\ transformLifecycle' = "failed"
    /\ transformState' = "failed"
    /\ transformOutputPublished' = FALSE
    /\ transformConsumed' = FALSE
    /\ transformReused' = FALSE
    /\ trace' = Append(trace, "FailTransform")
    /\ UNCHANGED <<candidatePlan, candidateSteps, planValidated, planRejected,
                    queryExecuted, resultReturned, resultStatus, resultItems,
                    resultOrder, resultRelations, witnessPath,
                    inputCompleteness, inputTruncated, inputUnknown,
                    abstractIndexVersion, queryWrites, currentGeneration,
                    transformInputComplete, transformGeneration,
                    requiredFactSet, outputFactSet, seeded>>

InvalidateTransform ==
    /\ transformLifecycle = "published"
    /\ ~transformReused
    /\ transformLifecycle' = "stale"
    /\ transformState' = "stale"
    /\ transformOutputPublished' = FALSE
    /\ transformConsumed' = FALSE
    /\ trace' = Append(trace, "InvalidateTransform")
    /\ UNCHANGED <<candidatePlan, candidateSteps, planValidated, planRejected,
                    queryExecuted, resultReturned, resultStatus, resultItems,
                    resultOrder, resultRelations, witnessPath,
                    inputCompleteness, inputTruncated, inputUnknown,
                    abstractIndexVersion, queryWrites, currentGeneration,
                    transformInputComplete, transformGeneration,
                    requiredFactSet, outputFactSet, transformReused, seeded>>

ReuseTransform ==
    /\ transformLifecycle = "published"
    /\ ~transformReused
    /\ transformReused' = TRUE
    /\ transformConsumed' = TRUE
    /\ trace' = Append(trace, "ReuseTransform")
    /\ UNCHANGED <<candidatePlan, candidateSteps, planValidated, planRejected,
                    queryExecuted, resultReturned, resultStatus, resultItems,
                    resultOrder, resultRelations, witnessPath,
                    inputCompleteness, inputTruncated, inputUnknown,
                    abstractIndexVersion, queryWrites, currentGeneration,
                    transformState, transformInputComplete,
                    transformOutputPublished, transformGeneration,
                    requiredFactSet, outputFactSet, transformLifecycle,
                    seeded>>

SeedDefect ==
    /\ Defect \in Defects \ {"none"}
    /\ ~seeded
    /\ seeded' = TRUE
    /\ candidatePlan' =
        IF Defect = "illegal-stream"
        THEN [candidatePlan EXCEPT !.stream = "path", !.view = "relations"]
        ELSE candidatePlan
    /\ candidateSteps' =
        IF Defect = "illegal-source"
        THEN [candidateSteps EXCEPT ![1].outputView = "relations"]
        ELSE IF Defect = "illegal-filter"
        THEN [candidateSteps EXCEPT ![2].outputView = "paths",
                                      ![2].outputStream = "path"]
        ELSE IF Defect = "illegal-traverse"
        THEN [candidateSteps EXCEPT ![3].operation = "traverse"]
        ELSE IF Defect = "illegal-set"
        THEN [candidateSteps EXCEPT ![3].leftView = "relations"]
        ELSE IF Defect = "illegal-select"
        THEN [candidateSteps EXCEPT ![4].selection = "relation"]
        ELSE IF Defect = "illegal-order"
        THEN [candidateSteps EXCEPT ![5].outputView = "paths",
                                      ![5].outputStream = "path"]
        ELSE IF Defect = "illegal-limit"
        THEN [candidateSteps EXCEPT ![6].outputView = "paths",
                                      ![6].outputStream = "path"]
        ELSE candidateSteps
    /\ planValidated' = IF Defect \in
        {"illegal-stream", "illegal-source", "illegal-filter", "illegal-traverse", "illegal-set",
         "illegal-select", "illegal-order", "illegal-limit", "query-write"}
                         THEN TRUE
                         ELSE planValidated
    /\ resultReturned' = IF Defect = "invalid-witness"
                         THEN TRUE
                         ELSE resultReturned
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
    /\ inputTruncated' = IF Defect = "complete-truncated"
                         THEN TRUE
                         ELSE inputTruncated
    /\ inputUnknown' = IF Defect = "complete-unknown"
                       THEN TRUE
                       ELSE inputUnknown
    /\ resultRelations' = IF Defect = "complete-unknown"
                          THEN {"rel-3"}
                          ELSE resultRelations
    /\ queryExecuted' = IF Defect = "query-write"
                        THEN TRUE
                        ELSE queryExecuted
    /\ queryWrites' = IF Defect = "query-write" THEN 1 ELSE queryWrites
    /\ abstractIndexVersion' = IF Defect = "query-write"
                               THEN InitialIndexVersion + 1
                               ELSE abstractIndexVersion
    /\ transformState' = IF Defect = "stale-transform" THEN "stale"
                        ELSE IF Defect = "stale-fact-consumption" THEN "current"
                        ELSE IF Defect = "failed-transform" THEN "failed"
                        ELSE transformState
    /\ transformInputComplete' = IF Defect = "partial-transform" THEN FALSE
                                 ELSE transformInputComplete
    /\ transformOutputPublished' = IF Defect = "stale-fact-consumption"
                                   THEN TRUE
                                   ELSE IF Defect \in {"stale-transform",
                                                        "failed-transform"}
                                   THEN FALSE
                                   ELSE transformOutputPublished
    /\ transformConsumed' = IF Defect \in
        {"stale-fact-consumption", "failed-transform", "partial-transform"}
                            THEN TRUE
                            ELSE transformConsumed
    /\ requiredFactSet' = IF Defect = "stale-fact-consumption"
                          THEN "stale-facts-v0"
                          ELSE requiredFactSet
    /\ transformLifecycle' = IF Defect = "stale-transform" THEN "stale"
                             ELSE IF Defect = "failed-transform" THEN "failed"
                             ELSE transformLifecycle
    /\ transformReused' = transformReused
    /\ trace' = Append(trace, Defect)
    /\ UNCHANGED <<planRejected, resultItems, currentGeneration,
                    transformGeneration, outputFactSet>>

NoOp == UNCHANGED SemanticVars

Next == ValidatePlan \/ ExecuteQuery \/ ReturnResult \/ ConsumeTransform
    \/ PlanTransform \/ RunTransform \/ PublishTransform \/ FailTransform
    \/ InvalidateTransform \/ ReuseTransform \/ SeedDefect \/ NoOp

Fairness ==
    /\ WF_vars(ValidatePlan)
    /\ WF_vars(ExecuteQuery)
    /\ WF_vars(ReturnResult)
    /\ WF_vars(ConsumeTransform)
    /\ WF_vars(PlanTransform)
    /\ WF_vars(RunTransform)
    /\ WF_vars(PublishTransform)
    /\ WF_vars(FailTransform)
    /\ WF_vars(InvalidateTransform)
    /\ WF_vars(ReuseTransform)
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
