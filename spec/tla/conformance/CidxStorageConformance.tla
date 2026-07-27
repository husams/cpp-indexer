--------------------- MODULE CidxStorageConformance ---------------------
(***************************************************************************)
(* Deterministic conformance replay for the sidecar-publication flow named  *)
(* in HSE-89's acceptance criteria (alongside index-generation publication  *)
(* and QueryPlan read-only execution, both already covered by               *)
(* conformance/CidxConformance.tla over CidxBehavior).  This module         *)
(* replays a fixed seven-action trace against the real CidxStorageLifecycle *)
(* actions -- StartOneTUUpdate, BuildSidecar, PrepareStagedArtifact,        *)
(* PrepareSidecarArtifact, ValidateStagedArtifact, PublishCoreGeneration,   *)
(* PublishSidecar (or a MarkSidecarMissing/MarkSidecarCorrupt substitute    *)
(* for the negative scenarios) -- so an impossible sidecar-publication      *)
(* order or a mismatched final observation fails the same way an impossible *)
(* CidxBehavior trace does in CidxConformance.tla.                         *)
(***************************************************************************)
EXTENDS CidxStorageLifecycle

CONSTANTS
    ScenarioAction1,
    ScenarioAction2,
    ScenarioAction3,
    ScenarioAction4,
    ScenarioAction5,
    ScenarioAction6,
    ScenarioAction7,
    ExpectedStagedArtifactQuality,
    ExpectedSidecarQuality,
    ExpectedSidecarState,
    ExpectedSidecarValidated,
    ExpectedSidecarAttachment,
    ExpectedSidecarFilePublication,
    ExpectedReadCompleteness,
    ExpectedCurrentGeneration

ScenarioLength == 7

ScenarioActions ==
    <<ScenarioAction1, ScenarioAction2, ScenarioAction3, ScenarioAction4,
      ScenarioAction5, ScenarioAction6, ScenarioAction7>>

VARIABLE step

replayVars == vars \o <<step>>

ReplayInit ==
    /\ Init
    /\ step = 0

(* Constrain the two actions whose real specification is nondeterministic  *)
(* (\E quality \in ArtifactQualities) to the concrete quality the replayed  *)
(* trace actually observed, exactly as CidxConformance.tla constrains       *)
(* PlanTransform/ValidateQueryPlan/PublishGeneration. *)
ReplayPrepareStagedArtifact ==
    /\ PrepareStagedArtifact
    /\ stagedArtifactQuality' = ExpectedStagedArtifactQuality

ReplayPrepareSidecarArtifact ==
    /\ PrepareSidecarArtifact
    /\ sidecarQuality' = ExpectedSidecarQuality

ReplayAction(name) ==
    IF name = "StartOneTUUpdate" THEN StartOneTUUpdate
    ELSE IF name = "BuildSidecar" THEN BuildSidecar
    ELSE IF name = "PrepareStagedArtifact" THEN ReplayPrepareStagedArtifact
    ELSE IF name = "PrepareSidecarArtifact" THEN ReplayPrepareSidecarArtifact
    ELSE IF name = "ValidateStagedArtifact" THEN ValidateStagedArtifact
    ELSE IF name = "PublishCoreGeneration" THEN PublishCoreGeneration
    ELSE IF name = "PublishSidecar" THEN PublishSidecar
    ELSE IF name = "MarkSidecarMissing" THEN MarkSidecarMissing
    ELSE IF name = "MarkSidecarCorrupt" THEN MarkSidecarCorrupt
    ELSE FALSE

ReplayStep ==
    /\ step < ScenarioLength
    /\ ReplayAction(ScenarioActions[step + 1])
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
        /\ sidecarState = ExpectedSidecarState
        /\ sidecarQuality = ExpectedSidecarQuality
        /\ sidecarValidated = ExpectedSidecarValidated
        /\ sidecarAttachment = ExpectedSidecarAttachment
        /\ sidecarFilePublication = ExpectedSidecarFilePublication
        /\ readCompleteness = ExpectedReadCompleteness
        /\ currentGeneration = ExpectedCurrentGeneration)

=============================================================================
