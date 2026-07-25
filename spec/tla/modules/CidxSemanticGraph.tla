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
TransformLifecycleStates == {"planned", "running", "published", "stale", "failed"}
TransformNames == {"resolve", "answer"}
FactSets == {"raw-facts", "resolved-facts-v1", "answer-facts-v1"}
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
TransformDependencyEdges == {<<"answer", "resolve">>}
TransformRequiredFacts == {<<"resolve", "raw-facts">>,
                           <<"answer", "resolved-facts-v1">>}
TransformProducedFacts == {<<"resolve", "resolved-facts-v1">>,
                           <<"answer", "answer-facts-v1">>}
Defects == {"none", "illegal-source", "illegal-view", "illegal-filter",
            "illegal-traverse", "illegal-set", "illegal-select", "illegal-order",
            "illegal-limit", "duplicate-results", "query-write",
            "witness-below-bound", "witness-above-bound", "witness-missing-edge",
            "partial-left", "partial-right", "partial-filter", "partial-view",
            "partial-traverse", "partial-evidence", "partial-transform",
            "truncated-limit", "unknown-target-complete", "partial-evidence-complete",
            "cycle-loop", "diamond-duplicate", "fanout-dedup",
            "fanout-complete", "stale-resolve", "failed-resolve",
            "stale-answer-consumption", "failed-answer-publication"}

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
    factPublished, factGeneration, seeded, trace

SemanticVars == <<candidatePlan, candidateSteps, leftOperand, rightOperand, fixture,
    planValidated, planRejected, queryExecuted, resultReturned, resultStatus,
    resultItems, resultOrder, resultRelations, witnessPath,
    sourceCompleteness, filterCompleteness, viewCompleteness, leftCompleteness,
    rightCompleteness, traverseCompleteness, evidenceCompleteness,
    limitCompleteness, abstractIndexVersion, queryWrites, currentGeneration,
    transformState, transformInputComplete, transformOutputPublished,
    transformGeneration, transformConsumed, transformLifecycle, transformReused,
    factPublished, factGeneration, seeded, trace>>
vars == SemanticVars

SequenceValues(s) == {s[i] : i \in 1..Len(s)}
DistinctSequence(s) == Len(s) = Cardinality(SequenceValues(s))
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

FixtureItems(f) ==
    IF f = "cycle" THEN {"node-1", "node-2"}
    ELSE IF f = "diamond" THEN {"node-1", "node-2", "node-3"}
    ELSE IF f = "fanout" THEN FanoutTargets
    ELSE IF f = "unknown-target" THEN {"record-1", "unknown-target"}
    ELSE IF f = "partial-evidence" THEN {"node-1", "node-3"}
    ELSE IF candidatePlan.view = "relations" THEN {"rel-1", "rel-2"}
    ELSE IF candidatePlan.view = "evidence" THEN {"evidence-1", "evidence-2"}
    ELSE IF candidatePlan.stream = "path" THEN {"node-1", "node-2", "node-3"}
    ELSE SetOperationResult(candidatePlan.operation, leftOperand, rightOperand)

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
    THEN /\ resultItems = FixtureItems(fixture)
         /\ resultRelations = FixtureRelations(fixture)
         /\ resultOrder = CanonicalOrder(resultItems) /\ DistinctSequence(resultOrder)
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
    => WitnessPathValid(witnessPath, candidatePlan.pathStart,
                         candidatePlan.pathTarget, candidatePlan.depth)

JoinCompleteness(a, b) ==
    IF a = "unknown" \/ b = "unknown" THEN "unknown"
    ELSE IF a = "partial" \/ b = "partial" THEN "partial"
    ELSE "complete"

RelationCompleteness(relations) ==
    IF "rel-3" \in relations THEN "unknown"
    ELSE IF "rel-4" \in relations THEN "partial"
    ELSE "complete"

RequiredFact(t) == IF t = "resolve" THEN "raw-facts" ELSE "resolved-facts-v1"
ProducedFact(t) == IF t = "resolve" THEN "resolved-facts-v1" ELSE "answer-facts-v1"
ParentTransform(t) == IF t = "answer" THEN "resolve" ELSE "none"

DependenciesFresh(t) ==
    IF t = "resolve"
    THEN /\ factPublished["raw-facts"]
         /\ factGeneration["raw-facts"] = currentGeneration
         /\ transformState["resolve"] = "current"
         /\ transformOutputPublished["resolve"]
         /\ transformGeneration["resolve"] = currentGeneration
    ELSE /\ factPublished["raw-facts"]
         /\ factGeneration["raw-facts"] = currentGeneration
         /\ transformState["resolve"] = "current"
         /\ transformOutputPublished["resolve"]
         /\ transformGeneration["resolve"] = currentGeneration
         /\ factPublished["resolved-facts-v1"]
         /\ factGeneration["resolved-facts-v1"] = currentGeneration
         /\ transformState["answer"] = "current"
         /\ transformOutputPublished["answer"]
         /\ transformGeneration["answer"] = currentGeneration

FreshTransform(t) ==
    /\ DependenciesFresh(t)
    /\ transformInputComplete[t]

TransformCompleteness ==
    IF ~DependenciesFresh("answer") THEN "unknown"
    ELSE IF ~transformInputComplete["answer"] THEN "partial"
    ELSE "complete"

PlanInputCompleteness ==
    JoinCompleteness(sourceCompleteness,
        JoinCompleteness(filterCompleteness,
            JoinCompleteness(viewCompleteness,
                JoinCompleteness(
                    IF candidatePlan.operation \in {"union", "intersection", "difference"}
                    THEN JoinCompleteness(leftCompleteness, rightCompleteness)
                    ELSE IF candidatePlan.operation = "traverse"
                    THEN traverseCompleteness
                    ELSE "complete",
                    JoinCompleteness(traverseCompleteness, evidenceCompleteness)))))

FixtureCompleteness ==
    IF fixture = "unknown-target" THEN "unknown"
    ELSE IF fixture = "partial-evidence" \/ fixture = "fanout" THEN "partial"
    ELSE "complete"

ComputedResultCompleteness ==
    JoinCompleteness(
        JoinCompleteness(FixtureCompleteness, PlanInputCompleteness),
        JoinCompleteness(RelationCompleteness(resultRelations),
                         JoinCompleteness(limitCompleteness,
                                          JoinCompleteness(evidenceCompleteness,
                                                           TransformCompleteness))))

CompletenessInvariant ==
    resultReturned => /\ resultStatus = ComputedResultCompleteness
                      /\ (resultStatus = "complete" =>
                          /\ RelationCompleteness(resultRelations) = "complete"
                          /\ evidenceCompleteness = "complete"
                          /\ limitCompleteness = "complete"
                          /\ TransformCompleteness = "complete")

ReadOnlyExecutionInvariant ==
    /\ queryWrites = 0 /\ (queryExecuted => abstractIndexVersion = InitialIndexVersion)

TransformDependencyInvariant ==
    /\ TransformDependencyEdges = {<<"answer", "resolve">>}
    /\ \A t \in TransformNames :
        /\ RequiredFact(t) \in FactSets /\ ProducedFact(t) \in FactSets
        /\ (t = "answer" => <<"answer", ParentTransform(t)>> \in TransformDependencyEdges)

TransformStalePropagationInvariant ==
    /\ transformState["resolve"] \in {"stale", "failed"}
        => /\ transformState["answer"] \in {"stale", "failed"}
           /\ ~transformOutputPublished["answer"]
           /\ ~transformConsumed["answer"]

TransformPublicationInvariant ==
    \A t \in TransformNames :
        /\ (transformState[t] = "current" =>
            /\ transformInputComplete[t]
            /\ transformOutputPublished[t]
            /\ transformGeneration[t] = currentGeneration
            /\ factPublished[ProducedFact(t)]
            /\ factGeneration[ProducedFact(t)] = currentGeneration)
        /\ (transformState[t] \in {"stale", "failed"} =>
            /\ ~transformOutputPublished[t] /\ ~transformConsumed[t])

TransformConsumptionInvariant ==
    \A t \in TransformNames : transformConsumed[t] => DependenciesFresh(t)

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
        /\ resultItems = {"node-1", "node-2"}
        /\ resultOrder = <<"node-1", "node-2">>
        /\ DistinctSequence(resultOrder)

DiamondExecutionInvariant ==
    queryExecuted /\ fixture = "diamond" =>
        /\ resultItems = {"node-1", "node-2", "node-3"}
        /\ resultOrder = CanonicalResultOrder
        /\ DistinctSequence(resultOrder)

FanoutExecutionInvariant ==
    queryExecuted /\ fixture = "fanout" =>
        /\ resultItems = FanoutTargets
        /\ resultOrder = CanonicalResultOrder
        /\ resultStatus \in {"none", "partial"}

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
        factPublished, factGeneration, seeded>>

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
        factGeneration, seeded>>

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
        factGeneration, seeded>>

ExecuteQuery ==
    /\ planValidated /\ ~queryExecuted /\ DependenciesFresh("answer")
    /\ queryExecuted' = TRUE /\ resultItems' = FixtureItems(fixture)
    /\ resultOrder' = CanonicalOrder(FixtureItems(fixture))
    /\ resultRelations' = FixtureRelations(fixture)
    /\ witnessPath' = FixtureWitness(fixture)
    /\ trace' = Append(trace, "ExecuteQuery")
    /\ UNCHANGED <<candidatePlan, candidateSteps, leftOperand, rightOperand, fixture,
        planValidated, planRejected, resultReturned, resultStatus,
        sourceCompleteness, filterCompleteness, viewCompleteness, leftCompleteness,
        rightCompleteness, traverseCompleteness, evidenceCompleteness,
        limitCompleteness, abstractIndexVersion, queryWrites, currentGeneration,
        transformState, transformInputComplete, transformOutputPublished,
        transformGeneration, transformConsumed, transformLifecycle, transformReused,
        factPublished, factGeneration, seeded>>

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
        transformLifecycle, transformReused, factPublished, factGeneration, seeded>>

ObserveResult ==
    /\ queryExecuted /\ resultReturned
    /\ UNCHANGED vars

PlanTransform ==
    \E t \in TransformNames :
        /\ Len(trace) = 1
        /\ transformLifecycle[t] = "published" /\ ~transformReused[t]
        /\ transformLifecycle' = [transformLifecycle EXCEPT ![t] = "planned"]
        /\ trace' = Append(trace, "PlanTransform")
    /\ UNCHANGED <<candidatePlan, candidateSteps, leftOperand, rightOperand, fixture,
        planValidated, planRejected, queryExecuted, resultReturned, resultStatus,
        resultItems, resultOrder, resultRelations, witnessPath, sourceCompleteness,
        filterCompleteness, viewCompleteness, leftCompleteness, rightCompleteness,
        traverseCompleteness, evidenceCompleteness, limitCompleteness,
        abstractIndexVersion, queryWrites, currentGeneration, transformState,
        transformInputComplete, transformOutputPublished, transformGeneration,
        transformConsumed, transformReused, factPublished, factGeneration, seeded>>

RunTransform ==
    \E t \in TransformNames :
        /\ ~queryExecuted /\ transformLifecycle[t] = "planned" /\ DependenciesFresh(t)
        /\ transformLifecycle' = [transformLifecycle EXCEPT ![t] = "running"]
        /\ trace' = Append(trace, "RunTransform")
    /\ UNCHANGED <<candidatePlan, candidateSteps, leftOperand, rightOperand, fixture,
        planValidated, planRejected, queryExecuted, resultReturned, resultStatus,
        resultItems, resultOrder, resultRelations, witnessPath, sourceCompleteness,
        filterCompleteness, viewCompleteness, leftCompleteness, rightCompleteness,
        traverseCompleteness, evidenceCompleteness, limitCompleteness,
        abstractIndexVersion, queryWrites, currentGeneration, transformState,
        transformInputComplete, transformOutputPublished, transformGeneration,
        transformConsumed, transformReused, factPublished, factGeneration, seeded>>

PublishTransform ==
    \E t \in TransformNames :
        /\ ~queryExecuted /\ transformLifecycle[t] = "running" /\ transformInputComplete[t]
        /\ DependenciesFresh(t)
        /\ transformLifecycle' = [transformLifecycle EXCEPT ![t] = "published"]
        /\ transformState' = [transformState EXCEPT ![t] = "current"]
        /\ transformOutputPublished' = [transformOutputPublished EXCEPT ![t] = TRUE]
        /\ transformGeneration' = [transformGeneration EXCEPT ![t] = currentGeneration]
        /\ transformConsumed' = [transformConsumed EXCEPT ![t] = FALSE]
        /\ transformReused' = [transformReused EXCEPT ![t] = FALSE]
        /\ factPublished' = [factPublished EXCEPT ![ProducedFact(t)] = TRUE]
        /\ factGeneration' = [factGeneration EXCEPT ![ProducedFact(t)] = currentGeneration]
        /\ trace' = Append(trace, "PublishTransform")
    /\ UNCHANGED <<candidatePlan, candidateSteps, leftOperand, rightOperand, fixture,
        planValidated, planRejected, queryExecuted, resultReturned, resultStatus,
        resultItems, resultOrder, resultRelations, witnessPath, sourceCompleteness,
        filterCompleteness, viewCompleteness, leftCompleteness, rightCompleteness,
        traverseCompleteness, evidenceCompleteness, limitCompleteness,
        abstractIndexVersion, queryWrites, currentGeneration, transformInputComplete,
        transformLifecycle, seeded>>

FailTransform ==
    \E t \in TransformNames :
        /\ ~queryExecuted /\ transformLifecycle[t] = "running"
        /\ transformLifecycle' = IF t = "resolve"
            THEN [transformLifecycle EXCEPT !["resolve"] = "failed", !["answer"] = "stale"]
            ELSE [transformLifecycle EXCEPT ![t] = "failed"]
        /\ transformState' = IF t = "resolve"
            THEN [transformState EXCEPT !["resolve"] = "failed", !["answer"] = "stale"]
            ELSE [transformState EXCEPT ![t] = "failed"]
        /\ transformOutputPublished' = IF t = "resolve"
            THEN [transformOutputPublished EXCEPT !["resolve"] = FALSE, !["answer"] = FALSE]
            ELSE [transformOutputPublished EXCEPT ![t] = FALSE]
        /\ transformConsumed' = IF t = "resolve"
            THEN [transformConsumed EXCEPT !["resolve"] = FALSE, !["answer"] = FALSE]
            ELSE [transformConsumed EXCEPT ![t] = FALSE]
        /\ transformReused' = IF t = "resolve"
            THEN [transformReused EXCEPT !["resolve"] = FALSE, !["answer"] = FALSE]
            ELSE [transformReused EXCEPT ![t] = FALSE]
        /\ factPublished' = IF t = "resolve"
            THEN [factPublished EXCEPT !["resolved-facts-v1"] = FALSE,
                                      !["answer-facts-v1"] = FALSE]
            ELSE [factPublished EXCEPT !["answer-facts-v1"] = FALSE]
        /\ trace' = Append(trace, "FailTransform")
    /\ UNCHANGED <<candidatePlan, candidateSteps, leftOperand, rightOperand, fixture,
        planValidated, planRejected, queryExecuted, resultReturned, resultStatus,
        resultItems, resultOrder, resultRelations, witnessPath, sourceCompleteness,
        filterCompleteness, viewCompleteness, leftCompleteness, rightCompleteness,
        traverseCompleteness, evidenceCompleteness, limitCompleteness,
        abstractIndexVersion, queryWrites, currentGeneration, transformInputComplete,
        transformGeneration, transformLifecycle, transformReused, factGeneration,
        seeded>>

InvalidateTransform ==
    \E t \in TransformNames :
        /\ ~queryExecuted /\ transformLifecycle[t] = "published" /\ ~transformReused[t]
        /\ transformLifecycle' = IF t = "resolve"
            THEN [transformLifecycle EXCEPT !["resolve"] = "stale", !["answer"] = "stale"]
            ELSE [transformLifecycle EXCEPT ![t] = "stale"]
        /\ transformState' = IF t = "resolve"
            THEN [transformState EXCEPT !["resolve"] = "stale", !["answer"] = "stale"]
            ELSE [transformState EXCEPT ![t] = "stale"]
        /\ transformOutputPublished' = IF t = "resolve"
            THEN [transformOutputPublished EXCEPT !["resolve"] = FALSE, !["answer"] = FALSE]
            ELSE [transformOutputPublished EXCEPT ![t] = FALSE]
        /\ transformConsumed' = IF t = "resolve"
            THEN [transformConsumed EXCEPT !["resolve"] = FALSE, !["answer"] = FALSE]
            ELSE [transformConsumed EXCEPT ![t] = FALSE]
        /\ transformReused' = IF t = "resolve"
            THEN [transformReused EXCEPT !["resolve"] = FALSE, !["answer"] = FALSE]
            ELSE [transformReused EXCEPT ![t] = FALSE]
        /\ factPublished' = IF t = "resolve"
            THEN [factPublished EXCEPT !["resolved-facts-v1"] = FALSE,
                                      !["answer-facts-v1"] = FALSE]
            ELSE [factPublished EXCEPT !["answer-facts-v1"] = FALSE]
        /\ trace' = Append(trace, "InvalidateTransform")
    /\ UNCHANGED <<candidatePlan, candidateSteps, leftOperand, rightOperand, fixture,
        planValidated, planRejected, queryExecuted, resultReturned, resultStatus,
        resultItems, resultOrder, resultRelations, witnessPath, sourceCompleteness,
        filterCompleteness, viewCompleteness, leftCompleteness, rightCompleteness,
        traverseCompleteness, evidenceCompleteness, limitCompleteness,
        abstractIndexVersion, queryWrites, currentGeneration, transformInputComplete,
        transformGeneration, transformLifecycle, transformReused, factGeneration,
        seeded>>

ReuseTransform ==
    \E t \in TransformNames :
        /\ ~queryExecuted /\ transformLifecycle[t] = "published"
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
        transformLifecycle, factPublished, factGeneration, seeded>>

SeedPlan(defect) ==
    IF defect = "witness-above-bound"
    THEN PathDepth1Option
    ELSE IF defect \in {"witness-below-bound", "witness-missing-edge"}
    THEN PathDepth2Option
    ELSE IF defect \in {"cycle-loop", "diamond-duplicate", "fanout-dedup", "fanout-complete"}
    THEN ViewNodesOption
    ELSE IF defect \in {"unknown-target-complete", "partial-evidence-complete"}
    THEN ViewRelationsOption
    ELSE SetUnionOption

SeedDefect ==
    /\ Defect \in Defects \ {"none"} /\ ~seeded /\ Len(trace) = 1
    /\ seeded' = TRUE
    /\ candidatePlan' = IF Defect = "witness-above-bound"
                        THEN [PathDepth1Option EXCEPT !.depth = 1]
                        ELSE SeedPlan(Defect)
    /\ candidateSteps' = IF Defect = "illegal-source"
        THEN [candidateSteps EXCEPT ![1].outputView = "relations"]
        ELSE IF Defect = "illegal-view"
        THEN [candidateSteps EXCEPT ![2].outputStream = "path"]
        ELSE IF Defect = "illegal-filter"
        THEN [candidateSteps EXCEPT ![2].outputView = "paths"]
        ELSE IF Defect = "illegal-traverse"
        THEN [candidateSteps EXCEPT ![3].operation = "traverse"]
        ELSE IF Defect = "illegal-set"
        THEN [candidateSteps EXCEPT ![3].leftView = "relations"]
        ELSE IF Defect = "illegal-select"
        THEN [candidateSteps EXCEPT ![4].selection = "relation"]
        ELSE IF Defect = "illegal-order"
        THEN [candidateSteps EXCEPT ![5].outputView = "paths"]
        ELSE IF Defect = "illegal-limit"
        THEN [candidateSteps EXCEPT ![6].outputView = "paths"]
        ELSE IF Defect \in {"witness-below-bound", "witness-above-bound",
                             "witness-missing-edge"}
        THEN SeedPlan(Defect).steps
        ELSE SeedPlan(Defect).steps
    /\ planValidated' = IF Defect \in {"illegal-source", "illegal-view",
        "illegal-filter", "illegal-traverse", "illegal-set", "illegal-select",
        "illegal-order", "illegal-limit", "witness-below-bound", "witness-above-bound",
        "witness-missing-edge", "duplicate-results", "query-write",
        "partial-left", "partial-right", "partial-filter", "partial-view",
        "partial-traverse", "partial-evidence", "partial-transform", "truncated-limit",
        "unknown-target-complete", "partial-evidence-complete", "cycle-loop",
        "diamond-duplicate", "fanout-dedup", "fanout-complete"} THEN TRUE ELSE planValidated
    /\ queryExecuted' = IF Defect \in {"duplicate-results", "query-write",
        "witness-below-bound", "witness-above-bound", "witness-missing-edge",
        "partial-left", "partial-right", "partial-filter", "partial-view",
        "partial-traverse", "partial-evidence", "partial-transform", "truncated-limit",
        "unknown-target-complete", "partial-evidence-complete", "cycle-loop",
        "diamond-duplicate", "fanout-dedup", "fanout-complete"} THEN TRUE ELSE queryExecuted
    /\ resultReturned' = IF Defect \in {"duplicate-results", "witness-below-bound",
        "witness-above-bound", "witness-missing-edge", "partial-left", "partial-right",
        "partial-filter", "partial-view", "partial-traverse", "partial-evidence",
        "partial-transform", "truncated-limit", "unknown-target-complete",
        "partial-evidence-complete", "cycle-loop", "diamond-duplicate", "fanout-dedup",
        "fanout-complete"} THEN TRUE ELSE resultReturned
    /\ resultItems' = IF Defect = "cycle-loop" THEN {"node-1", "node-2"}
        ELSE IF Defect = "diamond-duplicate" THEN {"node-1", "node-2", "node-3"}
        ELSE IF Defect = "fanout-dedup" \/ Defect = "fanout-complete" THEN FanoutTargets
        ELSE IF Defect = "unknown-target-complete" THEN {"record-1", "unknown-target"}
        ELSE IF Defect = "partial-evidence-complete" THEN {"node-1", "node-3"}
        ELSE resultItems
    /\ resultRelations' = IF Defect = "unknown-target-complete" THEN {"rel-3"}
        ELSE IF Defect = "partial-evidence-complete" THEN {"rel-4"}
        ELSE resultRelations
    /\ resultOrder' = IF Defect = "duplicate-results" THEN <<"node-1", "node-1", "node-2", "node-3">>
        ELSE IF Defect = "cycle-loop" THEN <<"node-1", "node-2", "node-1">>
        ELSE IF Defect = "diamond-duplicate" THEN <<"node-1", "node-2", "node-2", "node-3">>
        ELSE IF Defect = "fanout-dedup" THEN <<"node-1", "node-1", "node-2", "node-3">>
        ELSE IF Defect = "witness-below-bound" THEN <<"node-1", "node-2">>
        ELSE IF Defect = "witness-above-bound" THEN <<"node-1", "node-2", "node-3">>
        ELSE IF Defect = "witness-missing-edge" THEN <<"node-1", "node-3">>
        ELSE resultOrder
    /\ witnessPath' = IF Defect = "witness-below-bound" THEN <<PathStart, "node-2">>
        ELSE IF Defect = "witness-above-bound" THEN <<PathStart, "node-2", PathTarget>>
        ELSE IF Defect = "witness-missing-edge" THEN <<PathStart, "unknown-target", PathTarget>>
        ELSE witnessPath
    /\ resultStatus' = IF Defect \in {"duplicate-results", "fanout-dedup"} THEN "partial"
        ELSE IF Defect \in {"cycle-loop", "diamond-duplicate", "partial-left", "partial-right",
        "partial-filter", "partial-view", "partial-traverse", "partial-evidence",
        "partial-transform", "truncated-limit", "unknown-target-complete",
        "partial-evidence-complete", "fanout-complete"} THEN "complete" ELSE resultStatus
    /\ fixture' = IF Defect = "cycle-loop" THEN "cycle"
        ELSE IF Defect = "diamond-duplicate" THEN "diamond"
        ELSE IF Defect = "fanout-dedup" \/ Defect = "fanout-complete" THEN "fanout"
        ELSE "normal"
    /\ leftCompleteness' = IF Defect = "partial-left" THEN "partial" ELSE leftCompleteness
    /\ rightCompleteness' = IF Defect = "partial-right" THEN "partial" ELSE rightCompleteness
    /\ filterCompleteness' = IF Defect = "partial-filter" THEN "partial" ELSE filterCompleteness
    /\ viewCompleteness' = IF Defect = "partial-view" THEN "partial" ELSE viewCompleteness
    /\ traverseCompleteness' = IF Defect = "partial-traverse" THEN "partial" ELSE traverseCompleteness
    /\ evidenceCompleteness' = IF Defect = "partial-evidence" THEN "partial" ELSE evidenceCompleteness
    /\ limitCompleteness' = IF Defect = "truncated-limit" THEN "partial" ELSE limitCompleteness
    /\ queryWrites' = IF Defect = "query-write" THEN 1 ELSE queryWrites
    /\ abstractIndexVersion' = IF Defect = "query-write" THEN InitialIndexVersion + 1 ELSE abstractIndexVersion
    /\ transformState' = IF Defect = "stale-resolve" THEN [transformState EXCEPT !["resolve"] = "stale", !["answer"] = "current"]
        ELSE IF Defect = "failed-resolve" THEN [transformState EXCEPT !["resolve"] = "failed", !["answer"] = "current"]
        ELSE IF Defect = "stale-answer-consumption" THEN [transformState EXCEPT !["answer"] = "stale"]
        ELSE IF Defect = "failed-answer-publication" THEN [transformState EXCEPT !["answer"] = "failed"]
        ELSE transformState
    /\ transformOutputPublished' = IF Defect \in {"stale-resolve", "failed-resolve"}
        THEN [transformOutputPublished EXCEPT !["resolve"] = FALSE, !["answer"] = TRUE]
        ELSE IF Defect = "stale-answer-consumption" THEN [transformOutputPublished EXCEPT !["answer"] = FALSE]
        ELSE IF Defect = "failed-answer-publication" THEN [transformOutputPublished EXCEPT !["answer"] = TRUE]
        ELSE transformOutputPublished
    /\ transformInputComplete' = IF Defect = "partial-transform"
        THEN [transformInputComplete EXCEPT !["answer"] = FALSE]
        ELSE transformInputComplete
    /\ transformConsumed' = IF Defect = "stale-answer-consumption" THEN [transformConsumed EXCEPT !["answer"] = TRUE]
        ELSE transformConsumed
    /\ transformLifecycle' = IF Defect = "stale-resolve" THEN [transformLifecycle EXCEPT !["resolve"] = "stale", !["answer"] = "published"]
        ELSE IF Defect = "failed-resolve" THEN [transformLifecycle EXCEPT !["resolve"] = "failed", !["answer"] = "published"]
        ELSE IF Defect = "stale-answer-consumption" THEN [transformLifecycle EXCEPT !["answer"] = "stale"]
        ELSE IF Defect = "failed-answer-publication" THEN [transformLifecycle EXCEPT !["answer"] = "failed"]
        ELSE transformLifecycle
    /\ factPublished' = IF Defect \in {"stale-resolve", "failed-resolve"}
        THEN [factPublished EXCEPT !["resolved-facts-v1"] = FALSE]
        ELSE IF Defect = "stale-answer-consumption" THEN [factPublished EXCEPT !["answer-facts-v1"] = FALSE]
        ELSE factPublished
    /\ trace' = Append(trace, Defect)
    /\ UNCHANGED <<leftOperand, rightOperand, planRejected,
        sourceCompleteness, currentGeneration, transformGeneration, transformReused,
        factGeneration>>

Next == SelectPlan \/ ValidatePlan \/ SelectFixture \/ ExecuteQuery \/ ReturnResult
    \/ ObserveResult
    \/ PlanTransform \/ RunTransform \/ PublishTransform \/ FailTransform
    \/ InvalidateTransform \/ ReuseTransform \/ SeedDefect
Fairness ==
    /\ WF_vars(ValidatePlan) /\ WF_vars(ExecuteQuery) /\ WF_vars(ReturnResult)
    /\ WF_vars(PlanTransform) /\ WF_vars(RunTransform) /\ WF_vars(PublishTransform)
    /\ WF_vars(FailTransform) /\ WF_vars(InvalidateTransform) /\ WF_vars(ReuseTransform)
    /\ WF_vars(SeedDefect)
Spec == Init /\ [][Next]_SemanticVars /\ Fairness
SemanticLiveness == [](planValidated /\ ~queryExecuted => <> queryExecuted)

=============================================================================
