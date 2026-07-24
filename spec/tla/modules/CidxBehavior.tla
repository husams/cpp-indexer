---------------------------- MODULE CidxBehavior ----------------------------

EXTENDS CidxTypes, CidxProtected, Naturals, Sequences

CONSTANTS
    WorkspaceId,
    RepositoryId,
    TranslationUnitId,
    ConfigurationId,
    GenerationId,
    ArtifactId,
    QueryId,
    RelationId,
    EvidenceId,
    SemanticUniverseId,
    SharedUniverseId,
    TraceBound

VARIABLES
    workspaceState,
    configState,
    indexState,
    publicationState,
    transformState,
    queryState,
    storageState,
    artifactState,
    failureMode,
    generation,
    currentGeneration,
    migrationBaseline,
    invalidated,
    evidenceState,
    queryResultState,
    queryTruncated,
    queryWrites,
    identityMode,
    graphState,
    graphEvidenceState,
    graphTargetState,
    graphConfigurationState,
    graphArgumentOrder,
    queryPlanState,
    queryStreamShape,
    queryTraversalBound,
    queryViewState,
    queryReadOnly,
    includePlanState,
    includeConfigurationState,
    includeEditState,
    destructiveEditAuthorized,
    trace

vars == <<
    workspaceState,
    configState,
    indexState,
    publicationState,
    transformState,
    queryState,
    storageState,
    artifactState,
    failureMode,
    generation,
    currentGeneration,
    migrationBaseline,
    invalidated,
    evidenceState,
    queryResultState,
    queryTruncated,
    queryWrites,
    identityMode,
    graphState,
    graphEvidenceState,
    graphTargetState,
    graphConfigurationState,
    graphArgumentOrder,
    queryPlanState,
    queryStreamShape,
    queryTraversalBound,
    queryViewState,
    queryReadOnly,
    includePlanState,
    includeConfigurationState,
    includeEditState,
    destructiveEditAuthorized,
    trace
>>

TraceAvailable == Len(trace) < TraceBound

ProgressTraceAvailable == Len(trace) + 1 < TraceBound

Init ==
    /\ workspaceState = "empty"
    /\ configState = "uncaptured"
    /\ indexState = "empty"
    /\ publicationState = "none"
    /\ transformState = "idle"
    /\ queryState = "idle"
    /\ storageState = "ready"
    /\ artifactState = "none"
    /\ failureMode = "none"
    /\ generation = 0
    /\ currentGeneration = 0
    /\ migrationBaseline = 0
    /\ invalidated = FALSE
    /\ evidenceState = "complete"
    /\ queryResultState = "none"
    /\ queryTruncated = FALSE
    /\ queryWrites = 0
    /\ identityMode = "separate"
    /\ graphState = "empty"
    /\ graphEvidenceState = "none"
    /\ graphTargetState = "none"
    /\ graphConfigurationState = "unbound"
    /\ graphArgumentOrder = <<>>
    /\ queryPlanState = "absent"
    /\ queryStreamShape = "none"
    /\ queryTraversalBound = 0
    /\ queryViewState = "none"
    /\ queryReadOnly = TRUE
    /\ includePlanState = "none"
    /\ includeConfigurationState = "none"
    /\ includeEditState = "none"
    /\ destructiveEditAuthorized = FALSE
    /\ trace = <<"Init">>

ImportWorkspace ==
    /\ storageState = "ready"
    /\ workspaceState = "empty"
    /\ TraceAvailable
    /\ workspaceState' = "imported"
    /\ trace' = Append(trace, "ImportWorkspace")
    /\ UNCHANGED <<configState, indexState, publicationState, transformState,
                    queryState, storageState, artifactState, failureMode,
                    generation, currentGeneration, migrationBaseline,
                    invalidated, evidenceState, queryResultState,
                    queryTruncated, queryWrites, identityMode, graphState,
                    graphEvidenceState, graphTargetState, graphConfigurationState,
                    graphArgumentOrder, queryPlanState, queryStreamShape,
                    queryTraversalBound, queryViewState, queryReadOnly,
                    includePlanState, includeConfigurationState, includeEditState,
                    destructiveEditAuthorized>>

CaptureConfiguration ==
    /\ storageState = "ready"
    /\ workspaceState = "imported"
    /\ configState = "uncaptured"
    /\ TraceAvailable
    /\ configState' = "captured"
    /\ workspaceState' = "captured"
    /\ trace' = Append(trace, "CaptureConfiguration")
    /\ UNCHANGED <<indexState, publicationState, transformState, queryState,
                    storageState, artifactState, failureMode, generation,
                    currentGeneration, migrationBaseline, invalidated,
                    evidenceState, queryResultState, queryTruncated,
                    queryWrites, identityMode, graphState, graphEvidenceState,
                    graphTargetState, graphConfigurationState, graphArgumentOrder,
                    queryPlanState, queryStreamShape, queryTraversalBound,
                    queryViewState, queryReadOnly, includePlanState,
                    includeConfigurationState, includeEditState,
                    destructiveEditAuthorized>>

StartIndexing ==
    /\ configState = "captured"
    /\ storageState = "ready"
    /\ indexState \in {"empty", "stale", "failed"}
    /\ ProgressTraceAvailable
    /\ indexState' = "indexing"
    /\ failureMode' = "none"
    /\ trace' = Append(trace, "StartIndexing")
    /\ UNCHANGED <<workspaceState, configState, publicationState,
                    transformState, queryState, storageState, artifactState,
                    generation, currentGeneration, migrationBaseline,
                    invalidated, evidenceState, queryResultState,
                    queryTruncated, queryWrites, identityMode, graphState,
                    graphEvidenceState, graphTargetState, graphConfigurationState,
                    graphArgumentOrder, queryPlanState, queryStreamShape,
                    queryTraversalBound, queryViewState, queryReadOnly,
                    includePlanState, includeConfigurationState, includeEditState,
                    destructiveEditAuthorized>>

IndexSuccessfully ==
    /\ indexState = "indexing"
    /\ ProgressTraceAvailable
    /\ indexState' = "indexed"
    /\ publicationState' = "candidate"
    /\ generation' = generation + 1
    /\ failureMode' = "none"
    /\ trace' = Append(trace, "IndexSuccessfully")
    /\ UNCHANGED <<workspaceState, configState, transformState, queryState,
                    storageState, artifactState, currentGeneration,
                    migrationBaseline, invalidated, evidenceState,
                    queryResultState, queryTruncated, queryWrites, identityMode,
                    graphState, graphEvidenceState, graphTargetState,
                    graphConfigurationState, graphArgumentOrder, queryPlanState,
                    queryStreamShape, queryTraversalBound, queryViewState,
                    queryReadOnly, includePlanState, includeConfigurationState,
                    includeEditState, destructiveEditAuthorized>>

IndexFails ==
    /\ indexState = "indexing"
    /\ TraceAvailable
    /\ indexState' = "failed"
    /\ failureMode' = "index-failure"
    /\ trace' = Append(trace, "IndexFails")
    /\ UNCHANGED <<workspaceState, configState, publicationState,
                    transformState, queryState, storageState, artifactState,
                    generation, currentGeneration, migrationBaseline,
                    invalidated, evidenceState, queryResultState,
                    queryTruncated, queryWrites, identityMode, graphState,
                    graphEvidenceState, graphTargetState, graphConfigurationState,
                    graphArgumentOrder, queryPlanState, queryStreamShape,
                    queryTraversalBound, queryViewState, queryReadOnly,
                    includePlanState, includeConfigurationState, includeEditState,
                    destructiveEditAuthorized>>

PublishGeneration ==
    /\ publicationState = "candidate"
    /\ indexState = "indexed"
    /\ storageState = "ready"
    /\ TraceAvailable
    /\ publicationState' = "current"
    /\ indexState' = "current"
    /\ artifactState' = "published"
    /\ currentGeneration' = generation
    /\ invalidated' = FALSE
    /\ graphState' = "published"
    /\ graphEvidenceState' = "owned"
    /\ graphTargetState' \in {"known", "unknown-retained"}
    /\ graphConfigurationState' = "exact"
    /\ graphArgumentOrder' = <<1, 2>>
    /\ queryPlanState' = "absent"
    /\ queryStreamShape' = "none"
    /\ queryTraversalBound' = 0
    /\ queryViewState' = "none"
    /\ queryReadOnly' = TRUE
    /\ includePlanState' = "none"
    /\ includeConfigurationState' = "none"
    /\ includeEditState' = "none"
    /\ destructiveEditAuthorized' = FALSE
    /\ trace' = Append(trace, "PublishGeneration")
    /\ UNCHANGED <<workspaceState, configState, transformState, queryState,
                    storageState, failureMode, generation, migrationBaseline,
                    evidenceState, queryResultState, queryTruncated,
                    queryWrites, identityMode>>

InterruptPublication ==
    /\ publicationState = "candidate"
    /\ indexState = "indexed"
    /\ TraceAvailable
    /\ publicationState' = "stale"
    /\ indexState' = "stale"
    /\ failureMode' = "interrupted"
    /\ graphState' = "empty"
    /\ graphEvidenceState' = "none"
    /\ graphTargetState' = "none"
    /\ graphConfigurationState' = "unbound"
    /\ graphArgumentOrder' = <<>>
    /\ queryPlanState' = "absent"
    /\ queryStreamShape' = "none"
    /\ queryTraversalBound' = 0
    /\ queryViewState' = "none"
    /\ queryReadOnly' = TRUE
    /\ includePlanState' = "none"
    /\ includeConfigurationState' = "none"
    /\ includeEditState' = "none"
    /\ destructiveEditAuthorized' = FALSE
    /\ trace' = Append(trace, "InterruptPublication")
    /\ UNCHANGED <<workspaceState, configState, transformState, queryState,
                    storageState, artifactState, generation, currentGeneration,
                    migrationBaseline, invalidated, evidenceState,
                    queryResultState, queryTruncated, queryWrites,
                    identityMode, graphState, graphEvidenceState,
                    graphTargetState, graphConfigurationState, graphArgumentOrder,
                    queryPlanState, queryStreamShape, queryTraversalBound,
                    queryViewState, queryReadOnly, includePlanState,
                    includeConfigurationState, includeEditState,
                    destructiveEditAuthorized>>

InvalidateGeneration ==
    /\ publicationState = "current"
    /\ indexState = "current"
    /\ storageState = "ready"
    /\ queryState = "idle"
    /\ transformState = "idle"
    /\ TraceAvailable
    /\ publicationState' = "stale"
    /\ indexState' = "stale"
    /\ invalidated' = TRUE
    /\ graphState' = "stale"
    /\ graphConfigurationState' = "stale"
    /\ queryPlanState' = "absent"
    /\ queryStreamShape' = "none"
    /\ queryTraversalBound' = 0
    /\ queryViewState' = "none"
    /\ queryReadOnly' = TRUE
    /\ includePlanState' = "none"
    /\ includeConfigurationState' = "none"
    /\ includeEditState' = "none"
    /\ destructiveEditAuthorized' = FALSE
    /\ trace' = Append(trace, "InvalidateGeneration")
    /\ UNCHANGED <<workspaceState, configState, transformState, queryState,
                    storageState, artifactState, failureMode, generation,
                    currentGeneration, migrationBaseline, evidenceState,
                    queryResultState, queryTruncated, queryWrites, identityMode,
                    graphEvidenceState, graphTargetState, graphArgumentOrder,
                    queryPlanState, queryStreamShape, queryTraversalBound,
                    queryViewState, queryReadOnly, includePlanState,
                    includeConfigurationState, includeEditState,
                    destructiveEditAuthorized>>

PlanTransform ==
    /\ publicationState = "current"
    /\ indexState = "current"
    /\ storageState = "ready"
    /\ queryState = "idle"
    /\ transformState = "idle"
    /\ includePlanState = "none"
    /\ invalidated = FALSE
    /\ artifactState = "published"
    /\ ProgressTraceAvailable
    /\ transformState' = "planned"
    /\ evidenceState' \in EvidenceStates
    /\ destructiveEditAuthorized' = FALSE
    /\ failureMode' = "none"
    /\ trace' = Append(trace, "PlanTransform")
    /\ UNCHANGED <<workspaceState, configState, indexState, publicationState,
                    queryState, storageState, artifactState,
                    generation, currentGeneration, migrationBaseline,
                    invalidated, queryResultState, queryTruncated,
                    queryWrites, identityMode, graphState, graphEvidenceState,
                    graphTargetState, graphConfigurationState, graphArgumentOrder,
                    queryPlanState, queryStreamShape, queryTraversalBound,
                    queryViewState, queryReadOnly, includePlanState,
                    includeConfigurationState, includeEditState>>

ApplyTransform ==
    /\ transformState = "planned"
    /\ evidenceState = "complete"
    /\ TraceAvailable
    /\ transformState' = "applied"
    /\ artifactState' = "derived"
    /\ trace' = Append(trace, "ApplyTransform")
    /\ UNCHANGED <<workspaceState, configState, indexState, publicationState,
                    queryState, storageState, failureMode, generation,
                    currentGeneration, migrationBaseline, invalidated,
                    evidenceState, queryResultState, queryTruncated,
                    queryWrites, identityMode, graphState, graphEvidenceState,
                    graphTargetState, graphConfigurationState, graphArgumentOrder,
                    queryPlanState, queryStreamShape, queryTraversalBound,
                    queryViewState, queryReadOnly, includePlanState,
                    includeConfigurationState, includeEditState,
                    destructiveEditAuthorized>>

RejectIncompleteTransform ==
    /\ transformState = "planned"
    /\ evidenceState = "incomplete"
    /\ TraceAvailable
    /\ transformState' = "failed"
    /\ failureMode' = "incomplete-evidence"
    /\ trace' = Append(trace, "RejectIncompleteTransform")
    /\ UNCHANGED <<workspaceState, configState, indexState, publicationState,
                    queryState, storageState, artifactState, generation,
                    currentGeneration, migrationBaseline, invalidated,
                    evidenceState, queryResultState, queryTruncated,
                    queryWrites, identityMode, graphState, graphEvidenceState,
                    graphTargetState, graphConfigurationState, graphArgumentOrder,
                    queryPlanState, queryStreamShape, queryTraversalBound,
                    queryViewState, queryReadOnly, includePlanState,
                    includeConfigurationState, includeEditState,
                    destructiveEditAuthorized>>

ValidateQueryPlan ==
    /\ publicationState = "current"
    /\ indexState = "current"
    /\ storageState = "ready"
    /\ transformState = "idle"
    /\ includePlanState = "none"
    /\ invalidated = FALSE
    /\ queryState = "idle"
    /\ queryPlanState = "absent"
    /\ TraceAvailable
    /\ queryPlanState' = "validated"
    /\ queryStreamShape' \in {"set", "path"}
    /\ queryTraversalBound' = 1
    /\ queryViewState' = "safe"
    /\ queryReadOnly' = TRUE
    /\ trace' = Append(trace, "ValidateQueryPlan")
    /\ UNCHANGED <<workspaceState, configState, indexState, publicationState,
                    transformState, storageState, artifactState, failureMode,
                    generation, currentGeneration, migrationBaseline,
                    invalidated, evidenceState, queryResultState,
                    queryTruncated, queryWrites, identityMode, graphState,
                    graphEvidenceState, graphTargetState, graphConfigurationState,
                    graphArgumentOrder, queryState, includePlanState,
                    includeConfigurationState, includeEditState,
                    destructiveEditAuthorized>>

StartQuery ==
    /\ publicationState = "current"
    /\ indexState = "current"
    /\ storageState = "ready"
    /\ transformState = "idle"
    /\ includePlanState = "none"
    /\ invalidated = FALSE
    /\ queryState = "idle"
    /\ queryPlanState = "validated"
    /\ ProgressTraceAvailable
    /\ queryState' = "running"
    /\ queryPlanState' = "executing"
    /\ trace' = Append(trace, "StartQuery")
    /\ UNCHANGED <<workspaceState, configState, indexState, publicationState,
                    transformState, storageState, artifactState, failureMode,
                    generation, currentGeneration, migrationBaseline,
                    invalidated, evidenceState, queryResultState,
                    queryTruncated, queryWrites, identityMode, graphState,
                    graphEvidenceState, graphTargetState, graphConfigurationState,
                    graphArgumentOrder, queryStreamShape, queryTraversalBound,
                    queryViewState, queryReadOnly, includePlanState,
                    includeConfigurationState, includeEditState,
                    destructiveEditAuthorized>>

ReturnQuery ==
    /\ queryState = "running"
    /\ queryPlanState = "executing"
    /\ TraceAvailable
    /\ queryState' = "complete"
    /\ queryPlanState' = "returned"
    /\ queryResultState' \in {"complete", "partial", "ambiguous", "unknown"}
    /\ queryTruncated' = (queryResultState' = "partial")
    /\ trace' = Append(trace, "ReturnQuery")
    /\ UNCHANGED <<workspaceState, configState, indexState, publicationState,
                    transformState, storageState, artifactState, failureMode,
                    generation, currentGeneration, migrationBaseline,
                    invalidated, evidenceState, queryWrites, identityMode,
                    graphState, graphEvidenceState, graphTargetState,
                    graphConfigurationState, graphArgumentOrder, queryStreamShape,
                    queryTraversalBound, queryViewState, queryReadOnly,
                    includePlanState, includeConfigurationState, includeEditState,
                    destructiveEditAuthorized>>

BeginMigration ==
    /\ storageState = "ready"
    /\ indexState # "indexing"
    /\ publicationState # "candidate"
    /\ queryState = "idle"
    /\ transformState = "idle"
    /\ includePlanState = "none"
    /\ ProgressTraceAvailable
    /\ storageState' = "migrating"
    /\ migrationBaseline' = currentGeneration
    /\ trace' = Append(trace, "BeginMigration")
    /\ UNCHANGED <<workspaceState, configState, indexState, publicationState,
                    transformState, queryState, artifactState, failureMode,
                    generation, currentGeneration, invalidated, evidenceState,
                    queryResultState, queryTruncated, queryWrites, identityMode,
                    graphState, graphEvidenceState, graphTargetState,
                    graphConfigurationState, graphArgumentOrder, queryPlanState,
                    queryStreamShape, queryTraversalBound, queryViewState,
                    queryReadOnly, includePlanState, includeConfigurationState,
                    includeEditState, destructiveEditAuthorized>>

CompleteMigration ==
    /\ storageState = "migrating"
    /\ TraceAvailable
    /\ storageState' = "ready"
    /\ failureMode' = "none"
    /\ trace' = Append(trace, "CompleteMigration")
    /\ UNCHANGED <<workspaceState, configState, indexState, publicationState,
                    transformState, queryState, artifactState, generation,
                    currentGeneration, migrationBaseline, invalidated,
                    evidenceState, queryResultState, queryTruncated,
                    queryWrites, identityMode, graphState, graphEvidenceState,
                    graphTargetState, graphConfigurationState, graphArgumentOrder,
                    queryPlanState, queryStreamShape, queryTraversalBound,
                    queryViewState, queryReadOnly, includePlanState,
                    includeConfigurationState, includeEditState,
                    destructiveEditAuthorized>>

InterruptMigration ==
    /\ storageState = "migrating"
    /\ ProgressTraceAvailable
    /\ storageState' = "recovery-required"
    /\ failureMode' = "interrupted"
    /\ trace' = Append(trace, "InterruptMigration")
    /\ UNCHANGED <<workspaceState, configState, indexState, publicationState,
                    transformState, queryState, artifactState, generation,
                    currentGeneration, migrationBaseline, invalidated,
                    evidenceState, queryResultState, queryTruncated,
                    queryWrites, identityMode, graphState, graphEvidenceState,
                    graphTargetState, graphConfigurationState, graphArgumentOrder,
                    queryPlanState, queryStreamShape, queryTraversalBound,
                    queryViewState, queryReadOnly, includePlanState,
                    includeConfigurationState, includeEditState,
                    destructiveEditAuthorized>>

RecoverMigration ==
    /\ storageState = "recovery-required"
    /\ TraceAvailable
    /\ storageState' = "ready"
    /\ failureMode' = "none"
    /\ trace' = Append(trace, "RecoverMigration")
    /\ UNCHANGED <<workspaceState, configState, indexState, publicationState,
                    transformState, queryState, artifactState, generation,
                    currentGeneration, migrationBaseline, invalidated,
                    evidenceState, queryResultState, queryTruncated,
                    queryWrites, identityMode, graphState, graphEvidenceState,
                    graphTargetState, graphConfigurationState, graphArgumentOrder,
                    queryPlanState, queryStreamShape, queryTraversalBound,
                    queryViewState, queryReadOnly, includePlanState,
                    includeConfigurationState, includeEditState,
                    destructiveEditAuthorized>>

MergeSymbolsInUniverse ==
    /\ identityMode = "separate"
    /\ SemanticUniverseId = SharedUniverseId
    /\ indexState # "indexing"
    /\ publicationState # "candidate"
    /\ queryState = "idle"
    /\ storageState = "ready"
    /\ transformState = "idle"
    /\ includePlanState = "none"
    /\ TraceAvailable
    /\ identityMode' = "merged"
    /\ trace' = Append(trace, "MergeSymbolsInUniverse")
    /\ UNCHANGED <<workspaceState, configState, indexState, publicationState,
                    transformState, queryState, storageState, artifactState,
                    failureMode, generation, currentGeneration,
                    migrationBaseline, invalidated, evidenceState,
                    queryResultState, queryTruncated, queryWrites, graphState,
                    graphEvidenceState, graphTargetState, graphConfigurationState,
                    graphArgumentOrder, queryPlanState, queryStreamShape,
                    queryTraversalBound, queryViewState, queryReadOnly,
                    includePlanState, includeConfigurationState, includeEditState,
                    destructiveEditAuthorized>>

SeparateSymbolsAcrossUniverses ==
    /\ identityMode = "merged"
    /\ indexState # "indexing"
    /\ publicationState # "candidate"
    /\ queryState = "idle"
    /\ transformState = "idle"
    /\ storageState = "ready"
    /\ includePlanState = "none"
    /\ TraceAvailable
    /\ identityMode' = "separate"
    /\ trace' = Append(trace, "SeparateSymbolsAcrossUniverses")
    /\ UNCHANGED <<workspaceState, configState, indexState, publicationState,
                    transformState, queryState, storageState, artifactState,
                    failureMode, generation, currentGeneration,
                    migrationBaseline, invalidated, evidenceState,
                    queryResultState, queryTruncated, queryWrites, graphState,
                    graphEvidenceState, graphTargetState, graphConfigurationState,
                    graphArgumentOrder, queryPlanState, queryStreamShape,
                    queryTraversalBound, queryViewState, queryReadOnly,
                    includePlanState, includeConfigurationState, includeEditState,
                    destructiveEditAuthorized>>

PlanIncludeChange ==
    /\ publicationState = "current"
    /\ indexState = "current"
    /\ storageState = "ready"
    /\ queryState = "idle"
    /\ transformState = "idle"
    /\ invalidated = FALSE
    /\ includePlanState = "none"
    /\ ProgressTraceAvailable
    /\ includePlanState' = "planned"
    /\ includeConfigurationState' \in {"exact", "mismatch"}
    /\ includeEditState' = "none"
    /\ destructiveEditAuthorized' = FALSE
    /\ trace' = Append(trace, "PlanIncludeChange")
    /\ UNCHANGED <<workspaceState, configState, indexState, publicationState,
                    transformState, queryState, storageState, artifactState,
                    failureMode, generation, currentGeneration,
                    migrationBaseline, invalidated, evidenceState,
                    queryResultState, queryTruncated, queryWrites, identityMode,
                    graphState, graphEvidenceState, graphTargetState,
                    graphConfigurationState, graphArgumentOrder, queryPlanState,
                    queryStreamShape, queryTraversalBound, queryViewState,
                    queryReadOnly>>

ValidateIncludeConfiguration ==
    /\ includePlanState = "planned"
    /\ includeConfigurationState = "exact"
    /\ evidenceState = "complete"
    /\ TraceAvailable
    /\ includePlanState' = "validated"
    /\ destructiveEditAuthorized' = TRUE
    /\ trace' = Append(trace, "ValidateIncludeConfiguration")
    /\ UNCHANGED <<workspaceState, configState, indexState, publicationState,
                    transformState, queryState, storageState, artifactState,
                    failureMode, generation, currentGeneration,
                    migrationBaseline, invalidated, evidenceState,
                    queryResultState, queryTruncated, queryWrites, identityMode,
                    graphState, graphEvidenceState, graphTargetState,
                    graphConfigurationState, graphArgumentOrder, queryPlanState,
                    queryStreamShape, queryTraversalBound, queryViewState,
                    queryReadOnly, includeConfigurationState, includeEditState>>

RejectIncludeConfiguration ==
    /\ includePlanState = "planned"
    /\ includeConfigurationState = "mismatch"
    /\ TraceAvailable
    /\ includePlanState' = "rejected"
    /\ destructiveEditAuthorized' = FALSE
    /\ trace' = Append(trace, "RejectIncludeConfiguration")
    /\ UNCHANGED <<workspaceState, configState, indexState, publicationState,
                    transformState, queryState, storageState, artifactState,
                    failureMode, generation, currentGeneration,
                    migrationBaseline, invalidated, evidenceState,
                    queryResultState, queryTruncated, queryWrites, identityMode,
                    graphState, graphEvidenceState, graphTargetState,
                    graphConfigurationState, graphArgumentOrder, queryPlanState,
                    queryStreamShape, queryTraversalBound, queryViewState,
                    queryReadOnly, includeConfigurationState, includeEditState>>

ApplyIncludeChange ==
    /\ includePlanState = "validated"
    /\ includeConfigurationState = "exact"
    /\ destructiveEditAuthorized = TRUE
    /\ evidenceState = "complete"
    /\ TraceAvailable
    /\ includePlanState' = "applied"
    /\ includeEditState' = "changed"
    /\ destructiveEditAuthorized' = FALSE
    /\ trace' = Append(trace, "ApplyIncludeChange")
    /\ UNCHANGED <<workspaceState, configState, indexState, publicationState,
                    transformState, queryState, storageState, artifactState,
                    failureMode, generation, currentGeneration,
                    migrationBaseline, invalidated, evidenceState,
                    queryResultState, queryTruncated, queryWrites, identityMode,
                    graphState, graphEvidenceState, graphTargetState,
                    graphConfigurationState, graphArgumentOrder, queryPlanState,
                    queryStreamShape, queryTraversalBound, queryViewState,
                    queryReadOnly, includeConfigurationState>>

NoOp == UNCHANGED vars

Advance == ImportWorkspace \/ CaptureConfiguration \/ StartIndexing
    \/ IndexSuccessfully \/ IndexFails \/ PublishGeneration
    \/ InterruptPublication \/ InvalidateGeneration \/ PlanTransform
    \/ ApplyTransform \/ RejectIncompleteTransform \/ ValidateQueryPlan
    \/ StartQuery \/ ReturnQuery
    \/ BeginMigration \/ CompleteMigration \/ InterruptMigration
    \/ RecoverMigration \/ MergeSymbolsInUniverse
    \/ SeparateSymbolsAcrossUniverses \/ PlanIncludeChange
    \/ ValidateIncludeConfiguration \/ RejectIncludeConfiguration
    \/ ApplyIncludeChange

Next == Advance \/ NoOp

Fairness ==
    /\ WF_vars(IndexSuccessfully)
    /\ WF_vars(IndexFails)
    /\ WF_vars(PublishGeneration)
    /\ WF_vars(ReturnQuery)
    /\ WF_vars(CompleteMigration)
    /\ WF_vars(RecoverMigration)
    /\ WF_vars(ApplyTransform)
    /\ WF_vars(RejectIncompleteTransform)
    /\ WF_vars(ValidateIncludeConfiguration)
    /\ WF_vars(RejectIncludeConfiguration)

Spec == Init /\ [][Next]_vars /\ Fairness

IndexingLiveness ==
    [](indexState = "indexing" =>
        <> (indexState \in {"indexed", "current", "failed", "stale"}))

PublicationLiveness ==
    [](publicationState = "candidate" =>
        <> (publicationState \in {"current", "failed", "stale"}))

QueryLiveness ==
    [](queryState = "running" => <> (queryState = "complete"))

RecoveryLiveness ==
    [](storageState = "recovery-required" =>
        <> (storageState = "ready"))

TransformLiveness ==
    [](transformState = "planned" =>
        <> (transformState \in {"applied", "failed"}))

IncludePlanLiveness ==
    [](includePlanState = "planned" =>
        <> (includePlanState \in {"validated", "rejected"}))

TypeInvariant ==
    /\ workspaceState \in WorkspaceStates
    /\ configState \in ConfigurationStates
    /\ indexState \in IndexStates
    /\ publicationState \in PublicationStates
    /\ transformState \in TransformStates
    /\ queryState \in QueryStates
    /\ storageState \in StorageStates
    /\ artifactState \in ArtifactStates
    /\ failureMode \in FailureModes
    /\ evidenceState \in EvidenceStates
    /\ queryResultState \in QueryResultStates
    /\ identityMode \in IdentityModes
    /\ graphState \in GraphStates
    /\ graphEvidenceState \in GraphEvidenceStates
    /\ graphTargetState \in GraphTargetStates
    /\ graphConfigurationState \in GraphConfigurationStates
    /\ graphArgumentOrder \in Seq(Nat)
    /\ queryPlanState \in QueryPlanStates
    /\ queryStreamShape \in QueryStreamShapes
    /\ queryTraversalBound \in Nat
    /\ queryViewState \in QueryViewStates
    /\ queryReadOnly \in BOOLEAN
    /\ includePlanState \in IncludePlanStates
    /\ includeConfigurationState \in IncludeConfigurationStates
    /\ includeEditState \in IncludeEditStates
    /\ destructiveEditAuthorized \in BOOLEAN
    /\ generation \in Nat
    /\ currentGeneration \in Nat
    /\ migrationBaseline \in Nat
    /\ currentGeneration <= generation
    /\ queryWrites = 0
    /\ queryTraversalBound <= TraceBound
    /\ Len(trace) <= TraceBound
    /\ WorkspaceId \in WorkspaceIds
    /\ RepositoryId \in RepositoryIds
    /\ TranslationUnitId \in TranslationUnitIds
    /\ ConfigurationId \in ConfigurationIds
    /\ GenerationId \in GenerationIds
    /\ ArtifactId \in ArtifactIds
    /\ QueryId \in QueryIds
    /\ RelationId \in RelationIds
    /\ EvidenceId \in EvidenceIds

NoPartialGenerationInvariant ==
    NoPartialPublication(indexState, publicationState, artifactState,
                         currentGeneration)

AtomicPublicationInvariant ==
    publicationState = "current"
        => currentGeneration = generation

InvalidationInvariant ==
    invalidated => publicationState # "current" /\ indexState # "current"

FailureHonestyInvariant ==
    /\ failureMode = "index-failure" => indexState = "failed"
    /\ failureMode = "incomplete-evidence" => transformState = "failed"
    /\ failureMode = "interrupted"
        => /\ storageState = "recovery-required"
           \/ publicationState = "stale"

QueryHonestyInvariant ==
    /\ HonestPartialResults(queryResultState, queryTruncated)
    /\ ReadOnlyQueries(queryWrites)
    /\ queryResultState = "ambiguous" => queryState = "complete"
    /\ queryResultState = "unknown" => queryState = "complete"

MigrationInvariant ==
    PreservePublishedGeneration(storageState, currentGeneration,
                                migrationBaseline)

IdentityInvariant ==
    identityMode = "merged" => SemanticUniverseId = SharedUniverseId

GraphInvariant ==
    /\ graphState = "published"
        => /\ graphEvidenceState = "owned"
           /\ graphTargetState \in {"known", "unknown-retained"}
           /\ graphConfigurationState = "exact"
           /\ graphArgumentOrder = <<1, 2>>
    /\ graphTargetState = "unknown-retained"
        => graphState \in {"published", "stale"}

QueryPlanInvariant ==
    /\ queryPlanState = "absent"
        => /\ queryStreamShape = "none"
           /\ queryTraversalBound = 0
           /\ queryViewState = "none"
    /\ queryPlanState \in {"validated", "executing", "returned"}
        => /\ queryStreamShape \in {"set", "path"}
           /\ queryTraversalBound > 0
           /\ queryViewState = "safe"
           /\ queryReadOnly
    /\ queryState = "complete" => queryPlanState = "returned"

IncludeHygieneInvariant ==
    /\ destructiveEditAuthorized
        => /\ includePlanState = "validated"
           /\ includeConfigurationState = "exact"
           /\ evidenceState = "complete"
    /\ includeEditState = "changed"
        => /\ includePlanState = "applied"
           /\ ~destructiveEditAuthorized
    /\ evidenceState = "incomplete" => ~destructiveEditAuthorized

TraceInvariant ==
    /\ Len(trace) > 0
    /\ Head(trace) = "Init"

BoundedProgressInvariant ==
    Len(trace) = TraceBound
        => /\ indexState # "indexing"
           /\ publicationState # "candidate"
           /\ queryState # "running"
           /\ storageState # "recovery-required"
           /\ transformState # "planned"
           /\ includePlanState # "planned"

ProtectedInvariant ==
    /\ NoPartialGenerationInvariant
    /\ QueryHonestyInvariant
    /\ MigrationInvariant
    /\ GraphInvariant
    /\ QueryPlanInvariant
    /\ IncludeHygieneInvariant
    /\ BoundedProgressInvariant

=============================================================================
