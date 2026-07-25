--------------------------- MODULE CidxSemanticGraph --------------------------

EXTENDS CidxTypes, FiniteSets, Naturals, Sequences

(***************************************************************************)
(* M2 semantic contract.  This is a finite executable contract for graph  *)
(* identity, query-plan shape transitions, evidence/completeness transfer, *)
(* named transform dependencies, and adversarial query behavior.            *)
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
PlanOperations == {"identity", "view", "filter", "traverse", "union",
                   "intersection", "difference", "select"}
PlanSelections == {"node", "relation", "evidence", "path"}
PlanOrders == {"canonical", "none"}
CompletenessStates == {"complete", "partial", "unknown"}
ResultStates == {"none", "complete", "partial", "unknown"}
DerivedTransformStates == {"absent", "planned", "current", "stale", "failed"}
PlanStepOperations == {"source", "view", "filter", "traverse", "union",
                       "intersection", "difference", "select", "order", "limit"}
StageNames == {"source", "filter", "view", "traverse", "evidence", "limit"}
OperandNames == {"left", "right"}
TransformLifecycleStates == {"planned", "running", "published", "stale", "failed"}
TransformNames == {"parse", "resolve", "answer", "summary"}
FactSets == {"raw-facts", "parsed-facts-v1", "resolved-facts-v1",
             "answer-facts-v1", "summary-facts-v1"}
Fixtures == {"normal", "cycle", "diamond", "fanout", "unknown-target",
             "partial-evidence"}
ResultItemIds == GraphNodeIds \cup RelationIds \cup EvidenceIds
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
TransformDependencyEdges == {<<"resolve", "parse">>,
                             <<"answer", "resolve">>,
                             <<"summary", "resolve">>}
TransformRequiredFacts == {<<"parse", "raw-facts">>,
                           <<"resolve", "parsed-facts-v1">>,
                           <<"answer", "resolved-facts-v1">>,
                           <<"summary", "resolved-facts-v1">>}
TransformProducedFacts == {<<"parse", "parsed-facts-v1">>,
                           <<"resolve", "resolved-facts-v1">>,
                           <<"answer", "answer-facts-v1">>,
                           <<"summary", "summary-facts-v1">>}
Defects == {"none", "illegal-source", "illegal-view", "illegal-filter",
            "illegal-traverse", "illegal-set", "illegal-select", "illegal-order",
            "illegal-limit", "duplicate-results", "query-write",
            "witness-below-bound", "witness-above-bound", "witness-missing-edge",
            "partial-left", "partial-right", "partial-filter", "partial-view",
            "partial-traverse", "partial-evidence", "partial-transform",
            "truncated-limit", "unknown-target-complete", "partial-evidence-complete",
            "cycle-loop", "diamond-duplicate", "fanout-dedup",
            "fanout-complete", "fanout-truncated", "stale-resolve", "failed-resolve",
            "stale-answer-consumption", "failed-answer-publication",
            "stale-summary", "failed-summary", "illegal-later-source",
            "illegal-terminal", "illegal-plan-operation"}

SetUnionSteps ==
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

SetIntersectionSteps == [SetUnionSteps EXCEPT ![3].operation = "intersection"]
SetDifferenceSteps == [SetUnionSteps EXCEPT ![3].operation = "difference"]

ViewNodesSteps ==
    <<SetUnionSteps[1],
      [operation |-> "view", inputView |-> "nodes", inputStream |-> "set",
       outputView |-> "nodes", outputStream |-> "set", selection |-> "node",
       leftView |-> "nodes", rightView |-> "nodes", depth |-> 0, limit |-> 10],
      SetUnionSteps[4], SetUnionSteps[5], SetUnionSteps[6]>>

ViewRelationsSteps ==
    <<SetUnionSteps[1],
      [operation |-> "view", inputView |-> "nodes", inputStream |-> "set",
       outputView |-> "relations", outputStream |-> "set", selection |-> "relation",
       leftView |-> "relations", rightView |-> "relations", depth |-> 0, limit |-> 10],
      [operation |-> "select", inputView |-> "relations", inputStream |-> "set",
       outputView |-> "relations", outputStream |-> "set", selection |-> "relation",
       leftView |-> "relations", rightView |-> "relations", depth |-> 0, limit |-> 10],
      [operation |-> "order", inputView |-> "relations", inputStream |-> "set",
       outputView |-> "relations", outputStream |-> "set", selection |-> "relation",
       leftView |-> "relations", rightView |-> "relations", depth |-> 0, limit |-> 10],
      [operation |-> "limit", inputView |-> "relations", inputStream |-> "set",
       outputView |-> "relations", outputStream |-> "set", selection |-> "relation",
       leftView |-> "relations", rightView |-> "relations", depth |-> 0, limit |-> 10]>>

ViewEvidenceSteps ==
    <<SetUnionSteps[1],
      [operation |-> "view", inputView |-> "nodes", inputStream |-> "set",
       outputView |-> "evidence", outputStream |-> "set", selection |-> "evidence",
       leftView |-> "evidence", rightView |-> "evidence", depth |-> 0, limit |-> 10],
      [operation |-> "select", inputView |-> "evidence", inputStream |-> "set",
       outputView |-> "evidence", outputStream |-> "set", selection |-> "evidence",
       leftView |-> "evidence", rightView |-> "evidence", depth |-> 0, limit |-> 10],
      [operation |-> "order", inputView |-> "evidence", inputStream |-> "set",
       outputView |-> "evidence", outputStream |-> "set", selection |-> "evidence",
       leftView |-> "evidence", rightView |-> "evidence", depth |-> 0, limit |-> 10],
      [operation |-> "limit", inputView |-> "evidence", inputStream |-> "set",
       outputView |-> "evidence", outputStream |-> "set", selection |-> "evidence",
       leftView |-> "evidence", rightView |-> "evidence", depth |-> 0, limit |-> 10]>>

PathDepth1Steps ==
    <<SetUnionSteps[1],
      [operation |-> "traverse", inputView |-> "nodes", inputStream |-> "set",
       outputView |-> "paths", outputStream |-> "path", selection |-> "path",
       leftView |-> "nodes", rightView |-> "nodes", depth |-> 1, limit |-> 10],
      [operation |-> "select", inputView |-> "paths", inputStream |-> "path",
       outputView |-> "paths", outputStream |-> "path", selection |-> "path",
       leftView |-> "paths", rightView |-> "paths", depth |-> 1, limit |-> 10]>>

PathDepth2Steps == [PathDepth1Steps EXCEPT ![2].depth = 2, ![3].depth = 2]

PlanFor(name, view, stream, operation, selection, depth, steps) ==
    [name |-> name, source |-> "graph", view |-> view, filter |-> "none",
     stream |-> stream, operation |-> operation, selection |-> selection,
     order |-> "canonical", depth |-> depth, limit |-> 10,
     transform |-> "answer", pathStart |-> PathStart, pathTarget |-> PathTarget,
     steps |-> steps]

SetUnionOption == PlanFor("set-union", "nodes", "set", "union", "node", 0,
                          SetUnionSteps)
SetIntersectionOption == PlanFor("set-intersection", "nodes", "set",
                                  "intersection", "node", 0, SetIntersectionSteps)
SetDifferenceOption == PlanFor("set-difference", "nodes", "set", "difference",
                               "node", 0, SetDifferenceSteps)
ViewNodesOption == PlanFor("view-nodes", "nodes", "set", "view", "node", 0,
                           ViewNodesSteps)
ViewRelationsOption == PlanFor("view-relations", "relations", "set", "view",
                               "relation", 0, ViewRelationsSteps)
ViewEvidenceOption == PlanFor("view-evidence", "evidence", "set", "view",
                              "evidence", 0, ViewEvidenceSteps)
PathDepth1Option == PlanFor("path-depth-1", "paths", "path", "traverse", "path",
                            1, PathDepth1Steps)
PathDepth2Option == PlanFor("path-depth-2", "paths", "path", "traverse", "path",
                            2, PathDepth2Steps)
PlanOptions == {SetUnionOption, SetIntersectionOption, SetDifferenceOption,
                ViewNodesOption, ViewRelationsOption, ViewEvidenceOption,
                PathDepth1Option, PathDepth2Option}

GraphNodes == {
    <<"node-1", "function">>, <<"node-2", "function">>,
    <<"node-3", "function">>, <<"record-1", "record">>,
    <<"unknown-target", "unknown">>
}

GraphRelations == {
    <<"rel-1", "calls", "node-1", "node-2", "known", {"evidence-1"}, <<1, 2>>>>,
    <<"rel-2", "calls", "node-2", "node-3", "known", {"evidence-2"}, <<1>>>>,
    <<"rel-3", "inherits", "record-1", "unknown-target", "unknown",
      {"evidence-3"}, <<>>>>,
    <<"rel-4", "calls", "node-1", "node-3", "known", {"evidence-4"}, <<1>>>>
}

GraphEvidence == {
    <<"evidence-1", "rel-1", "source", TRUE>>,
    <<"evidence-2", "rel-2", "source", TRUE>>,
    <<"evidence-3", "rel-3", "model", FALSE>>,
    <<"evidence-4", "rel-4", "model", FALSE>>
}

CyclePath == <<"node-1", "node-2", "node-1">>
CycleEdges == {<<"node-1", "node-2">>, <<"node-2", "node-1">>}
DiamondEdges == {<<"node-1", "node-2">>, <<"node-1", "node-3">>,
                 <<"node-2", "node-3">>, <<"node-3", "node-2">>}
FanoutTargets == {"node-1", "node-2", "node-3"}
IncompleteTargetIds == {"unknown-target"}
PartialEvidenceIds == {"evidence-3", "evidence-4"}
CanonicalResultOrder == <<"node-1", "node-2", "node-3">>

CanonicalNode(nodes) ==
    IF "node-1" \in nodes THEN "node-1"
    ELSE IF "node-2" \in nodes THEN "node-2"
    ELSE IF "node-3" \in nodes THEN "node-3"
    ELSE CHOOSE n \in nodes : TRUE

NeighborSet(f, node) ==
    IF f = "cycle" /\ node = "node-1" THEN {"node-2"}
    ELSE IF f = "cycle" /\ node = "node-2" THEN {"node-1"}
    ELSE IF f = "diamond" /\ node = "node-1"
    THEN {"node-2", "node-3"}
    ELSE IF f = "diamond" /\ node = "node-2" THEN {"node-3"}
    ELSE IF f = "diamond" /\ node = "node-3" THEN {"node-2"}
    ELSE IF f = "fanout" /\ node = "node-1"
    THEN {"node-2", "node-3"}
    ELSE {}

RECURSIVE BoundedWalk(_, _, _, _, _)
BoundedWalk(f, pending, visited, order, limit) ==
    IF pending = {} /\ Len(order) < limit /\ Len(order) < AdversarialBound
    THEN [visited |-> visited, order |-> order, truncated |-> FALSE]
    ELSE IF pending = {}
    THEN [visited |-> visited, order |-> order, truncated |-> FALSE]
    ELSE IF Len(order) >= limit \/ Len(order) >= AdversarialBound
    THEN [visited |-> visited, order |-> order, truncated |-> TRUE]
    ELSE LET node == CanonicalNode(pending)
             nextPending == (pending \ {node})
                              \cup (NeighborSet(f, node) \ visited)
         IN BoundedWalk(f, nextPending, visited \cup {node},
                        Append(order, node), limit)

ExpectedTraversal(f, limit) == BoundedWalk(f, {PathStart}, {}, <<>>, limit)

VARIABLES
    candidatePlan, candidateSteps, leftOperand, rightOperand, fixture,
    planValidated, planRejected, queryExecuted, resultReturned, resultStatus,
    resultItems, resultOrder, resultRelations, witnessPath,
    sourceCompleteness, filterCompleteness, viewCompleteness,
    leftCompleteness, rightCompleteness, traverseCompleteness,
    evidenceCompleteness, limitCompleteness,
    abstractIndexVersion, queryWrites, currentGeneration,
    transformState, transformInputComplete, transformOutputPublished,
    transformGeneration, transformConsumed, transformLifecycle, transformReused,
    factPublished, factGeneration, factCompleteness, stageCompleteness,
    operandCompleteness, stepResults, stepCompleteness, stepFacts,
    queryStarted, frontier, traversalVisited, traversalOrder, traversalTruncated,
    seeded, trace

SemanticVars == <<candidatePlan, candidateSteps, leftOperand, rightOperand, fixture,
    planValidated, planRejected, queryExecuted, resultReturned, resultStatus,
    resultItems, resultOrder, resultRelations, witnessPath,
    sourceCompleteness, filterCompleteness, viewCompleteness, leftCompleteness,
    rightCompleteness, traverseCompleteness, evidenceCompleteness,
    limitCompleteness, abstractIndexVersion, queryWrites, currentGeneration,
    transformState, transformInputComplete, transformOutputPublished,
    transformGeneration, transformConsumed, transformLifecycle, transformReused,
    factPublished, factGeneration, factCompleteness, stageCompleteness,
    operandCompleteness, stepResults, stepCompleteness, stepFacts, queryStarted,
    frontier, traversalVisited, traversalOrder, traversalTruncated,
    factGeneration, seeded, trace>>
vars == SemanticVars

SequenceValues(s) == {s[i] : i \in 1..Len(s)}
DistinctSequence(s) == Len(s) = Cardinality(SequenceValues(s))
JoinCompleteness(a, b) ==
    IF a = "unknown" \/ b = "unknown" THEN "unknown"
    ELSE IF a = "partial" \/ b = "partial" THEN "partial"
    ELSE "complete"
NodeKind(id) == CHOOSE n \in GraphNodes : n[1] = id

ValidNode(n) == /\ n[1] \in GraphNodeIds /\ n[2] \in GraphNodeKinds
ValidGraphRelation(r) ==
    /\ r[1] \in RelationIds /\ r[2] \in RelationKinds
    /\ r[3] \in GraphNodeIds /\ r[4] \in GraphNodeIds
    /\ r[5] \in RelationTargetStates /\ r[6] \subseteq EvidenceIds
    /\ r[7] \in Seq(Nat)

EndpointCompatible(r) ==
    \/ r[2] = "calls" /\ NodeKind(r[3])[2] \in {"function", "method"}
                       /\ NodeKind(r[4])[2] \in {"function", "method"}
    \/ r[2] = "inherits" /\ NodeKind(r[3])[2] = "record"
                         /\ NodeKind(r[4])[2] \in {"record", "unknown"}

GraphNodeInvariant ==
    /\ GraphNodes # {} /\ \A n \in GraphNodes : ValidNode(n)
    /\ Cardinality({n[1] : n \in GraphNodes}) = Cardinality(GraphNodes)

GraphRelationInvariant ==
    /\ GraphRelations # {}
    /\ \A r \in GraphRelations :
        /\ ValidGraphRelation(r) /\ EndpointCompatible(r)
        /\ (r[5] = "unknown" => NodeKind(r[4])[2] = "unknown")
        /\ (r[5] = "known" => NodeKind(r[4])[2] # "unknown")
        /\ DistinctSequence(r[7])
    /\ Cardinality({r[1] : r \in GraphRelations}) = Cardinality(GraphRelations)

EvidenceOwnershipInvariant ==
    /\ \A e \in GraphEvidence :
        /\ e[1] \in EvidenceIds /\ e[2] \in RelationIds
        /\ e[3] \in EvidenceKinds /\ e[4] \in BOOLEAN
        /\ \E r \in GraphRelations : r[1] = e[2] /\ e[1] \in r[6]
    /\ \A r \in GraphRelations :
        \A e \in GraphEvidence : e[1] \in r[6] => e[2] = r[1]

UnknownTargetInvariant ==
    \A r \in GraphRelations : r[5] = "unknown" =>
        /\ NodeKind(r[4])[2] = "unknown" /\ r[4] \in GraphNodeIds

EdgeEndpointsValid(edges) ==
    \A e \in edges : e[1] \in GraphNodeIds /\ e[2] \in GraphNodeIds

CycleAdversarialInvariant ==
    /\ CyclePath \in Seq(GraphNodeIds) /\ Len(CyclePath) - 1 <= AdversarialBound
    /\ CyclePath[1] = CyclePath[Len(CyclePath)]
    /\ \A i \in 1..(Len(CyclePath) - 1) : <<CyclePath[i], CyclePath[i + 1]>> \in CycleEdges

DiamondAdversarialInvariant ==
    /\ EdgeEndpointsValid(DiamondEdges) /\ Cardinality(DiamondEdges) = 4
    /\ DiamondEdges \subseteq (GraphNodeIds \X GraphNodeIds)
    /\ Cardinality(DiamondEdges) <= AdversarialBound

FanoutAdversarialInvariant ==
    /\ FanoutTargets \subseteq GraphNodeIds /\ FanoutTargets # {}
    /\ Cardinality(FanoutTargets) <= AdversarialBound

IncompleteTargetAdversarialInvariant ==
    /\ IncompleteTargetIds \subseteq GraphNodeIds
    /\ \E r \in GraphRelations : r[4] \in IncompleteTargetIds

PartialEvidenceAdversarialInvariant ==
    /\ PartialEvidenceIds \subseteq EvidenceIds /\ PartialEvidenceIds # {}
    /\ \E e \in GraphEvidence : e[1] \in PartialEvidenceIds /\ ~e[4]

SemanticGraphInvariant ==
    /\ GraphNodeInvariant /\ GraphRelationInvariant /\ EvidenceOwnershipInvariant
    /\ UnknownTargetInvariant /\ CycleAdversarialInvariant
    /\ DiamondAdversarialInvariant /\ FanoutAdversarialInvariant
    /\ IncompleteTargetAdversarialInvariant /\ PartialEvidenceAdversarialInvariant

PlanStepShapeValid(step) ==
    /\ step.operation \in PlanStepOperations /\ step.inputView \in PlanViews
    /\ step.inputStream \in PlanStreams /\ step.outputView \in PlanViews
    /\ step.outputStream \in PlanStreams /\ step.selection \in PlanSelections
    /\ step.leftView \in PlanViews /\ step.rightView \in PlanViews
    /\ step.depth \in Nat /\ step.depth <= MaxDepth
    /\ step.limit \in Nat /\ step.limit > 0

FirstSourceValid(step) ==
    /\ step.operation = "source" /\ step.inputView = "graph"
    /\ step.inputStream = "set" /\ step.outputView = "nodes"
    /\ step.outputStream = "set"

PlanStepTransitionValid(previous, step) ==
    /\ step.inputView = previous.outputView /\ step.inputStream = previous.outputStream
    /\ step.operation # "source"
    /\ (step.operation = "view" =>
        /\ step.inputView = "nodes" /\ step.inputStream = "set"
        /\ step.outputView \in {"nodes", "relations", "evidence"}
        /\ step.outputStream = "set")
    /\ (step.operation = "filter" =>
        /\ step.inputView \in {"nodes", "relations", "evidence"}
        /\ step.inputStream = "set" /\ step.outputView = step.inputView
        /\ step.outputStream = "set")
    /\ (step.operation = "traverse" =>
        /\ step.inputView \in {"nodes", "relations"} /\ step.inputStream = "set"
        /\ step.outputView = "paths" /\ step.outputStream = "path"
        /\ step.selection = "path" /\ step.depth > 0)
    /\ (step.operation \in {"union", "intersection", "difference"} =>
        /\ step.inputView \in {"nodes", "relations", "evidence"}
        /\ step.inputStream = "set" /\ step.leftView = step.inputView
        /\ step.rightView = step.inputView /\ step.outputView = step.inputView
        /\ step.outputStream = "set")
    /\ (step.operation = "select" /\ step.selection = "node" =>
        /\ step.inputView = "nodes" /\ step.inputStream = "set"
        /\ step.outputView = "nodes" /\ step.outputStream = "set")
    /\ (step.operation = "select" /\ step.selection = "relation" =>
        /\ step.inputView = "relations" /\ step.inputStream = "set"
        /\ step.outputView = "relations" /\ step.outputStream = "set")
    /\ (step.operation = "select" /\ step.selection = "evidence" =>
        /\ step.inputView = "evidence" /\ step.inputStream = "set"
        /\ step.outputView = "evidence" /\ step.outputStream = "set")
    /\ (step.operation = "select" /\ step.selection = "path" =>
        /\ step.inputView = "paths" /\ step.inputStream = "path"
        /\ step.outputView = "paths" /\ step.outputStream = "path")
    /\ (step.operation \in {"order", "limit"} =>
        /\ step.outputView = step.inputView /\ step.outputStream = step.inputStream)

PlanStepsValid(steps) ==
    /\ steps \in Seq(PlanStepRecord) /\ Len(steps) > 0
    /\ PlanStepShapeValid(steps[1]) /\ FirstSourceValid(steps[1])
    /\ \A i \in 2..Len(steps) :
        /\ PlanStepShapeValid(steps[i])
        /\ PlanStepTransitionValid(steps[i - 1], steps[i])

PlanOperationRepresented(p, steps) ==
    \E i \in 1..Len(steps) : steps[i].operation = p.operation

PlanTerminalValid(p, steps) ==
    LET terminal == steps[Len(steps)] IN
        /\ terminal.outputView = p.view
        /\ terminal.outputStream = p.stream
        /\ terminal.selection = p.selection
        /\ PlanOperationRepresented(p, steps)

PlanValid(p) ==
    /\ p.source \in PlanSources /\ p.view \in PlanViews
    /\ p.filter \in PlanFilters /\ p.stream \in PlanStreams
    /\ p.operation \in PlanOperations /\ p.selection \in PlanSelections
    /\ p.order \in PlanOrders /\ p.depth \in Nat /\ p.depth <= MaxDepth
    /\ p.limit \in Nat /\ p.limit > 0 /\ p.transform \in TransformNames
    /\ p.pathStart \in GraphNodeIds /\ p.pathTarget \in GraphNodeIds
    /\ (p.view = "paths" =>
        /\ p.stream = "path" /\ p.operation = "traverse"
        /\ p.selection = "path" /\ p.depth > 0)
    /\ (p.stream = "path" => p.view = "paths")
    /\ (p.operation \in {"union", "intersection", "difference"} =>
        /\ p.stream = "set" /\ p.view \in {"nodes", "relations", "evidence"})
    /\ (p.operation = "view" => p.stream = "set" /\ p.view # "graph")
    /\ (p.order = "none" => p.stream = "path")

PlanTransitionInvariant ==
    /\ planValidated => /\ PlanValid(candidatePlan)
                          /\ PlanStepsValid(candidateSteps)
                          /\ PlanTerminalValid(candidatePlan, candidateSteps)
                          /\ candidatePlan.steps = candidateSteps
    /\ planRejected => ~planValidated /\ (queryExecuted => planValidated)

SetOperationResult(op, left, right) ==
    IF op = "union" THEN left \cup right
    ELSE IF op = "intersection" THEN left \cap right
    ELSE left \ right

CanonicalOrder(items) ==
    IF items = {"node-1", "node-2", "node-3"} THEN <<"node-1", "node-2", "node-3">>
    ELSE IF items = {"node-1", "node-2"} THEN <<"node-1", "node-2">>
    ELSE IF items = {"node-1", "node-3"} THEN <<"node-1", "node-3">>
    ELSE IF items = {"node-2", "node-3"} THEN <<"node-2", "node-3">>
    ELSE IF items = {"record-1", "unknown-target"} THEN <<"record-1", "unknown-target">>
    ELSE IF items = {"rel-1", "rel-2"} THEN <<"rel-1", "rel-2">>
    ELSE IF items = {"evidence-1", "evidence-2"} THEN <<"evidence-1", "evidence-2">>
    ELSE IF items = {"node-2"} THEN <<"node-2">>
    ELSE IF items = {"node-1"} THEN <<"node-1">>
    ELSE <<>>

FixtureRelations(f) ==
    IF f = "unknown-target" THEN {"rel-3"}
    ELSE IF f = "partial-evidence" THEN {"rel-4"}
    ELSE IF candidatePlan.view = "evidence" THEN {"rel-1", "rel-2"}
    ELSE IF candidatePlan.view = "relations" THEN {"rel-1", "rel-2"}
    ELSE IF f = "cycle" THEN {"rel-1"}
    ELSE {"rel-1", "rel-2"}

FixtureWitness(f) ==
    IF f = "cycle" THEN CyclePath
    ELSE IF candidatePlan.depth = 1 THEN <<PathStart, PathTarget>>
    ELSE <<PathStart, "node-2", PathTarget>>

ViewItems(view, f) ==
    IF view = "relations"
    THEN IF f = "unknown-target" THEN {"rel-3"} ELSE {"rel-1", "rel-2"}
    ELSE IF view = "evidence"
    THEN IF f = "partial-evidence" THEN {"evidence-4"}
         ELSE {"evidence-1", "evidence-2"}
    ELSE IF view = "nodes" THEN LeftSet \cup RightSet
    ELSE {}

StepResult(step, input, f, left, right) ==
    IF step.operation = "source" THEN left \cup right
    ELSE IF step.operation = "view" THEN ViewItems(step.outputView, f)
    ELSE IF step.operation = "filter" THEN input
    ELSE IF step.operation = "traverse"
    THEN IF f = "normal"
         THEN IF step.depth = 1
              THEN {PathStart, PathTarget}
              ELSE {PathStart, "node-2", PathTarget}
         ELSE ExpectedTraversal(f, step.limit).visited
    ELSE IF step.operation \in {"union", "intersection", "difference"}
    THEN SetOperationResult(step.operation, left, right)
    ELSE input

StepFactsFor(step) ==
    IF step.operation = "source" THEN {"raw-facts"}
    ELSE IF step.operation \in {"view", "traverse"}
    THEN {"resolved-facts-v1"}
    ELSE {"answer-facts-v1"}

RECURSIVE EvaluateStepResults(_, _, _, _, _, _)
EvaluateStepResults(steps, i, input, f, left, right) ==
    IF i > Len(steps)
    THEN <<>>
    ELSE LET output == StepResult(steps[i], input, f, left, right)
         IN <<output>> \o EvaluateStepResults(steps, i + 1, output,
                                                  f, left, right)

FactSetCompleteness(facts) ==
    IF \E f \in facts : factCompleteness[f] = "unknown" THEN "unknown"
    ELSE IF \E f \in facts : factCompleteness[f] = "partial" THEN "partial"
    ELSE "complete"

StepOwnCompleteness(step, input, f, left, right) ==
    JoinCompleteness(
        JoinCompleteness(stageCompleteness[IF step.operation = "source"
                                           THEN "source"
                                           ELSE IF step.operation = "filter"
                                           THEN "filter"
                                           ELSE IF step.operation = "view"
                                           THEN "view"
                                           ELSE IF step.operation = "traverse"
                                           THEN "traverse"
                                           ELSE IF step.operation = "limit"
                                           THEN "limit"
                                           ELSE "source"],
                         FactSetCompleteness(StepFactsFor(step))),
        IF step.operation \in {"union", "intersection", "difference"}
        THEN JoinCompleteness(operandCompleteness["left"],
                              operandCompleteness["right"])
        ELSE IF step.operation = "view" /\ step.outputView = "evidence"
        THEN stageCompleteness["evidence"]
        ELSE IF step.operation = "traverse" /\ f = "unknown-target"
        THEN "unknown"
        ELSE IF step.operation = "traverse" /\ f = "partial-evidence"
        THEN "partial"
        ELSE IF step.operation = "traverse" /\ traversalTruncated
        THEN "partial"
        ELSE IF step.operation = "limit" /\ Cardinality(input) > step.limit
        THEN "partial"
        ELSE "complete")

RECURSIVE EvaluateStepCompleteness(_, _, _, _, _, _, _, _)
EvaluateStepCompleteness(steps, i, input, inputCompleteness, f, left, right,
                         unused) ==
    IF i > Len(steps)
    THEN <<>>
    ELSE LET output == StepResult(steps[i], input, f, left, right)
             completeness == StepOwnCompleteness(steps[i], input, f, left, right)
         IN <<JoinCompleteness(inputCompleteness, completeness)>>
            \o EvaluateStepCompleteness(steps, i + 1, output, completeness,
                                          f, left, right, unused)

EvaluateStepFacts(steps) == [i \in 1..Len(steps) |-> StepFactsFor(steps[i])]

SetSemanticsInvariant ==
    IF queryExecuted /\ fixture = "normal"
    THEN IF candidatePlan.operation = "union"
         \/ candidatePlan.operation = "intersection"
         \/ candidatePlan.operation = "difference"
         THEN /\ resultItems = SetOperationResult(candidatePlan.operation, leftOperand, rightOperand)
              /\ resultOrder = CanonicalOrder(resultItems)
              /\ SequenceValues(resultOrder) = resultItems /\ DistinctSequence(resultOrder)
         ELSE TRUE
    ELSE TRUE

QueryResultInvariant ==
    IF ~seeded /\ queryExecuted
    THEN /\ resultItems = IF fixture \in {"cycle", "diamond", "fanout"}
                          THEN traversalVisited
                          ELSE stepResults[Len(stepResults)]
         /\ resultRelations = FixtureRelations(fixture)
         /\ resultOrder = IF fixture \in {"cycle", "diamond", "fanout"}
                          THEN traversalOrder
                          ELSE CanonicalOrder(resultItems)
         /\ DistinctSequence(resultOrder)
    ELSE TRUE

PathHasEdges(path) ==
    \A i \in 1..(Len(path) - 1) :
        \E r \in GraphRelations : r[3] = path[i] /\ r[4] = path[i + 1]
                              /\ r[5] = "known"

WitnessPathValid(path, start, target, bound) ==
    /\ path \in Seq(GraphNodeIds) /\ Len(path) > 1
    /\ path[1] = start /\ path[Len(path)] = target
    /\ Len(path) - 1 <= bound /\ DistinctSequence(path) /\ PathHasEdges(path)

WitnessInvariant ==
    /\ resultReturned /\ candidatePlan.stream = "path"
    => /\ WitnessPathValid(witnessPath, candidatePlan.pathStart,
                           candidatePlan.pathTarget, candidatePlan.depth)
       /\ candidatePlan.pathStart \in stepResults[Len(stepResults)]
       /\ candidatePlan.pathTarget \in stepResults[Len(stepResults)]

RelationCompleteness(relations) ==
    IF "rel-3" \in relations THEN "unknown"
    ELSE IF "rel-4" \in relations THEN "partial"
    ELSE "complete"

RequiredFacts(t) == {f \in FactSets : <<t, f>> \in TransformRequiredFacts}
ProducedFacts(t) == {f \in FactSets : <<t, f>> \in TransformProducedFacts}
ParentsOf(t) == {p \in TransformNames : <<t, p>> \in TransformDependencyEdges}

RECURSIVE HasDependencyPath(_, _, _)
HasDependencyPath(child, parent, hops) ==
    child = parent
    \/ (hops > 0 /\ \E next \in TransformNames :
            <<child, next>> \in TransformDependencyEdges
            /\ HasDependencyPath(next, parent, hops - 1))

DescendantsOf(t) ==
    {d \in TransformNames : d # t /\
        HasDependencyPath(d, t, Cardinality(TransformNames))}
AffectedTransforms(t) == {t} \cup DescendantsOf(t)

DependenciesFresh(t) ==
    /\ \A f \in RequiredFacts(t) :
        /\ factPublished[f]
        /\ factGeneration[f] = currentGeneration
        /\ factCompleteness[f] = "complete"
    /\ \A p \in ParentsOf(t) :
        /\ transformState[p] = "current"
        /\ transformOutputPublished[p]
        /\ transformGeneration[p] = currentGeneration

FreshTransform(t) ==
    /\ DependenciesFresh(t)
    /\ transformInputComplete[t]

TransformCompleteness ==
    IF ~DependenciesFresh("answer") THEN "unknown"
    ELSE IF ~transformInputComplete["answer"] THEN "partial"
    ELSE FactSetCompleteness(ProducedFacts("answer"))

ComputedResultCompleteness ==
    JoinCompleteness(
        IF Len(stepCompleteness) = 0
        THEN "unknown"
        ELSE stepCompleteness[Len(stepCompleteness)],
        JoinCompleteness(RelationCompleteness(resultRelations),
                         JoinCompleteness(
                             IF Len(stepFacts) = 0
                             THEN "unknown"
                             ELSE FactSetCompleteness(stepFacts[Len(stepFacts)]),
                             JoinCompleteness(stageCompleteness["evidence"],
                                              JoinCompleteness(
                                                  TransformCompleteness,
                                                  IF traversalTruncated
                                                  THEN "partial"
                                                  ELSE "complete")))))

CompletenessInvariant ==
    resultReturned => /\ resultStatus = ComputedResultCompleteness
                      /\ (resultStatus = "complete" =>
                          /\ RelationCompleteness(resultRelations) = "complete"
                          /\ stageCompleteness["evidence"] = "complete"
                          /\ FactSetCompleteness(stepFacts[Len(stepFacts)]) = "complete"
                          /\ TransformCompleteness = "complete")

ReadOnlyExecutionInvariant ==
    /\ queryWrites = 0 /\ (queryExecuted => abstractIndexVersion = InitialIndexVersion)

TransformDependencyInvariant ==
    /\ TransformDependencyEdges = {<<"resolve", "parse">>,
                                   <<"answer", "resolve">>,
                                   <<"summary", "resolve">>}
    /\ \A t \in TransformNames :
        /\ RequiredFacts(t) # {} /\ ProducedFacts(t) # {}
        /\ ParentsOf(t) \subseteq TransformNames

TransformStalePropagationInvariant ==
    \A t \in TransformNames :
        transformState[t] \in {"stale", "failed"}
        => \A d \in DescendantsOf(t) :
            /\ transformState[d] \in {"stale", "failed"}
            /\ ~transformOutputPublished[d]
            /\ ~transformConsumed[d]

TransformPublicationInvariant ==
    \A t \in TransformNames :
        /\ (transformState[t] = "current" =>
            /\ transformInputComplete[t]
            /\ transformOutputPublished[t]
            /\ transformGeneration[t] = currentGeneration
            /\ \A f \in ProducedFacts(t) :
                /\ factPublished[f]
                /\ factGeneration[f] = currentGeneration
                /\ factCompleteness[f] = "complete")
        /\ (transformState[t] \in {"stale", "failed"} =>
            /\ ~transformOutputPublished[t] /\ ~transformConsumed[t])

TransformConsumptionInvariant ==
    \A t \in TransformNames :
        transformConsumed[t] =>
            /\ DependenciesFresh(t)
            /\ transformState[t] = "current"

TransformInvariant ==
    /\ \A t \in TransformNames :
        /\ transformLifecycle[t] \in TransformLifecycleStates
        /\ (transformLifecycle[t] = "published"
            => transformState[t] = "current" /\ transformOutputPublished[t])
        /\ (transformLifecycle[t] = "failed"
            => transformState[t] = "failed" /\ ~transformOutputPublished[t]
                                      /\ ~transformConsumed[t])
        /\ (transformLifecycle[t] = "stale"
            => transformState[t] = "stale" /\ ~transformOutputPublished[t]
                                     /\ ~transformConsumed[t])
    /\ \A f \in FactSets : factPublished[f] \in BOOLEAN

CycleExecutionInvariant ==
    queryExecuted /\ fixture = "cycle" =>
        LET expected == ExpectedTraversal(fixture, candidatePlan.limit) IN
            /\ traversalVisited = expected.visited
            /\ traversalOrder = expected.order
            /\ traversalOrder = resultOrder
            /\ traversalVisited = resultItems
            /\ DistinctSequence(traversalOrder)
            /\ frontier = {}

DiamondExecutionInvariant ==
    queryExecuted /\ fixture = "diamond" =>
        LET expected == ExpectedTraversal(fixture, candidatePlan.limit) IN
            /\ traversalVisited = expected.visited
            /\ traversalOrder = expected.order
            /\ traversalOrder = resultOrder
            /\ traversalVisited = resultItems
            /\ DistinctSequence(traversalOrder)
            /\ frontier = {}

FanoutExecutionInvariant ==
    queryExecuted /\ fixture = "fanout" =>
        LET expected == ExpectedTraversal(fixture, candidatePlan.limit) IN
            /\ traversalVisited = expected.visited
            /\ traversalOrder = expected.order
            /\ traversalTruncated = expected.truncated
            /\ traversalOrder = resultOrder
            /\ traversalVisited = resultItems
            /\ DistinctSequence(traversalOrder)
            /\ frontier = {}

TypeInvariant ==
    /\ candidatePlan \in [name: STRING, source: PlanSources, view: PlanViews, filter: PlanFilters,
        stream: PlanStreams, operation: PlanOperations, selection: PlanSelections,
        order: PlanOrders, depth: Nat, limit: Nat, transform: TransformNames,
        pathStart: GraphNodeIds, pathTarget: GraphNodeIds,
        steps: Seq(PlanStepRecord)]
    /\ candidateSteps \in Seq(PlanStepRecord)
    /\ leftOperand \subseteq GraphNodeIds /\ rightOperand \subseteq GraphNodeIds
    /\ fixture \in Fixtures /\ planValidated \in BOOLEAN /\ planRejected \in BOOLEAN
    /\ queryExecuted \in BOOLEAN /\ resultReturned \in BOOLEAN
    /\ resultStatus \in ResultStates /\ resultItems \subseteq ResultItemIds
    /\ resultOrder \in Seq(ResultItemIds) /\ resultRelations \subseteq RelationIds
    /\ witnessPath \in Seq(GraphNodeIds)
    /\ sourceCompleteness \in CompletenessStates
    /\ filterCompleteness \in CompletenessStates
    /\ viewCompleteness \in CompletenessStates
    /\ leftCompleteness \in CompletenessStates
    /\ rightCompleteness \in CompletenessStates
    /\ traverseCompleteness \in CompletenessStates
    /\ evidenceCompleteness \in CompletenessStates
    /\ limitCompleteness \in CompletenessStates
    /\ abstractIndexVersion \in Nat /\ queryWrites \in Nat /\ currentGeneration \in Nat
    /\ transformState \in [TransformNames -> DerivedTransformStates]
    /\ transformInputComplete \in [TransformNames -> BOOLEAN]
    /\ transformOutputPublished \in [TransformNames -> BOOLEAN]
    /\ transformGeneration \in [TransformNames -> Nat]
    /\ transformConsumed \in [TransformNames -> BOOLEAN]
    /\ transformLifecycle \in [TransformNames -> TransformLifecycleStates]
    /\ transformReused \in [TransformNames -> BOOLEAN]
    /\ factPublished \in [FactSets -> BOOLEAN]
    /\ factGeneration \in [FactSets -> Nat]
    /\ factCompleteness \in [FactSets -> CompletenessStates]
    /\ stageCompleteness \in [StageNames -> CompletenessStates]
    /\ operandCompleteness \in [OperandNames -> CompletenessStates]
    /\ stepResults \in Seq(SUBSET ResultItemIds)
    /\ stepCompleteness \in Seq(CompletenessStates)
    /\ stepFacts \in Seq(SUBSET FactSets)
    /\ queryStarted \in BOOLEAN
    /\ frontier \subseteq GraphNodeIds
    /\ traversalVisited \subseteq GraphNodeIds
    /\ traversalOrder \in Seq(GraphNodeIds)
    /\ traversalTruncated \in BOOLEAN
    /\ seeded \in BOOLEAN /\ Len(trace) <= TraceBound

Init ==
    /\ candidatePlan = SetUnionOption
    /\ candidateSteps = SetUnionSteps
    /\ leftOperand = LeftSet /\ rightOperand = RightSet /\ fixture = "normal"
    /\ planValidated = FALSE /\ planRejected = FALSE /\ queryExecuted = FALSE
    /\ resultReturned = FALSE /\ resultStatus = "none"
    /\ resultItems = LeftSet \cup RightSet /\ resultOrder = CanonicalResultOrder
    /\ resultRelations = {"rel-1", "rel-2"}
    /\ witnessPath = <<PathStart, "node-2", PathTarget>>
    /\ sourceCompleteness = "complete" /\ filterCompleteness = "complete"
    /\ viewCompleteness = "complete" /\ leftCompleteness = "complete"
    /\ rightCompleteness = "complete" /\ traverseCompleteness = "complete"
    /\ evidenceCompleteness = "complete" /\ limitCompleteness = "complete"
    /\ abstractIndexVersion = InitialIndexVersion /\ queryWrites = 0
    /\ currentGeneration = 1
    /\ transformState = [t \in TransformNames |-> "current"]
    /\ transformInputComplete = [t \in TransformNames |-> TRUE]
    /\ transformOutputPublished = [t \in TransformNames |-> TRUE]
    /\ transformGeneration = [t \in TransformNames |-> 1]
    /\ transformConsumed = [t \in TransformNames |-> FALSE]
    /\ transformLifecycle = [t \in TransformNames |-> "published"]
    /\ transformReused = [t \in TransformNames |-> FALSE]
    /\ factPublished = [f \in FactSets |-> TRUE]
    /\ factGeneration = [f \in FactSets |-> 1]
    /\ factCompleteness = [f \in FactSets |-> "complete"]
    /\ stageCompleteness = [s \in StageNames |-> "complete"]
    /\ operandCompleteness = [o \in OperandNames |-> "complete"]
    /\ stepResults = EvaluateStepResults(SetUnionSteps, 1, {}, "normal",
                                          LeftSet, RightSet)
    /\ stepCompleteness = EvaluateStepCompleteness(SetUnionSteps, 1, {},
                                                    "complete", "normal",
                                                    LeftSet, RightSet, 0)
    /\ stepFacts = EvaluateStepFacts(SetUnionSteps)
    /\ queryStarted = FALSE /\ frontier = {} /\ traversalVisited = {}
    /\ traversalOrder = <<>> /\ traversalTruncated = FALSE
    /\ seeded = FALSE /\ trace = <<"Init">>

SelectPlan ==
    /\ Len(trace) = 1 /\ ~planValidated /\ ~queryExecuted /\ ~seeded
    /\ \E option \in PlanOptions :
        /\ candidatePlan' = option
        /\ candidateSteps' = option.steps
        /\ leftOperand' = LeftSet /\ rightOperand' = RightSet
        /\ fixture' = "normal" /\ trace' = Append(trace, option.name)
    /\ UNCHANGED <<planValidated, planRejected, queryExecuted, resultReturned,
        resultStatus, resultItems, resultOrder, resultRelations, witnessPath,
        sourceCompleteness, filterCompleteness, viewCompleteness, leftCompleteness,
        rightCompleteness, traverseCompleteness, evidenceCompleteness,
        limitCompleteness, abstractIndexVersion, queryWrites, currentGeneration,
        transformState, transformInputComplete, transformOutputPublished,
        transformGeneration, transformConsumed, transformLifecycle, transformReused,
        factPublished, factGeneration, factCompleteness, stageCompleteness,
        operandCompleteness, stepResults, stepCompleteness, stepFacts,
        queryStarted, frontier, traversalVisited, traversalOrder,
        traversalTruncated, seeded>>

ValidatePlan ==
    /\ ~planValidated /\ ~planRejected /\ PlanValid(candidatePlan)
    /\ PlanStepsValid(candidateSteps) /\ planValidated' = TRUE
    /\ trace' = Append(trace, "ValidatePlan")
    /\ UNCHANGED <<candidatePlan, candidateSteps, leftOperand, rightOperand, fixture,
        planRejected, queryExecuted, resultReturned, resultStatus, resultItems,
        resultOrder, resultRelations, witnessPath, sourceCompleteness,
        filterCompleteness, viewCompleteness, leftCompleteness, rightCompleteness,
        traverseCompleteness, evidenceCompleteness, limitCompleteness,
        abstractIndexVersion, queryWrites, currentGeneration, transformState,
        transformInputComplete, transformOutputPublished, transformGeneration,
        transformConsumed, transformLifecycle, transformReused, factPublished,
        factGeneration, factCompleteness, stageCompleteness, operandCompleteness,
        stepResults, stepCompleteness, stepFacts, queryStarted, frontier,
        traversalVisited, traversalOrder, traversalTruncated, seeded>>

SelectFixture ==
    /\ Len(trace) = 3 /\ planValidated /\ ~queryExecuted /\ candidatePlan.stream = "set"
    /\ ~seeded /\ fixture' \in Fixtures \ {"normal"}
    /\ trace' = Append(trace, "SelectFixture")
    /\ UNCHANGED <<candidatePlan, candidateSteps, leftOperand, rightOperand,
        planValidated, planRejected, queryExecuted, resultReturned, resultStatus,
        resultItems, resultOrder, resultRelations, witnessPath, sourceCompleteness,
        filterCompleteness, viewCompleteness, leftCompleteness, rightCompleteness,
        traverseCompleteness, evidenceCompleteness, limitCompleteness,
        abstractIndexVersion, queryWrites, currentGeneration, transformState,
        transformInputComplete, transformOutputPublished, transformGeneration,
        transformConsumed, transformLifecycle, transformReused, factPublished,
        factGeneration, factCompleteness, stageCompleteness, operandCompleteness,
        stepResults, stepCompleteness, stepFacts, queryStarted, frontier,
        traversalVisited, traversalOrder, traversalTruncated, seeded>>

ExecuteQuery ==
    LET evaluatedResults == EvaluateStepResults(candidateSteps, 1, {}, fixture,
                                                leftOperand, rightOperand)
        evaluatedCompleteness == EvaluateStepCompleteness(candidateSteps, 1, {},
                                                           "complete", fixture,
                                                           leftOperand, rightOperand, 0)
    IN /\ planValidated /\ ~queryStarted /\ ~queryExecuted
       /\ queryStarted' = TRUE
       /\ queryExecuted' = (fixture \notin {"cycle", "diamond", "fanout"})
       /\ stepResults' = evaluatedResults
       /\ stepCompleteness' = evaluatedCompleteness
       /\ stepFacts' = EvaluateStepFacts(candidateSteps)
       /\ resultItems' = IF fixture \in {"cycle", "diamond", "fanout"}
                         THEN {} ELSE evaluatedResults[Len(evaluatedResults)]
       /\ resultOrder' = IF fixture \in {"cycle", "diamond", "fanout"}
                         THEN <<>>
                         ELSE CanonicalOrder(evaluatedResults[Len(evaluatedResults)])
       /\ resultRelations' = FixtureRelations(fixture)
       /\ witnessPath' = FixtureWitness(fixture)
       /\ frontier' = IF fixture \in {"cycle", "diamond", "fanout"}
                      THEN {PathStart} ELSE {}
       /\ traversalVisited' = {}
       /\ traversalOrder' = <<>>
       /\ traversalTruncated' = FALSE
       /\ trace' = Append(trace, "ExecuteQuery")
       /\ UNCHANGED <<candidatePlan, candidateSteps, leftOperand, rightOperand, fixture,
           planValidated, planRejected, resultReturned, resultStatus,
           sourceCompleteness, filterCompleteness, viewCompleteness, leftCompleteness,
           rightCompleteness, traverseCompleteness, evidenceCompleteness,
           limitCompleteness, abstractIndexVersion, queryWrites, currentGeneration,
           transformState, transformInputComplete, transformOutputPublished,
           transformGeneration, transformConsumed, transformLifecycle, transformReused,
           factPublished, factGeneration, factCompleteness, stageCompleteness,
           operandCompleteness, seeded>>

AdvanceTraversal ==
    LET node == CanonicalNode(frontier)
        nextFrontier == (frontier \ {node})
                        \cup (NeighborSet(fixture, node) \ traversalVisited)
        nextVisited == traversalVisited \cup {node}
        nextOrder == Append(traversalOrder, node)
        nextTruncated == Len(nextOrder) >= candidatePlan.limit
                           \/ Len(nextOrder) >= AdversarialBound
        done == nextFrontier = {} \/ nextTruncated
    IN /\ queryStarted /\ ~queryExecuted
       /\ candidatePlan.stream = "set"
       /\ fixture \in {"cycle", "diamond", "fanout"}
       /\ frontier # {}
       /\ frontier' = IF done THEN {} ELSE nextFrontier
       /\ traversalVisited' = nextVisited
       /\ traversalOrder' = nextOrder
       /\ traversalTruncated' = nextTruncated
       /\ queryExecuted' = done
       /\ resultItems' = IF done THEN nextVisited ELSE resultItems
       /\ resultOrder' = IF done THEN nextOrder ELSE resultOrder
       /\ resultRelations' = IF done THEN FixtureRelations(fixture) ELSE resultRelations
       /\ stepResults' = IF done
                         THEN [stepResults EXCEPT ![Len(stepResults)] = nextVisited]
                        ELSE stepResults
       /\ stepCompleteness' = IF done /\ nextTruncated
                              THEN [stepCompleteness EXCEPT
                                    ![Len(stepCompleteness)] = "partial"]
                              ELSE stepCompleteness
       /\ trace' = Append(trace, "AdvanceTraversal")
       /\ UNCHANGED <<candidatePlan, candidateSteps, leftOperand, rightOperand, fixture,
           planValidated, planRejected, resultReturned, resultStatus, witnessPath,
           sourceCompleteness, filterCompleteness, viewCompleteness, leftCompleteness,
           rightCompleteness, traverseCompleteness, evidenceCompleteness,
           limitCompleteness, abstractIndexVersion, queryWrites, currentGeneration,
           transformState, transformInputComplete, transformOutputPublished,
           transformGeneration, transformConsumed, transformLifecycle, transformReused,
           factPublished, factGeneration, factCompleteness, stageCompleteness,
           operandCompleteness, stepFacts, queryStarted, seeded>>

ReturnResult ==
    /\ queryExecuted /\ ~resultReturned /\ resultReturned' = TRUE
    /\ resultStatus' = ComputedResultCompleteness
    /\ trace' = Append(trace, "ReturnResult")
    /\ UNCHANGED <<candidatePlan, candidateSteps, leftOperand, rightOperand, fixture,
        planValidated, planRejected, queryExecuted, resultItems, resultOrder,
        resultRelations, witnessPath, sourceCompleteness, filterCompleteness,
        viewCompleteness, leftCompleteness, rightCompleteness, traverseCompleteness,
        evidenceCompleteness, limitCompleteness, abstractIndexVersion, queryWrites,
        currentGeneration, transformState, transformInputComplete,
        transformOutputPublished, transformGeneration, transformConsumed,
        transformLifecycle, transformReused, factPublished, factGeneration,
        factCompleteness, stageCompleteness, operandCompleteness, stepResults,
        stepCompleteness, stepFacts, queryStarted, frontier, traversalVisited,
        traversalOrder, traversalTruncated, seeded>>

ObserveResult ==
    /\ queryExecuted /\ resultReturned
    /\ UNCHANGED vars

PlanTransform ==
    \E t \in TransformNames :
        /\ ~planValidated
        /\ ((Len(trace) = 1 /\ ~seeded) \/ (Len(trace) = 2 /\ seeded))
        /\ transformLifecycle[t] \in {"published", "stale", "failed"}
        /\ ~transformReused[t]
        /\ transformLifecycle' = [transformLifecycle EXCEPT ![t] = "planned"]
        /\ transformState' = [transformState EXCEPT ![t] = "planned"]
        /\ trace' = Append(trace, "PlanTransform")
    /\ UNCHANGED <<candidatePlan, candidateSteps, leftOperand, rightOperand, fixture,
        planValidated, planRejected, queryExecuted, resultReturned, resultStatus,
        resultItems, resultOrder, resultRelations, witnessPath, sourceCompleteness,
        filterCompleteness, viewCompleteness, leftCompleteness, rightCompleteness,
        traverseCompleteness, evidenceCompleteness, limitCompleteness,
        abstractIndexVersion, queryWrites, currentGeneration, transformState,
        transformInputComplete, transformOutputPublished, transformGeneration,
        transformConsumed, transformReused, factPublished, factGeneration,
        factCompleteness, stageCompleteness, operandCompleteness, stepResults,
        stepCompleteness, stepFacts, queryStarted, frontier, traversalVisited,
        traversalOrder, traversalTruncated, seeded>>

RunTransform ==
    \E t \in TransformNames :
        /\ ~planValidated /\ ~queryExecuted
        /\ transformLifecycle[t] = "planned" /\ DependenciesFresh(t)
        /\ transformLifecycle' = [transformLifecycle EXCEPT ![t] = "running"]
        /\ trace' = Append(trace, "RunTransform")
    /\ UNCHANGED <<candidatePlan, candidateSteps, leftOperand, rightOperand, fixture,
        planValidated, planRejected, queryExecuted, resultReturned, resultStatus,
        resultItems, resultOrder, resultRelations, witnessPath, sourceCompleteness,
        filterCompleteness, viewCompleteness, leftCompleteness, rightCompleteness,
        traverseCompleteness, evidenceCompleteness, limitCompleteness,
        abstractIndexVersion, queryWrites, currentGeneration, transformState,
        transformInputComplete, transformOutputPublished, transformGeneration,
        transformConsumed, transformReused, factPublished, factGeneration,
        factCompleteness, stageCompleteness, operandCompleteness, stepResults,
        stepCompleteness, stepFacts, queryStarted, frontier, traversalVisited,
        traversalOrder, traversalTruncated, seeded>>

PublishTransform ==
    \E t \in TransformNames :
        /\ ~planValidated /\ ~queryExecuted
        /\ transformLifecycle[t] = "running" /\ transformInputComplete[t]
        /\ DependenciesFresh(t)
        /\ transformLifecycle' = [transformLifecycle EXCEPT ![t] = "published"]
        /\ transformState' = [transformState EXCEPT ![t] = "current"]
        /\ transformOutputPublished' = [transformOutputPublished EXCEPT ![t] = TRUE]
        /\ transformGeneration' = [transformGeneration EXCEPT ![t] = currentGeneration]
        /\ transformConsumed' = [transformConsumed EXCEPT ![t] = FALSE]
        /\ transformReused' = [transformReused EXCEPT ![t] = FALSE]
        /\ factPublished' = [f \in FactSets |->
             IF f \in ProducedFacts(t) THEN TRUE ELSE factPublished[f]]
        /\ factGeneration' = [f \in FactSets |->
             IF f \in ProducedFacts(t) THEN currentGeneration ELSE factGeneration[f]]
        /\ factCompleteness' = [f \in FactSets |->
             IF f \in ProducedFacts(t) THEN "complete" ELSE factCompleteness[f]]
        /\ trace' = Append(trace, "PublishTransform")
    /\ UNCHANGED <<candidatePlan, candidateSteps, leftOperand, rightOperand, fixture,
        planValidated, planRejected, queryExecuted, resultReturned, resultStatus,
        resultItems, resultOrder, resultRelations, witnessPath, sourceCompleteness,
        filterCompleteness, viewCompleteness, leftCompleteness, rightCompleteness,
        traverseCompleteness, evidenceCompleteness, limitCompleteness,
        abstractIndexVersion, queryWrites, currentGeneration, transformInputComplete,
        transformLifecycle, factCompleteness,
        stageCompleteness, operandCompleteness, stepResults, stepCompleteness,
        stepFacts, queryStarted, frontier, traversalVisited, traversalOrder,
        traversalTruncated, seeded>>

FailTransform ==
    \E t \in TransformNames :
        /\ ~planValidated /\ ~queryExecuted /\ transformLifecycle[t] = "running"
        /\ LET affected == AffectedTransforms(t) IN
            /\ transformLifecycle' = [x \in TransformNames |->
                 IF x = t THEN "failed"
                 ELSE IF x \in DescendantsOf(t) THEN "stale"
                 ELSE transformLifecycle[x]]
            /\ transformState' = [x \in TransformNames |->
                 IF x = t THEN "failed"
                 ELSE IF x \in DescendantsOf(t) THEN "stale"
                 ELSE transformState[x]]
            /\ transformOutputPublished' = [x \in TransformNames |->
                 IF x \in affected THEN FALSE ELSE transformOutputPublished[x]]
            /\ transformConsumed' = [x \in TransformNames |->
                 IF x \in affected THEN FALSE ELSE transformConsumed[x]]
            /\ transformReused' = [x \in TransformNames |->
                 IF x \in affected THEN FALSE ELSE transformReused[x]]
            /\ factPublished' = [f \in FactSets |->
                 IF \E x \in affected : f \in ProducedFacts(x)
                 THEN FALSE ELSE factPublished[f]]
            /\ factCompleteness' = [f \in FactSets |->
                 IF \E x \in affected : f \in ProducedFacts(x)
                 THEN "unknown" ELSE factCompleteness[f]]
        /\ trace' = Append(trace, "FailTransform")
    /\ UNCHANGED <<candidatePlan, candidateSteps, leftOperand, rightOperand, fixture,
        planValidated, planRejected, queryExecuted, resultReturned, resultStatus,
        resultItems, resultOrder, resultRelations, witnessPath, sourceCompleteness,
        filterCompleteness, viewCompleteness, leftCompleteness, rightCompleteness,
        traverseCompleteness, evidenceCompleteness, limitCompleteness,
        abstractIndexVersion, queryWrites, currentGeneration, transformInputComplete,
        transformGeneration, transformLifecycle, factGeneration,
        stageCompleteness, operandCompleteness, stepResults, stepCompleteness,
        stepFacts, queryStarted, frontier, traversalVisited, traversalOrder,
        traversalTruncated, seeded>>

InvalidateTransform ==
    \E t \in TransformNames :
        /\ ~planValidated /\ ~queryExecuted
        /\ transformLifecycle[t] = "published" /\ ~transformReused[t]
        /\ LET affected == AffectedTransforms(t) IN
            /\ transformLifecycle' = [x \in TransformNames |->
                 IF x \in affected THEN "stale" ELSE transformLifecycle[x]]
            /\ transformState' = [x \in TransformNames |->
                 IF x \in affected THEN "stale" ELSE transformState[x]]
            /\ transformOutputPublished' = [x \in TransformNames |->
                 IF x \in affected THEN FALSE ELSE transformOutputPublished[x]]
            /\ transformConsumed' = [x \in TransformNames |->
                 IF x \in affected THEN FALSE ELSE transformConsumed[x]]
            /\ transformReused' = [x \in TransformNames |->
                 IF x \in affected THEN FALSE ELSE transformReused[x]]
            /\ factPublished' = [f \in FactSets |->
                 IF \E x \in affected : f \in ProducedFacts(x)
                 THEN FALSE ELSE factPublished[f]]
            /\ factCompleteness' = [f \in FactSets |->
                 IF \E x \in affected : f \in ProducedFacts(x)
                 THEN "unknown" ELSE factCompleteness[f]]
        /\ trace' = Append(trace, "InvalidateTransform")
    /\ UNCHANGED <<candidatePlan, candidateSteps, leftOperand, rightOperand, fixture,
        planValidated, planRejected, queryExecuted, resultReturned, resultStatus,
        resultItems, resultOrder, resultRelations, witnessPath, sourceCompleteness,
        filterCompleteness, viewCompleteness, leftCompleteness, rightCompleteness,
        traverseCompleteness, evidenceCompleteness, limitCompleteness,
        abstractIndexVersion, queryWrites, currentGeneration, transformInputComplete,
        transformGeneration, transformLifecycle, factGeneration,
        stageCompleteness, operandCompleteness, stepResults, stepCompleteness,
        stepFacts, queryStarted, frontier, traversalVisited, traversalOrder,
        traversalTruncated, seeded>>

ReuseTransform ==
    \E t \in TransformNames :
        /\ ~planValidated /\ ~queryExecuted /\ transformLifecycle[t] = "published"
        /\ ~transformReused[t] /\ FreshTransform(t)
        /\ transformConsumed' = [transformConsumed EXCEPT ![t] = TRUE]
        /\ transformReused' = [transformReused EXCEPT ![t] = TRUE]
        /\ trace' = Append(trace, "ReuseTransform")
    /\ UNCHANGED <<candidatePlan, candidateSteps, leftOperand, rightOperand, fixture,
        planValidated, planRejected, queryExecuted, resultReturned, resultStatus,
        resultItems, resultOrder, resultRelations, witnessPath, sourceCompleteness,
        filterCompleteness, viewCompleteness, leftCompleteness, rightCompleteness,
        traverseCompleteness, evidenceCompleteness, limitCompleteness,
        abstractIndexVersion, queryWrites, currentGeneration, transformState,
        transformInputComplete, transformOutputPublished, transformGeneration,
        transformLifecycle, factPublished, factGeneration, factCompleteness,
        stageCompleteness, operandCompleteness, stepResults, stepCompleteness,
        stepFacts, queryStarted, frontier, traversalVisited, traversalOrder,
        traversalTruncated, seeded>>

SeedPlan(defect) ==
    IF defect = "witness-above-bound"
    THEN PathDepth1Option
    ELSE IF defect \in {"witness-below-bound", "witness-missing-edge"}
    THEN PathDepth2Option
    ELSE IF defect \in {"cycle-loop", "diamond-duplicate", "fanout-dedup",
                         "fanout-complete", "fanout-truncated"}
    THEN ViewNodesOption
    ELSE IF defect \in {"unknown-target-complete", "partial-evidence-complete"}
    THEN ViewRelationsOption
    ELSE IF defect = "truncated-limit"
    THEN [SetUnionOption EXCEPT !.limit = 2,
                               !.steps = [SetUnionSteps EXCEPT ![6].limit = 2]]
    ELSE SetUnionOption

SeedSteps(defect) ==
    IF defect = "illegal-source"
    THEN [SetUnionSteps EXCEPT ![1].outputView = "relations"]
    ELSE IF defect = "illegal-view"
    THEN [SetUnionSteps EXCEPT ![2].outputStream = "path"]
    ELSE IF defect = "illegal-filter"
    THEN [SetUnionSteps EXCEPT ![2].outputView = "paths"]
    ELSE IF defect = "illegal-traverse"
    THEN [SetUnionSteps EXCEPT ![3].operation = "traverse"]
    ELSE IF defect = "illegal-set"
    THEN [SetUnionSteps EXCEPT ![3].leftView = "relations"]
    ELSE IF defect = "illegal-select"
    THEN [SetUnionSteps EXCEPT ![4].selection = "relation"]
    ELSE IF defect = "illegal-order"
    THEN [SetUnionSteps EXCEPT ![5].outputView = "paths"]
    ELSE IF defect = "illegal-limit"
    THEN [SetUnionSteps EXCEPT ![6].outputView = "paths"]
    ELSE IF defect = "illegal-later-source"
    THEN [SetUnionSteps EXCEPT ![2].operation = "source"]
    ELSE IF defect = "illegal-terminal"
    THEN [SetUnionSteps EXCEPT ![6].outputView = "relations"]
    ELSE SetUnionSteps

SeedTransformName(defect) ==
    IF defect \in {"stale-resolve", "failed-resolve"} THEN "resolve"
    ELSE IF defect \in {"stale-summary", "failed-summary"} THEN "summary"
    ELSE "answer"

SeedTransformAffected(defect) == AffectedTransforms(SeedTransformName(defect))

SeedFixture(defect) ==
    IF defect = "cycle-loop" THEN "cycle"
    ELSE IF defect = "diamond-duplicate" THEN "diamond"
    ELSE IF defect \in {"fanout-dedup", "fanout-complete", "fanout-truncated"}
    THEN "fanout"
    ELSE "normal"

SeedTransformState(defect) ==
    IF defect = "stale-resolve"
    THEN [transformState EXCEPT !["resolve"] = "stale",
                              !["answer"] = "current",
                              !["summary"] = "current"]
    ELSE IF defect = "failed-resolve"
    THEN [transformState EXCEPT !["resolve"] = "failed",
                              !["answer"] = "current",
                              !["summary"] = "current"]
    ELSE IF defect = "stale-summary"
    THEN [transformState EXCEPT !["summary"] = "stale"]
    ELSE IF defect = "failed-summary"
    THEN [transformState EXCEPT !["summary"] = "failed"]
    ELSE IF defect = "stale-answer-consumption"
    THEN [transformState EXCEPT !["answer"] = "stale"]
    ELSE IF defect = "failed-answer-publication"
    THEN [transformState EXCEPT !["answer"] = "failed"]
    ELSE transformState

SeedTransformLifecycle(defect) ==
    IF defect = "stale-resolve"
    THEN [transformLifecycle EXCEPT !["resolve"] = "stale",
                                 !["answer"] = "published",
                                 !["summary"] = "published"]
    ELSE IF defect = "failed-resolve"
    THEN [transformLifecycle EXCEPT !["resolve"] = "failed",
                                 !["answer"] = "published",
                                 !["summary"] = "published"]
    ELSE IF defect = "stale-summary"
    THEN [transformLifecycle EXCEPT !["summary"] = "stale"]
    ELSE IF defect = "failed-summary"
    THEN [transformLifecycle EXCEPT !["summary"] = "failed"]
    ELSE IF defect = "stale-answer-consumption"
    THEN [transformLifecycle EXCEPT !["answer"] = "stale"]
    ELSE IF defect = "failed-answer-publication"
    THEN [transformLifecycle EXCEPT !["answer"] = "failed"]
    ELSE transformLifecycle

SeedTransformOutput(defect) ==
    IF defect \in {"stale-resolve", "failed-resolve"}
    THEN [transformOutputPublished EXCEPT !["resolve"] = FALSE]
    ELSE IF defect \in {"stale-summary", "failed-summary"}
    THEN [transformOutputPublished EXCEPT !["summary"] = FALSE]
    ELSE IF defect = "stale-answer-consumption"
    THEN [transformOutputPublished EXCEPT !["answer"] = FALSE]
    ELSE IF defect = "failed-answer-publication"
    THEN [transformOutputPublished EXCEPT !["answer"] = TRUE]
    ELSE transformOutputPublished

SeedTransformConsumed(defect) ==
    IF defect \in {"stale-summary", "failed-summary", "stale-answer-consumption"}
    THEN [transformConsumed EXCEPT ![SeedTransformName(defect)] = TRUE]
    ELSE transformConsumed

SeedFactPublished(defect) ==
    IF defect \in {"stale-resolve", "failed-resolve", "stale-summary", "failed-summary"}
    THEN [f \in FactSets |->
             IF \E t \in SeedTransformAffected(defect) : f \in ProducedFacts(t)
             THEN FALSE ELSE factPublished[f]]
    ELSE IF defect = "stale-answer-consumption"
    THEN [factPublished EXCEPT !["answer-facts-v1"] = FALSE]
    ELSE factPublished

SeedFactCompleteness(defect) ==
    IF defect = "partial-transform"
    THEN [factCompleteness EXCEPT !["answer-facts-v1"] = "partial"]
    ELSE IF defect \in {"stale-resolve", "failed-resolve", "stale-summary",
                         "failed-summary", "stale-answer-consumption"}
    THEN [f \in FactSets |->
             IF \E t \in SeedTransformAffected(defect) : f \in ProducedFacts(t)
             THEN "unknown" ELSE factCompleteness[f]]
    ELSE factCompleteness

SeedExecutionSteps(defect) ==
    IF defect \in {"illegal-source", "illegal-view", "illegal-filter",
                    "illegal-traverse", "illegal-set", "illegal-select",
                    "illegal-order", "illegal-limit", "illegal-later-source",
                    "illegal-terminal"}
    THEN SeedSteps(defect)
    ELSE IF defect = "illegal-plan-operation"
    THEN SetUnionSteps
    ELSE SeedPlan(defect).steps

SeedExecutionResults(defect) ==
    EvaluateStepResults(SeedExecutionSteps(defect), 1, {}, SeedFixture(defect),
                        LeftSet, RightSet)

SeedExecutionCompleteness(defect) ==
    LET base == EvaluateStepCompleteness(SeedExecutionSteps(defect), 1, {},
                                         "complete", SeedFixture(defect),
                                         LeftSet, RightSet, 0)
    IN IF defect \in {"partial-left", "partial-right", "partial-filter",
                       "partial-view", "partial-traverse", "partial-evidence",
                       "partial-transform", "truncated-limit",
                       "fanout-complete", "fanout-truncated"}
       THEN [base EXCEPT ![Len(base)] = "partial"]
       ELSE base

SeedTraversal(defect) ==
    LET expected == ExpectedTraversal(SeedFixture(defect), 10) IN
        IF defect = "cycle-loop"
        THEN [expected EXCEPT !.order = Append(expected.order, "node-1")]
        ELSE IF defect = "diamond-duplicate"
        THEN [expected EXCEPT !.order = <<"node-1", "node-2", "node-2", "node-3">>]
        ELSE IF defect = "fanout-dedup"
        THEN [expected EXCEPT !.order = <<"node-1", "node-1", "node-2", "node-3">>]
        ELSE IF defect \in {"fanout-complete", "fanout-truncated"}
        THEN [expected EXCEPT !.truncated = TRUE]
        ELSE expected

SeedDefectNew ==
    /\ Defect \in Defects \ {"none"} /\ ~seeded /\ Len(trace) = 1
    /\ seeded' = TRUE
    /\ candidatePlan' = IF Defect = "illegal-plan-operation"
                        THEN [SetUnionOption EXCEPT !.operation = "intersection"]
                        ELSE IF Defect \in {"illegal-later-source", "illegal-terminal"}
                        THEN [SetUnionOption EXCEPT !.steps = SeedSteps(Defect)]
                        ELSE IF Defect = "witness-above-bound"
                        THEN [PathDepth1Option EXCEPT !.depth = 1]
                        ELSE SeedPlan(Defect)
    /\ candidateSteps' = SeedExecutionSteps(Defect)
    /\ planValidated' = (Defect \in {"illegal-source", "illegal-view", "illegal-filter",
        "illegal-traverse", "illegal-set", "illegal-select", "illegal-order",
        "illegal-limit", "illegal-later-source", "illegal-terminal",
        "illegal-plan-operation", "witness-below-bound", "witness-above-bound",
        "witness-missing-edge", "duplicate-results", "query-write", "partial-left",
        "partial-right", "partial-filter", "partial-view", "partial-traverse",
        "partial-evidence", "partial-transform", "truncated-limit",
        "unknown-target-complete", "partial-evidence-complete", "cycle-loop",
        "diamond-duplicate", "fanout-dedup", "fanout-complete", "fanout-truncated"})
    /\ queryExecuted' = (Defect \in {"duplicate-results", "query-write",
        "witness-below-bound", "witness-above-bound", "witness-missing-edge",
        "partial-left", "partial-right", "partial-filter", "partial-view",
        "partial-traverse", "partial-evidence", "partial-transform", "truncated-limit",
        "unknown-target-complete", "partial-evidence-complete", "cycle-loop",
        "diamond-duplicate", "fanout-dedup", "fanout-complete", "fanout-truncated"})
    /\ resultReturned' = queryExecuted'
    /\ resultItems' = IF Defect \in {"cycle-loop", "diamond-duplicate", "fanout-dedup",
                                      "fanout-complete", "fanout-truncated"}
                       THEN SeedTraversal(Defect).visited
                       ELSE IF queryExecuted'
                       THEN SeedExecutionResults(Defect)[Len(SeedExecutionResults(Defect))]
                       ELSE resultItems
    /\ resultRelations' = IF Defect = "unknown-target-complete" THEN {"rel-3"}
                          ELSE IF Defect = "partial-evidence-complete" THEN {"rel-4"}
                          ELSE resultRelations
    /\ resultOrder' = IF Defect = "duplicate-results"
                      THEN <<"node-1", "node-1", "node-2", "node-3">>
                      ELSE IF Defect \in {"cycle-loop", "diamond-duplicate", "fanout-dedup",
                                          "fanout-complete", "fanout-truncated"}
                      THEN SeedTraversal(Defect).order
                      ELSE IF queryExecuted'
                      THEN CanonicalOrder(SeedExecutionResults(Defect)[Len(SeedExecutionResults(Defect))])
                      ELSE resultOrder
    /\ witnessPath' = IF Defect = "witness-below-bound" THEN <<PathStart, "node-2">>
                      ELSE IF Defect = "witness-above-bound"
                      THEN <<PathStart, "node-2", PathTarget>>
                      ELSE IF Defect = "witness-missing-edge"
                      THEN <<PathStart, "unknown-target", PathTarget>>
                      ELSE witnessPath
    /\ resultStatus' = IF Defect = "duplicate-results"
                       THEN "partial"
                       ELSE IF queryExecuted' THEN "complete" ELSE resultStatus
    /\ fixture' = SeedFixture(Defect)
    /\ queryWrites' = IF Defect = "query-write" THEN 1 ELSE queryWrites
    /\ abstractIndexVersion' = IF Defect = "query-write"
                               THEN InitialIndexVersion + 1 ELSE abstractIndexVersion
    /\ transformState' = SeedTransformState(Defect)
    /\ transformOutputPublished' = SeedTransformOutput(Defect)
    /\ transformInputComplete' = IF Defect = "partial-transform"
                                 THEN [transformInputComplete EXCEPT !["answer"] = FALSE]
                                 ELSE transformInputComplete
    /\ transformConsumed' = SeedTransformConsumed(Defect)
    /\ transformLifecycle' = SeedTransformLifecycle(Defect)
    /\ factPublished' = SeedFactPublished(Defect)
    /\ factCompleteness' = SeedFactCompleteness(Defect)
    /\ stageCompleteness' = [s \in StageNames |->
        IF (Defect = "partial-filter" /\ s = "filter")
            \/ (Defect = "partial-view" /\ s = "view")
            \/ (Defect = "partial-traverse" /\ s = "traverse")
            \/ (Defect = "partial-evidence" /\ s = "evidence")
            \/ (Defect = "truncated-limit" /\ s = "limit")
        THEN "partial" ELSE stageCompleteness[s]]
    /\ operandCompleteness' = [o \in OperandNames |->
        IF (Defect = "partial-left" /\ o = "left")
            \/ (Defect = "partial-right" /\ o = "right")
        THEN "partial" ELSE operandCompleteness[o]]
    /\ stepResults' = IF Defect \in {"cycle-loop", "diamond-duplicate", "fanout-dedup",
                                       "fanout-complete", "fanout-truncated"}
                      THEN [SeedExecutionResults(Defect) EXCEPT
                            ![Len(SeedExecutionResults(Defect))] = SeedTraversal(Defect).visited]
                      ELSE SeedExecutionResults(Defect)
    /\ stepCompleteness' = SeedExecutionCompleteness(Defect)
    /\ stepFacts' = EvaluateStepFacts(SeedExecutionSteps(Defect))
    /\ queryStarted' = queryExecuted'
    /\ frontier' = {}
    /\ traversalVisited' = IF Defect \in {"cycle-loop", "diamond-duplicate", "fanout-dedup",
                                            "fanout-complete", "fanout-truncated"}
                                THEN SeedTraversal(Defect).visited ELSE traversalVisited
    /\ traversalOrder' = IF Defect \in {"cycle-loop", "diamond-duplicate", "fanout-dedup",
                                           "fanout-complete", "fanout-truncated"}
                               THEN SeedTraversal(Defect).order ELSE traversalOrder
    /\ traversalTruncated' = (Defect \in {"fanout-complete", "fanout-truncated"})
    /\ trace' = Append(trace, Defect)
    /\ UNCHANGED <<leftOperand, rightOperand, planRejected, sourceCompleteness,
        filterCompleteness, viewCompleteness, leftCompleteness, rightCompleteness,
        traverseCompleteness, evidenceCompleteness, limitCompleteness,
        currentGeneration, transformGeneration, transformReused, factGeneration>>

Next == SelectPlan \/ ValidatePlan \/ SelectFixture \/ ExecuteQuery \/ AdvanceTraversal
    \/ ReturnResult \/ ObserveResult
    \/ PlanTransform \/ RunTransform \/ PublishTransform \/ FailTransform
    \/ InvalidateTransform \/ ReuseTransform \/ SeedDefectNew
Fairness ==
    /\ WF_vars(ValidatePlan) /\ WF_vars(ExecuteQuery) /\ WF_vars(AdvanceTraversal)
    /\ WF_vars(ReturnResult)
    /\ WF_vars(PlanTransform) /\ WF_vars(RunTransform) /\ WF_vars(PublishTransform)
    /\ WF_vars(FailTransform) /\ WF_vars(InvalidateTransform) /\ WF_vars(ReuseTransform)
    /\ WF_vars(SeedDefectNew)
Spec == Init /\ [][Next]_SemanticVars /\ Fairness
SemanticLiveness == [](planValidated /\ ~queryExecuted => <> queryExecuted)

=============================================================================
