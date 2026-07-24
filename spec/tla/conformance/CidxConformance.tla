------------------------- MODULE CidxConformance --------------------------

EXTENDS CidxBehavior

CONSTANTS
    ScenarioAction1,
    ScenarioAction2,
    ScenarioAction3,
    ScenarioAction4,
    ScenarioAction5,
    ScenarioAction6,
    ScenarioAction7,
    ScenarioAction8,
    ScenarioLength,
    ExpectedWorkspaceState,
    ExpectedConfigState,
    ExpectedIndexState,
    ExpectedPublicationState,
    ExpectedTransformState,
    ExpectedQueryState,
    ExpectedStorageState,
    ExpectedArtifactState,
    ExpectedFailureMode,
    ExpectedGeneration,
    ExpectedCurrentGeneration,
    ExpectedMigrationBaseline,
    ExpectedInvalidated,
    ExpectedEvidenceState,
    ExpectedQueryResultState,
    ExpectedQueryTruncated,
    ExpectedQueryWrites,
    ExpectedIdentityMode,
    ExpectedGraphState,
    ExpectedGraphEvidenceState,
    ExpectedGraphTargetState,
    ExpectedGraphConfigurationState,
    ExpectedQueryPlanState,
    ExpectedQueryStreamShape,
    ExpectedQueryTraversalBound,
    ExpectedQueryViewState,
    ExpectedIncludePlanState,
    ExpectedIncludeConfigurationState,
    ExpectedIncludeEditState,
    ExpectedDestructiveEditAuthorized

ScenarioActions ==
    <<ScenarioAction1, ScenarioAction2, ScenarioAction3, ScenarioAction4,
      ScenarioAction5, ScenarioAction6, ScenarioAction7, ScenarioAction8>>

VARIABLE step

replayVars == vars \o <<step>>

ReplayInit ==
    /\ Init
    /\ step = 0

ReplayPlanTransform ==
    /\ PlanTransform
    /\ evidenceState' = ExpectedEvidenceState

ReplayValidateQueryPlan ==
    /\ ValidateQueryPlan
    /\ queryStreamShape' = ExpectedQueryStreamShape

ReplayPlanIncludeChange ==
    /\ PlanIncludeChange
    /\ includeConfigurationState' = ExpectedIncludeConfigurationState

ReplayPublishGeneration ==
    /\ PublishGeneration
    /\ graphTargetState' = ExpectedGraphTargetState

ReplayReturnQuery == ReturnQuery

ReplayAction(name) ==
    IF name = "ImportWorkspace" THEN ImportWorkspace
    ELSE IF name = "CaptureConfiguration" THEN CaptureConfiguration
    ELSE IF name = "StartIndexing" THEN StartIndexing
    ELSE IF name = "IndexSuccessfully" THEN IndexSuccessfully
    ELSE IF name = "IndexFails" THEN IndexFails
    ELSE IF name = "PublishGeneration" THEN ReplayPublishGeneration
    ELSE IF name = "InterruptPublication" THEN InterruptPublication
    ELSE IF name = "InvalidateGeneration" THEN InvalidateGeneration
    ELSE IF name = "PlanTransform" THEN ReplayPlanTransform
    ELSE IF name = "ApplyTransform" THEN ApplyTransform
    ELSE IF name = "RejectIncompleteTransform" THEN RejectIncompleteTransform
    ELSE IF name = "ValidateQueryPlan" THEN ReplayValidateQueryPlan
    ELSE IF name = "StartQuery" THEN StartQuery
    ELSE IF name = "ReturnQuery" THEN ReplayReturnQuery
    ELSE IF name = "BeginMigration" THEN BeginMigration
    ELSE IF name = "CompleteMigration" THEN CompleteMigration
    ELSE IF name = "InterruptMigration" THEN InterruptMigration
    ELSE IF name = "RecoverMigration" THEN RecoverMigration
    ELSE IF name = "MergeSymbolsInUniverse" THEN MergeSymbolsInUniverse
    ELSE IF name = "SeparateSymbolsAcrossUniverses" THEN SeparateSymbolsAcrossUniverses
    ELSE IF name = "PlanIncludeChange" THEN ReplayPlanIncludeChange
    ELSE IF name = "ValidateIncludeConfiguration" THEN ValidateIncludeConfiguration
    ELSE IF name = "RejectIncludeConfiguration" THEN RejectIncludeConfiguration
    ELSE IF name = "ApplyIncludeChange" THEN ApplyIncludeChange
    ELSE FALSE

ReplayOutcomeConstraintNext ==
    \/ queryResultState' = "none"
    \/ (queryResultState' = ExpectedQueryResultState
        /\ queryTruncated' = ExpectedQueryTruncated)

ReplayStep ==
    /\ step < ScenarioLength
    /\ ReplayAction(ScenarioActions[step + 1])
    /\ ReplayOutcomeConstraintNext
    /\ step' = step + 1

ReplayEnd ==
    /\ step = ScenarioLength
    /\ UNCHANGED vars
    /\ UNCHANGED step

ReplayNext == ReplayStep \/ ReplayEnd

ReplaySpec == ReplayInit /\ [][ReplayNext]_replayVars
    /\ WF_replayVars(ReplayStep)

ReplayCompletion == <> (step = ScenarioLength)

ReplayTraceInvariant == Len(trace) = step + 1

ReplayObservationInvariant ==
    /\ step \in 0..ScenarioLength
    /\ (step = ScenarioLength =>
        /\ workspaceState = ExpectedWorkspaceState
        /\ configState = ExpectedConfigState
        /\ indexState = ExpectedIndexState
        /\ publicationState = ExpectedPublicationState
        /\ transformState = ExpectedTransformState
        /\ queryState = ExpectedQueryState
        /\ storageState = ExpectedStorageState
        /\ artifactState = ExpectedArtifactState
        /\ failureMode = ExpectedFailureMode
        /\ generation = ExpectedGeneration
        /\ currentGeneration = ExpectedCurrentGeneration
        /\ migrationBaseline = ExpectedMigrationBaseline
        /\ invalidated = ExpectedInvalidated
        /\ evidenceState = ExpectedEvidenceState
        /\ queryResultState = ExpectedQueryResultState
        /\ queryTruncated = ExpectedQueryTruncated
        /\ queryWrites = ExpectedQueryWrites
        /\ identityMode = ExpectedIdentityMode
        /\ graphState = ExpectedGraphState
        /\ graphEvidenceState = ExpectedGraphEvidenceState
        /\ graphTargetState = ExpectedGraphTargetState
        /\ graphConfigurationState = ExpectedGraphConfigurationState
        /\ queryPlanState = ExpectedQueryPlanState
        /\ queryStreamShape = ExpectedQueryStreamShape
        /\ queryTraversalBound = ExpectedQueryTraversalBound
        /\ queryViewState = ExpectedQueryViewState
        /\ includePlanState = ExpectedIncludePlanState
        /\ includeConfigurationState = ExpectedIncludeConfigurationState
        /\ includeEditState = ExpectedIncludeEditState
        /\ destructiveEditAuthorized = ExpectedDestructiveEditAuthorized)

=============================================================================
