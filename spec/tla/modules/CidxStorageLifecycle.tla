-------------------------- MODULE CidxStorageLifecycle --------------------------

EXTENDS CidxTypes, Naturals, Sequences

(***************************************************************************)
(* This module specifies logical storage behavior, not SQLite pager or       *)
(* filesystem behavior.  Core publication and sidecar publication are      *)
(* separate transitions because no cross-file transaction is assumed.       *)
(***************************************************************************)

CONSTANTS
    WorkspaceId,
    CoreSchemaVersion,
    SupportedSchemaVersion,
    NewerSchemaVersion,
    IncompatibleSchemaVersion,
    InitialGeneration,
    ReplacementGeneration,
    SecondReplacementGeneration,
    TraceBound,
    Scenario

VARIABLES
    currentGeneration,
    stagedGeneration,
    generationState,
    currentArtifactQuality,
    stagedArtifactQuality,
    stagedValidated,
    corePublicationState,
    extractedGeneration,
    derivedGeneration,
    coreSchema,
    migrationPhase,
    migrationSourceSchema,
    migrationTargetSchema,
    preMigrationGeneration,
    migrationRecovery,
    readerStatus,
    readerCompatibility,
    sidecarGeneration,
    sidecarState,
    sidecarQuality,
    sidecarValidated,
    sidecarAttachment,
    coreFilePublication,
    sidecarFilePublication,
    readCompleteness,
    crossFileAtomicityAssumed,
    retiredGenerations,
    leasedGenerations,
    replayPinnedGenerations,
    removedGenerations,
    packageState,
    packageCoreComplete,
    packageSidecarComplete,
    packageImported,
    includePlanState,
    includeEvidence,
    includeEditState,
    readOnlyWrites,
    trace

vars == <<
    currentGeneration,
    stagedGeneration,
    generationState,
    currentArtifactQuality,
    stagedArtifactQuality,
    stagedValidated,
    corePublicationState,
    extractedGeneration,
    derivedGeneration,
    coreSchema,
    migrationPhase,
    migrationSourceSchema,
    migrationTargetSchema,
    preMigrationGeneration,
    migrationRecovery,
    readerStatus,
    readerCompatibility,
    sidecarGeneration,
    sidecarState,
    sidecarQuality,
    sidecarValidated,
    sidecarAttachment,
    coreFilePublication,
    sidecarFilePublication,
    readCompleteness,
    crossFileAtomicityAssumed,
    retiredGenerations,
    leasedGenerations,
    replayPinnedGenerations,
    removedGenerations,
    packageState,
    packageCoreComplete,
    packageSidecarComplete,
    packageImported,
    includePlanState,
    includeEvidence,
    includeEditState,
    readOnlyWrites,
    trace
>>

GenerationStates == {"current", "extracting", "validated", "failed"}
ArtifactQualities == {"valid", "partial", "corrupt", "incompatible"}
CorePublicationStates == {"current", "staging", "stale", "unavailable"}
MigrationPhases == {
    "none", "staging", "validated", "committed", "rejected", "recovery-required"
}
MigrationRecoveryStates == {
    "none", "snapshot-retained", "interrupted", "recovered-previous"
}
ReaderCompatibilityStates == {"supported", "newer", "incompatible"}
SidecarStates == {
    "absent", "building", "validated", "current", "stale",
    "missing", "corrupt", "incompatible"
}
SidecarAttachments == {"detached", "attached"}
FilePublicationStates == {"none", "current"}
ReadCompletenessStates == {"complete", "partial", "unknown", "unavailable"}
RetiredStates == {"none", "stale", "retired"}
CleanupStates == {"kept", "removed"}
PackageStates == {"none", "building", "complete", "partial", "rejected", "imported"}
IncludeEvidenceStates == {"none", "complete", "incomplete"}

TraceAvailable == Len(trace) + 1 < TraceBound
ProgressTraceAvailable == Len(trace) + 1 < TraceBound

SidecarCompleteFor(g) ==
    /\ sidecarState = "current"
    /\ sidecarValidated
    /\ sidecarQuality = "valid"
    /\ sidecarAttachment = "attached"
    /\ sidecarFilePublication = "current"
    /\ sidecarGeneration = g

InitialState ==
    [currentGeneration |-> InitialGeneration,
     stagedGeneration |-> 0,
     generationState |-> "current",
     currentArtifactQuality |-> "valid",
     stagedArtifactQuality |-> "valid",
     stagedValidated |-> FALSE,
     corePublicationState |-> "current",
     extractedGeneration |-> InitialGeneration,
     derivedGeneration |-> InitialGeneration,
     coreSchema |-> CoreSchemaVersion,
     migrationPhase |-> "none",
     migrationSourceSchema |-> CoreSchemaVersion,
     migrationTargetSchema |-> CoreSchemaVersion,
     preMigrationGeneration |-> InitialGeneration,
     migrationRecovery |-> "none",
     readerStatus |-> "current",
     readerCompatibility |-> "supported",
     sidecarGeneration |-> 0,
     sidecarState |-> "absent",
     sidecarQuality |-> "valid",
     sidecarValidated |-> FALSE,
     sidecarAttachment |-> "detached",
     coreFilePublication |-> "current",
     sidecarFilePublication |-> "none",
     readCompleteness |-> "partial",
     crossFileAtomicityAssumed |-> FALSE,
     retiredGenerations |-> {},
     leasedGenerations |-> {},
     replayPinnedGenerations |-> {},
     removedGenerations |-> {},
     packageState |-> "none",
     packageCoreComplete |-> FALSE,
     packageSidecarComplete |-> FALSE,
     packageImported |-> FALSE,
     includePlanState |-> "none",
     includeEvidence |-> "none",
     includeEditState |-> "none",
     readOnlyWrites |-> 0,
     trace |-> <<"Init">>]

StateEquals(state) ==
    /\ currentGeneration = state.currentGeneration
    /\ stagedGeneration = state.stagedGeneration
    /\ generationState = state.generationState
    /\ currentArtifactQuality = state.currentArtifactQuality
    /\ stagedArtifactQuality = state.stagedArtifactQuality
    /\ stagedValidated = state.stagedValidated
    /\ corePublicationState = state.corePublicationState
    /\ extractedGeneration = state.extractedGeneration
    /\ derivedGeneration = state.derivedGeneration
    /\ coreSchema = state.coreSchema
    /\ migrationPhase = state.migrationPhase
    /\ migrationSourceSchema = state.migrationSourceSchema
    /\ migrationTargetSchema = state.migrationTargetSchema
    /\ preMigrationGeneration = state.preMigrationGeneration
    /\ migrationRecovery = state.migrationRecovery
    /\ readerStatus = state.readerStatus
    /\ readerCompatibility = state.readerCompatibility
    /\ sidecarGeneration = state.sidecarGeneration
    /\ sidecarState = state.sidecarState
    /\ sidecarQuality = state.sidecarQuality
    /\ sidecarValidated = state.sidecarValidated
    /\ sidecarAttachment = state.sidecarAttachment
    /\ coreFilePublication = state.coreFilePublication
    /\ sidecarFilePublication = state.sidecarFilePublication
    /\ readCompleteness = state.readCompleteness
    /\ crossFileAtomicityAssumed = state.crossFileAtomicityAssumed
    /\ retiredGenerations = state.retiredGenerations
    /\ leasedGenerations = state.leasedGenerations
    /\ replayPinnedGenerations = state.replayPinnedGenerations
    /\ removedGenerations = state.removedGenerations
    /\ packageState = state.packageState
    /\ packageCoreComplete = state.packageCoreComplete
    /\ packageSidecarComplete = state.packageSidecarComplete
    /\ packageImported = state.packageImported
    /\ includePlanState = state.includePlanState
    /\ includeEvidence = state.includeEvidence
    /\ includeEditState = state.includeEditState
    /\ readOnlyWrites = state.readOnlyWrites
    /\ trace = state.trace

ValidInitialState == StateEquals(InitialState)

CrossFileAtomicitySeed ==
    [currentGeneration |-> ReplacementGeneration,
     stagedGeneration |-> 0,
     generationState |-> "current",
     currentArtifactQuality |-> "valid",
     stagedArtifactQuality |-> "valid",
     stagedValidated |-> FALSE,
     corePublicationState |-> "current",
     extractedGeneration |-> ReplacementGeneration,
     derivedGeneration |-> ReplacementGeneration,
     coreSchema |-> CoreSchemaVersion,
     migrationPhase |-> "none",
     migrationSourceSchema |-> CoreSchemaVersion,
     migrationTargetSchema |-> CoreSchemaVersion,
     preMigrationGeneration |-> ReplacementGeneration,
     migrationRecovery |-> "none",
     readerStatus |-> "current",
     readerCompatibility |-> "supported",
     sidecarGeneration |-> InitialGeneration,
     sidecarState |-> "current",
     sidecarQuality |-> "valid",
     sidecarValidated |-> TRUE,
     sidecarAttachment |-> "detached",
     coreFilePublication |-> "current",
     sidecarFilePublication |-> "current",
     readCompleteness |-> "partial",
     crossFileAtomicityAssumed |-> TRUE,
     retiredGenerations |-> {InitialGeneration},
     leasedGenerations |-> {},
     replayPinnedGenerations |-> {},
     removedGenerations |-> {},
     packageState |-> "none",
     packageCoreComplete |-> FALSE,
     packageSidecarComplete |-> FALSE,
     packageImported |-> FALSE,
     includePlanState |-> "none",
     includeEvidence |-> "none",
     includeEditState |-> "none",
     readOnlyWrites |-> 0,
     trace |-> <<"Init">>]

NewerReaderSeed ==
    [InitialState EXCEPT
        !.coreSchema = NewerSchemaVersion,
        !.readerStatus = "unavailable",
        !.readerCompatibility = "newer",
        !.readCompleteness = "unavailable"]

IncompatibleReaderSeed ==
    [InitialState EXCEPT
        !.coreSchema = IncompatibleSchemaVersion,
        !.readerStatus = "unavailable",
        !.readerCompatibility = "incompatible",
        !.readCompleteness = "unavailable"]

MultiGenerationSeed ==
    [InitialState EXCEPT
        !.currentGeneration = ReplacementGeneration,
        !.extractedGeneration = ReplacementGeneration,
        !.derivedGeneration = ReplacementGeneration,
        !.preMigrationGeneration = ReplacementGeneration,
        !.sidecarGeneration = ReplacementGeneration,
        !.sidecarState = "current",
        !.sidecarValidated = TRUE,
        !.sidecarAttachment = "attached",
        !.sidecarFilePublication = "current",
        !.readCompleteness = "complete",
        !.retiredGenerations = {InitialGeneration},
        !.trace = <<"Init", "SeedFirstReplacement">>]

Init ==
    IF Scenario = "valid"
    THEN ValidInitialState
    ELSE IF Scenario = "multi-generation"
    THEN StateEquals(MultiGenerationSeed)
    ELSE IF Scenario = "cross-file-atomicity"
    THEN
        /\ currentGeneration = CrossFileAtomicitySeed.currentGeneration
        /\ stagedGeneration = CrossFileAtomicitySeed.stagedGeneration
        /\ generationState = CrossFileAtomicitySeed.generationState
        /\ currentArtifactQuality = CrossFileAtomicitySeed.currentArtifactQuality
        /\ stagedArtifactQuality = CrossFileAtomicitySeed.stagedArtifactQuality
        /\ stagedValidated = CrossFileAtomicitySeed.stagedValidated
        /\ corePublicationState = CrossFileAtomicitySeed.corePublicationState
        /\ extractedGeneration = CrossFileAtomicitySeed.extractedGeneration
        /\ derivedGeneration = CrossFileAtomicitySeed.derivedGeneration
        /\ coreSchema = CrossFileAtomicitySeed.coreSchema
        /\ migrationPhase = CrossFileAtomicitySeed.migrationPhase
        /\ migrationSourceSchema = CrossFileAtomicitySeed.migrationSourceSchema
        /\ migrationTargetSchema = CrossFileAtomicitySeed.migrationTargetSchema
        /\ preMigrationGeneration = CrossFileAtomicitySeed.preMigrationGeneration
        /\ migrationRecovery = CrossFileAtomicitySeed.migrationRecovery
        /\ readerStatus = CrossFileAtomicitySeed.readerStatus
        /\ readerCompatibility = CrossFileAtomicitySeed.readerCompatibility
        /\ sidecarGeneration = CrossFileAtomicitySeed.sidecarGeneration
        /\ sidecarState = CrossFileAtomicitySeed.sidecarState
        /\ sidecarQuality = CrossFileAtomicitySeed.sidecarQuality
        /\ sidecarValidated = CrossFileAtomicitySeed.sidecarValidated
        /\ sidecarAttachment = CrossFileAtomicitySeed.sidecarAttachment
        /\ coreFilePublication = CrossFileAtomicitySeed.coreFilePublication
        /\ sidecarFilePublication = CrossFileAtomicitySeed.sidecarFilePublication
        /\ readCompleteness = CrossFileAtomicitySeed.readCompleteness
        /\ crossFileAtomicityAssumed = CrossFileAtomicitySeed.crossFileAtomicityAssumed
        /\ retiredGenerations = CrossFileAtomicitySeed.retiredGenerations
        /\ leasedGenerations = CrossFileAtomicitySeed.leasedGenerations
        /\ replayPinnedGenerations = CrossFileAtomicitySeed.replayPinnedGenerations
        /\ removedGenerations = CrossFileAtomicitySeed.removedGenerations
        /\ packageState = CrossFileAtomicitySeed.packageState
        /\ packageCoreComplete = CrossFileAtomicitySeed.packageCoreComplete
        /\ packageSidecarComplete = CrossFileAtomicitySeed.packageSidecarComplete
        /\ packageImported = CrossFileAtomicitySeed.packageImported
        /\ includePlanState = CrossFileAtomicitySeed.includePlanState
        /\ includeEvidence = CrossFileAtomicitySeed.includeEvidence
        /\ includeEditState = CrossFileAtomicitySeed.includeEditState
        /\ readOnlyWrites = CrossFileAtomicitySeed.readOnlyWrites
        /\ trace = CrossFileAtomicitySeed.trace
    ELSE IF Scenario = "newer-reader"
    THEN
        /\ currentGeneration = NewerReaderSeed.currentGeneration
        /\ stagedGeneration = NewerReaderSeed.stagedGeneration
        /\ generationState = NewerReaderSeed.generationState
        /\ currentArtifactQuality = NewerReaderSeed.currentArtifactQuality
        /\ stagedArtifactQuality = NewerReaderSeed.stagedArtifactQuality
        /\ stagedValidated = NewerReaderSeed.stagedValidated
        /\ corePublicationState = NewerReaderSeed.corePublicationState
        /\ extractedGeneration = NewerReaderSeed.extractedGeneration
        /\ derivedGeneration = NewerReaderSeed.derivedGeneration
        /\ coreSchema = NewerReaderSeed.coreSchema
        /\ migrationPhase = NewerReaderSeed.migrationPhase
        /\ migrationSourceSchema = NewerReaderSeed.migrationSourceSchema
        /\ migrationTargetSchema = NewerReaderSeed.migrationTargetSchema
        /\ preMigrationGeneration = NewerReaderSeed.preMigrationGeneration
        /\ migrationRecovery = NewerReaderSeed.migrationRecovery
        /\ readerStatus = NewerReaderSeed.readerStatus
        /\ readerCompatibility = NewerReaderSeed.readerCompatibility
        /\ sidecarGeneration = NewerReaderSeed.sidecarGeneration
        /\ sidecarState = NewerReaderSeed.sidecarState
        /\ sidecarQuality = NewerReaderSeed.sidecarQuality
        /\ sidecarValidated = NewerReaderSeed.sidecarValidated
        /\ sidecarAttachment = NewerReaderSeed.sidecarAttachment
        /\ coreFilePublication = NewerReaderSeed.coreFilePublication
        /\ sidecarFilePublication = NewerReaderSeed.sidecarFilePublication
        /\ readCompleteness = NewerReaderSeed.readCompleteness
        /\ crossFileAtomicityAssumed = NewerReaderSeed.crossFileAtomicityAssumed
        /\ retiredGenerations = NewerReaderSeed.retiredGenerations
        /\ leasedGenerations = NewerReaderSeed.leasedGenerations
        /\ replayPinnedGenerations = NewerReaderSeed.replayPinnedGenerations
        /\ removedGenerations = NewerReaderSeed.removedGenerations
        /\ packageState = NewerReaderSeed.packageState
        /\ packageCoreComplete = NewerReaderSeed.packageCoreComplete
        /\ packageSidecarComplete = NewerReaderSeed.packageSidecarComplete
        /\ packageImported = NewerReaderSeed.packageImported
        /\ includePlanState = NewerReaderSeed.includePlanState
        /\ includeEvidence = NewerReaderSeed.includeEvidence
        /\ includeEditState = NewerReaderSeed.includeEditState
        /\ readOnlyWrites = NewerReaderSeed.readOnlyWrites
        /\ trace = NewerReaderSeed.trace
    ELSE IF Scenario = "incompatible-reader"
    THEN
        /\ currentGeneration = IncompatibleReaderSeed.currentGeneration
        /\ stagedGeneration = IncompatibleReaderSeed.stagedGeneration
        /\ generationState = IncompatibleReaderSeed.generationState
        /\ currentArtifactQuality = IncompatibleReaderSeed.currentArtifactQuality
        /\ stagedArtifactQuality = IncompatibleReaderSeed.stagedArtifactQuality
        /\ stagedValidated = IncompatibleReaderSeed.stagedValidated
        /\ corePublicationState = IncompatibleReaderSeed.corePublicationState
        /\ extractedGeneration = IncompatibleReaderSeed.extractedGeneration
        /\ derivedGeneration = IncompatibleReaderSeed.derivedGeneration
        /\ coreSchema = IncompatibleReaderSeed.coreSchema
        /\ migrationPhase = IncompatibleReaderSeed.migrationPhase
        /\ migrationSourceSchema = IncompatibleReaderSeed.migrationSourceSchema
        /\ migrationTargetSchema = IncompatibleReaderSeed.migrationTargetSchema
        /\ preMigrationGeneration = IncompatibleReaderSeed.preMigrationGeneration
        /\ migrationRecovery = IncompatibleReaderSeed.migrationRecovery
        /\ readerStatus = IncompatibleReaderSeed.readerStatus
        /\ readerCompatibility = IncompatibleReaderSeed.readerCompatibility
        /\ sidecarGeneration = IncompatibleReaderSeed.sidecarGeneration
        /\ sidecarState = IncompatibleReaderSeed.sidecarState
        /\ sidecarQuality = IncompatibleReaderSeed.sidecarQuality
        /\ sidecarValidated = IncompatibleReaderSeed.sidecarValidated
        /\ sidecarAttachment = IncompatibleReaderSeed.sidecarAttachment
        /\ coreFilePublication = IncompatibleReaderSeed.coreFilePublication
        /\ sidecarFilePublication = IncompatibleReaderSeed.sidecarFilePublication
        /\ readCompleteness = IncompatibleReaderSeed.readCompleteness
        /\ crossFileAtomicityAssumed = IncompatibleReaderSeed.crossFileAtomicityAssumed
        /\ retiredGenerations = IncompatibleReaderSeed.retiredGenerations
        /\ leasedGenerations = IncompatibleReaderSeed.leasedGenerations
        /\ replayPinnedGenerations = IncompatibleReaderSeed.replayPinnedGenerations
        /\ removedGenerations = IncompatibleReaderSeed.removedGenerations
        /\ packageState = IncompatibleReaderSeed.packageState
        /\ packageCoreComplete = IncompatibleReaderSeed.packageCoreComplete
        /\ packageSidecarComplete = IncompatibleReaderSeed.packageSidecarComplete
        /\ packageImported = IncompatibleReaderSeed.packageImported
        /\ includePlanState = IncompatibleReaderSeed.includePlanState
        /\ includeEvidence = IncompatibleReaderSeed.includeEvidence
        /\ includeEditState = IncompatibleReaderSeed.includeEditState
        /\ readOnlyWrites = IncompatibleReaderSeed.readOnlyWrites
        /\ trace = IncompatibleReaderSeed.trace
    ELSE FALSE

StartOneTUUpdate ==
    /\ corePublicationState = "current"
    /\ migrationPhase = "none"
    /\ stagedGeneration = 0
    /\ packageState \notin {"building", "imported"}
    /\ ProgressTraceAvailable
    /\ stagedGeneration' = currentGeneration + 1
    /\ generationState' = "extracting"
    /\ stagedArtifactQuality' = "partial"
    /\ stagedValidated' = FALSE
    /\ corePublicationState' = "staging"
    /\ readCompleteness' = IF SidecarCompleteFor(currentGeneration)
                           THEN "complete"
                           ELSE "partial"
    /\ trace' = Append(trace, "StartOneTUUpdate")
    /\ UNCHANGED <<currentGeneration, currentArtifactQuality,
        extractedGeneration, derivedGeneration, coreSchema, migrationPhase,
        migrationSourceSchema, migrationTargetSchema, preMigrationGeneration,
        migrationRecovery, readerStatus, readerCompatibility, sidecarGeneration,
        sidecarState, sidecarQuality, sidecarValidated, sidecarAttachment,
        coreFilePublication, sidecarFilePublication, crossFileAtomicityAssumed,
        retiredGenerations, leasedGenerations, replayPinnedGenerations,
        removedGenerations, packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includePlanState, includeEvidence, includeEditState,
        readOnlyWrites>>

PrepareStagedArtifact ==
    /\ generationState = "extracting"
    /\ stagedGeneration > 0
    /\ TraceAvailable
    /\ \E quality \in ArtifactQualities :
        /\ stagedArtifactQuality' = quality
        /\ generationState' = "validated"
        /\ trace' = Append(trace, "PrepareStagedArtifact")
    /\ UNCHANGED <<currentGeneration, currentArtifactQuality,
        stagedGeneration, stagedValidated, corePublicationState, extractedGeneration,
        derivedGeneration,
        coreSchema, migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, sidecarGeneration, sidecarState, sidecarQuality,
        sidecarValidated, sidecarAttachment, coreFilePublication,
        sidecarFilePublication, readCompleteness, crossFileAtomicityAssumed,
        retiredGenerations, leasedGenerations, replayPinnedGenerations,
        removedGenerations, packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includePlanState, includeEvidence, includeEditState,
        readOnlyWrites>>

ValidateStagedArtifact ==
    /\ generationState = "validated"
    /\ stagedGeneration > 0
    /\ TraceAvailable
    /\ stagedValidated' = (stagedArtifactQuality = "valid")
    /\ generationState' = IF stagedArtifactQuality = "valid"
                           THEN "validated"
                           ELSE "failed"
    /\ trace' = Append(trace, "ValidateStagedArtifact")
    /\ UNCHANGED <<currentGeneration, currentArtifactQuality,
        stagedGeneration, stagedArtifactQuality, corePublicationState,
        extractedGeneration, derivedGeneration, coreSchema, migrationPhase,
        migrationSourceSchema, migrationTargetSchema, preMigrationGeneration,
        migrationRecovery, readerStatus, readerCompatibility, sidecarGeneration,
        sidecarState, sidecarQuality, sidecarValidated, sidecarAttachment,
        coreFilePublication, sidecarFilePublication, readCompleteness,
        crossFileAtomicityAssumed, retiredGenerations, leasedGenerations,
        replayPinnedGenerations, removedGenerations, packageState,
        packageCoreComplete, packageSidecarComplete, packageImported,
        includePlanState, includeEvidence, includeEditState, readOnlyWrites>>

PublishCoreGeneration ==
    /\ generationState = "validated"
    /\ stagedValidated
    /\ stagedArtifactQuality = "valid"
    /\ corePublicationState = "staging"
    /\ migrationPhase = "none"
    /\ TraceAvailable
    /\ retiredGenerations' = retiredGenerations \cup {currentGeneration}
    /\ currentGeneration' = stagedGeneration
    /\ currentArtifactQuality' = stagedArtifactQuality
    /\ generationState' = "current"
    /\ stagedGeneration' = 0
    /\ corePublicationState' = "current"
    /\ extractedGeneration' = currentGeneration'
    /\ derivedGeneration' = currentGeneration'
    /\ coreFilePublication' = "current"
    /\ sidecarState' = IF SidecarCompleteFor(currentGeneration)
                       THEN "stale"
                       ELSE sidecarState
    /\ sidecarValidated' = IF SidecarCompleteFor(currentGeneration)
                           THEN FALSE
                           ELSE sidecarValidated
    /\ sidecarAttachment' = IF SidecarCompleteFor(currentGeneration)
                            THEN "detached"
                            ELSE sidecarAttachment
    /\ readCompleteness' = "partial"
    /\ trace' = Append(trace, "PublishCoreGeneration")
    /\ UNCHANGED <<stagedArtifactQuality, stagedValidated, coreSchema,
        migrationPhase,
        migrationSourceSchema, migrationTargetSchema, preMigrationGeneration,
        migrationRecovery, readerStatus, readerCompatibility, sidecarGeneration,
        sidecarQuality,
        sidecarFilePublication, crossFileAtomicityAssumed, leasedGenerations,
        replayPinnedGenerations, removedGenerations, packageState, packageCoreComplete,
        packageSidecarComplete, packageImported, includePlanState,
        includeEvidence, includeEditState, readOnlyWrites>>

InterruptPublication ==
    /\ corePublicationState = "staging"
    /\ stagedGeneration > 0
    /\ TraceAvailable
    /\ generationState' = "failed"
    /\ stagedGeneration' = 0
    /\ stagedValidated' = FALSE
    /\ corePublicationState' = "current"
    /\ readCompleteness' = IF SidecarCompleteFor(currentGeneration)
                           THEN "complete"
                           ELSE "partial"
    /\ trace' = Append(trace, "InterruptPublication")
    /\ UNCHANGED <<currentGeneration, currentArtifactQuality,
        stagedArtifactQuality, extractedGeneration, derivedGeneration, coreSchema,
        migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, sidecarGeneration, sidecarState, sidecarQuality,
        sidecarValidated, sidecarAttachment, coreFilePublication,
        sidecarFilePublication, crossFileAtomicityAssumed, retiredGenerations,
        leasedGenerations, replayPinnedGenerations, removedGenerations,
        packageState, packageCoreComplete, packageSidecarComplete, packageImported,
        includePlanState, includeEvidence, includeEditState, readOnlyWrites>>

BuildSidecar ==
    /\ generationState \in {"validated", "current"}
    /\ sidecarState \in {"absent", "stale", "missing", "corrupt", "incompatible"}
    /\ (stagedGeneration > 0 \/ sidecarState # "absent")
    /\ ProgressTraceAvailable
    /\ sidecarGeneration' = IF stagedGeneration > 0
                            THEN stagedGeneration
                            ELSE currentGeneration
    /\ sidecarState' = "building"
    /\ sidecarQuality' = "partial"
    /\ sidecarValidated' = FALSE
    /\ sidecarAttachment' = "detached"
    /\ sidecarFilePublication' = "none"
    /\ trace' = Append(trace, "BuildSidecar")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, readCompleteness, crossFileAtomicityAssumed,
        coreFilePublication,
        retiredGenerations, leasedGenerations, replayPinnedGenerations,
        removedGenerations, packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includePlanState, includeEvidence, includeEditState,
        readOnlyWrites>>

PrepareSidecarArtifact ==
    /\ sidecarState = "building"
    /\ sidecarGeneration > 0
    /\ TraceAvailable
    /\ \E quality \in ArtifactQualities :
        /\ sidecarQuality' = quality
        /\ sidecarState' = "validated"
        /\ sidecarValidated' = (quality = "valid")
        /\ trace' = Append(trace, "PrepareSidecarArtifact")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, sidecarGeneration, sidecarAttachment,
        coreFilePublication, sidecarFilePublication, readCompleteness,
        crossFileAtomicityAssumed, retiredGenerations, leasedGenerations,
        replayPinnedGenerations, removedGenerations, packageState,
        packageCoreComplete, packageSidecarComplete, packageImported,
        includePlanState, includeEvidence, includeEditState, readOnlyWrites>>

PublishSidecar ==
    /\ sidecarState = "validated"
    /\ sidecarValidated
    /\ sidecarQuality = "valid"
    /\ sidecarGeneration = currentGeneration
    /\ corePublicationState = "current"
    /\ migrationPhase = "none"
    /\ readerStatus = "current"
    /\ TraceAvailable
    /\ sidecarState' = "current"
    /\ sidecarAttachment' = "attached"
    /\ sidecarFilePublication' = "current"
    /\ readCompleteness' = "complete"
    /\ trace' = Append(trace, "PublishSidecar")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, sidecarGeneration, sidecarQuality,
        sidecarValidated, coreFilePublication, crossFileAtomicityAssumed,
        retiredGenerations, leasedGenerations, replayPinnedGenerations,
        removedGenerations, packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includePlanState, includeEvidence, includeEditState,
        readOnlyWrites>>

MarkSidecarMissing ==
    /\ sidecarState \in {"current", "stale", "absent"}
    /\ TraceAvailable
    /\ sidecarState' = "missing"
    /\ sidecarAttachment' = "detached"
    /\ sidecarFilePublication' = "none"
    /\ readCompleteness' = "partial"
    /\ trace' = Append(trace, "MarkSidecarMissing")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, sidecarGeneration, sidecarQuality,
        sidecarValidated, coreFilePublication, crossFileAtomicityAssumed,
        retiredGenerations, leasedGenerations, replayPinnedGenerations,
        removedGenerations, packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includePlanState, includeEvidence, includeEditState,
        readOnlyWrites>>

MarkSidecarCorrupt ==
    /\ sidecarState \in {"current", "stale"}
    /\ TraceAvailable
    /\ sidecarState' = "corrupt"
    /\ sidecarAttachment' = "detached"
    /\ sidecarQuality' = "corrupt"
    /\ sidecarValidated' = FALSE
    /\ readCompleteness' = "unknown"
    /\ trace' = Append(trace, "MarkSidecarCorrupt")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, sidecarGeneration, sidecarFilePublication,
        coreFilePublication,
        crossFileAtomicityAssumed, retiredGenerations, leasedGenerations,
        replayPinnedGenerations, removedGenerations, packageState,
        packageCoreComplete, packageSidecarComplete, packageImported,
        includePlanState, includeEvidence, includeEditState, readOnlyWrites>>

MarkSidecarIncompatible ==
    /\ sidecarState \in {"current", "stale", "absent"}
    /\ TraceAvailable
    /\ sidecarState' = "incompatible"
    /\ sidecarAttachment' = "detached"
    /\ sidecarQuality' = "incompatible"
    /\ sidecarValidated' = FALSE
    /\ readCompleteness' = "unknown"
    /\ trace' = Append(trace, "MarkSidecarIncompatible")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, sidecarGeneration, sidecarFilePublication,
        coreFilePublication, crossFileAtomicityAssumed, retiredGenerations,
        leasedGenerations, replayPinnedGenerations, removedGenerations,
        packageState, packageCoreComplete, packageSidecarComplete, packageImported,
        includePlanState, includeEvidence, includeEditState, readOnlyWrites>>

BeginSupportedMigration ==
    /\ migrationPhase = "none"
    /\ coreSchema = CoreSchemaVersion
    /\ corePublicationState = "current"
    /\ readerStatus = "current"
    /\ TraceAvailable
    /\ migrationPhase' = "staging"
    /\ migrationSourceSchema' = coreSchema
    /\ migrationTargetSchema' = SupportedSchemaVersion
    /\ preMigrationGeneration' = currentGeneration
    /\ migrationRecovery' = "snapshot-retained"
    /\ trace' = Append(trace, "BeginSupportedMigration")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        readerStatus, readerCompatibility, sidecarGeneration, sidecarState,
        sidecarQuality, sidecarValidated, sidecarAttachment, coreFilePublication,
        sidecarFilePublication, readCompleteness, crossFileAtomicityAssumed,
        retiredGenerations, leasedGenerations, replayPinnedGenerations,
        removedGenerations, packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includePlanState, includeEvidence, includeEditState,
        readOnlyWrites>>

ValidateMigration ==
    /\ migrationPhase = "staging"
    /\ migrationTargetSchema = SupportedSchemaVersion
    /\ TraceAvailable
    /\ migrationPhase' = "validated"
    /\ trace' = Append(trace, "ValidateMigration")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationSourceSchema, migrationTargetSchema, preMigrationGeneration,
        migrationRecovery, readerStatus, readerCompatibility, sidecarGeneration,
        sidecarState, sidecarQuality, sidecarValidated, sidecarAttachment,
        coreFilePublication, sidecarFilePublication, readCompleteness,
        crossFileAtomicityAssumed, retiredGenerations, leasedGenerations,
        replayPinnedGenerations, removedGenerations, packageState,
        packageCoreComplete, packageSidecarComplete, packageImported,
        includePlanState, includeEvidence, includeEditState, readOnlyWrites>>

RejectMigration ==
    /\ migrationPhase = "staging"
    /\ migrationTargetSchema = SupportedSchemaVersion
    /\ TraceAvailable
    /\ migrationPhase' = "rejected"
    /\ migrationRecovery' = "snapshot-retained"
    /\ trace' = Append(trace, "RejectMigration")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationSourceSchema, migrationTargetSchema, preMigrationGeneration,
        readerStatus, readerCompatibility, sidecarGeneration, sidecarState,
        sidecarQuality, sidecarValidated, sidecarAttachment, coreFilePublication,
        sidecarFilePublication, readCompleteness, crossFileAtomicityAssumed,
        retiredGenerations, leasedGenerations, replayPinnedGenerations,
        removedGenerations, packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includePlanState, includeEvidence, includeEditState,
        readOnlyWrites>>

CommitMigration ==
    /\ migrationPhase = "validated"
    /\ migrationTargetSchema = SupportedSchemaVersion
    /\ coreSchema = migrationSourceSchema
    /\ TraceAvailable
    /\ coreSchema' = migrationTargetSchema
    /\ migrationPhase' = "committed"
    /\ migrationRecovery' = "snapshot-retained"
    /\ readerStatus' = "current"
    /\ readerCompatibility' = "supported"
    /\ trace' = Append(trace, "CommitMigration")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration,
        migrationSourceSchema, migrationTargetSchema, preMigrationGeneration,
        sidecarGeneration, sidecarState, sidecarQuality, sidecarValidated,
        sidecarAttachment, coreFilePublication, sidecarFilePublication,
        readCompleteness, crossFileAtomicityAssumed, retiredGenerations,
        leasedGenerations, replayPinnedGenerations, removedGenerations,
        packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includePlanState, includeEvidence, includeEditState,
        readOnlyWrites>>

RollbackRejectedMigration ==
    /\ migrationPhase = "rejected"
    /\ coreSchema = migrationSourceSchema
    /\ currentGeneration = preMigrationGeneration
    /\ TraceAvailable
    /\ migrationPhase' = "none"
    /\ migrationRecovery' = "recovered-previous"
    /\ readerStatus' = "current"
    /\ readerCompatibility' = "supported"
    /\ readCompleteness' = IF SidecarCompleteFor(currentGeneration)
                           THEN "complete"
                           ELSE "partial"
    /\ trace' = Append(trace, "RollbackRejectedMigration")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationSourceSchema, migrationTargetSchema, preMigrationGeneration,
        sidecarGeneration, sidecarState, sidecarQuality, sidecarValidated,
        sidecarAttachment, coreFilePublication, sidecarFilePublication,
        crossFileAtomicityAssumed, retiredGenerations, leasedGenerations,
        replayPinnedGenerations, removedGenerations, packageState,
        packageCoreComplete, packageSidecarComplete, packageImported,
        includePlanState, includeEvidence, includeEditState, readOnlyWrites>>

FinishMigration ==
    /\ migrationPhase = "committed"
    /\ TraceAvailable
    /\ migrationPhase' = "none"
    /\ trace' = Append(trace, "FinishMigration")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationSourceSchema, migrationTargetSchema, preMigrationGeneration,
        migrationRecovery, readerStatus, readerCompatibility, sidecarGeneration,
        sidecarState, sidecarQuality, sidecarValidated, sidecarAttachment,
        coreFilePublication, sidecarFilePublication, readCompleteness,
        crossFileAtomicityAssumed, retiredGenerations, leasedGenerations,
        replayPinnedGenerations, removedGenerations, packageState,
        packageCoreComplete, packageSidecarComplete, packageImported,
        includePlanState, includeEvidence, includeEditState, readOnlyWrites>>

RollbackCommittedMigration ==
    /\ migrationPhase = "committed"
    /\ coreSchema = SupportedSchemaVersion
    /\ TraceAvailable
    /\ coreSchema' = migrationSourceSchema
    /\ migrationPhase' = "none"
    /\ migrationRecovery' = "recovered-previous"
    /\ readerStatus' = "current"
    /\ readerCompatibility' = "supported"
    /\ readCompleteness' = IF SidecarCompleteFor(currentGeneration)
                           THEN "complete"
                           ELSE "partial"
    /\ trace' = Append(trace, "RollbackCommittedMigration")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration,
        migrationSourceSchema, migrationTargetSchema, preMigrationGeneration,
        sidecarGeneration, sidecarState, sidecarQuality, sidecarValidated,
        sidecarAttachment, coreFilePublication, sidecarFilePublication,
        crossFileAtomicityAssumed, retiredGenerations, leasedGenerations,
        replayPinnedGenerations, removedGenerations, packageState,
        packageCoreComplete, packageSidecarComplete, packageImported,
        includePlanState, includeEvidence, includeEditState, readOnlyWrites>>

InterruptMigration ==
    /\ migrationPhase \in {"staging", "validated", "committed"}
    /\ TraceAvailable
    /\ migrationPhase' = "recovery-required"
    /\ migrationRecovery' = "interrupted"
    /\ readerStatus' = "unavailable"
    /\ readCompleteness' = "unavailable"
    /\ trace' = Append(trace, "InterruptMigration")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationSourceSchema, migrationTargetSchema, preMigrationGeneration,
        readerCompatibility, sidecarGeneration, sidecarState, sidecarQuality,
        sidecarValidated, sidecarAttachment, coreFilePublication,
        sidecarFilePublication, crossFileAtomicityAssumed, retiredGenerations,
        leasedGenerations, replayPinnedGenerations, removedGenerations,
        packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includePlanState, includeEvidence, includeEditState,
        readOnlyWrites>>

RecoverPreviousMigration ==
    /\ migrationPhase = "recovery-required"
    /\ coreSchema \in {migrationSourceSchema, migrationTargetSchema}
    /\ currentGeneration = preMigrationGeneration
    /\ TraceAvailable
    /\ coreSchema' = migrationSourceSchema
    /\ migrationPhase' = "none"
    /\ migrationRecovery' = "recovered-previous"
    /\ readerStatus' = "current"
    /\ readerCompatibility' = "supported"
    /\ readCompleteness' = IF SidecarCompleteFor(currentGeneration)
                           THEN "complete"
                           ELSE "partial"
    /\ trace' = Append(trace, "RecoverPreviousMigration")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration,
        migrationSourceSchema, migrationTargetSchema, preMigrationGeneration,
        sidecarGeneration, sidecarState, sidecarQuality, sidecarValidated,
        sidecarAttachment, coreFilePublication, sidecarFilePublication,
        crossFileAtomicityAssumed, retiredGenerations, leasedGenerations,
        replayPinnedGenerations, removedGenerations, packageState,
        packageCoreComplete, packageSidecarComplete, packageImported,
        includePlanState, includeEvidence, includeEditState, readOnlyWrites>>

OpenNewerReader ==
    /\ coreSchema = NewerSchemaVersion
    /\ readerStatus = "unavailable"
    /\ readerCompatibility = "newer"
    /\ TraceAvailable
    /\ trace' = Append(trace, "OpenNewerReader")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, sidecarGeneration, sidecarState, sidecarQuality,
        sidecarValidated, sidecarAttachment, coreFilePublication,
        sidecarFilePublication, readCompleteness, crossFileAtomicityAssumed,
        retiredGenerations, leasedGenerations, replayPinnedGenerations,
        removedGenerations, packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includePlanState, includeEvidence, includeEditState,
        readOnlyWrites>>

OpenIncompatibleReader ==
    /\ coreSchema = IncompatibleSchemaVersion
    /\ readerStatus = "unavailable"
    /\ readerCompatibility = "incompatible"
    /\ TraceAvailable
    /\ trace' = Append(trace, "OpenIncompatibleReader")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, sidecarGeneration, sidecarState, sidecarQuality,
        sidecarValidated, sidecarAttachment, coreFilePublication,
        sidecarFilePublication, readCompleteness, crossFileAtomicityAssumed,
        retiredGenerations, leasedGenerations, replayPinnedGenerations,
        removedGenerations, packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includePlanState, includeEvidence, includeEditState,
        readOnlyWrites>>

AcquireLease ==
    /\ \E generation \in retiredGenerations \ removedGenerations :
        /\ generation # currentGeneration
        /\ generation \notin leasedGenerations
        /\ leasedGenerations' = leasedGenerations \cup {generation}
    /\ TraceAvailable
    /\ trace' = Append(trace, "AcquireLease")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, sidecarGeneration, sidecarState, sidecarQuality,
        sidecarValidated, sidecarAttachment, coreFilePublication,
        sidecarFilePublication, readCompleteness, crossFileAtomicityAssumed,
        retiredGenerations, replayPinnedGenerations, removedGenerations,
        packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includePlanState, includeEvidence, includeEditState,
        readOnlyWrites>>

ReleaseLease ==
    /\ \E generation \in leasedGenerations :
        /\ leasedGenerations' = leasedGenerations \ {generation}
    /\ TraceAvailable
    /\ trace' = Append(trace, "ReleaseLease")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, sidecarGeneration, sidecarState, sidecarQuality,
        sidecarValidated, sidecarAttachment, coreFilePublication,
        sidecarFilePublication, readCompleteness, crossFileAtomicityAssumed,
        retiredGenerations, replayPinnedGenerations, removedGenerations,
        packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includePlanState, includeEvidence, includeEditState,
        readOnlyWrites>>

PinReplay ==
    /\ \E generation \in retiredGenerations \ removedGenerations :
        /\ generation # currentGeneration
        /\ generation \notin replayPinnedGenerations
        /\ replayPinnedGenerations' = replayPinnedGenerations \cup {generation}
    /\ TraceAvailable
    /\ trace' = Append(trace, "PinReplay")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, sidecarGeneration, sidecarState, sidecarQuality,
        sidecarValidated, sidecarAttachment, coreFilePublication,
        sidecarFilePublication, readCompleteness, crossFileAtomicityAssumed,
        retiredGenerations, leasedGenerations, removedGenerations,
        packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includePlanState, includeEvidence, includeEditState,
        readOnlyWrites>>

UnpinReplay ==
    /\ \E generation \in replayPinnedGenerations :
        /\ replayPinnedGenerations' = replayPinnedGenerations \ {generation}
    /\ TraceAvailable
    /\ trace' = Append(trace, "UnpinReplay")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, sidecarGeneration, sidecarState, sidecarQuality,
        sidecarValidated, sidecarAttachment, coreFilePublication,
        sidecarFilePublication, readCompleteness, crossFileAtomicityAssumed,
        retiredGenerations, leasedGenerations, removedGenerations,
        packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includePlanState, includeEvidence, includeEditState,
        readOnlyWrites>>

CleanupRetired ==
    /\ \E generation \in retiredGenerations \ removedGenerations :
        /\ generation # currentGeneration
        /\ generation \notin leasedGenerations
        /\ generation \notin replayPinnedGenerations
        /\ removedGenerations' = removedGenerations \cup {generation}
    /\ TraceAvailable
    /\ trace' = Append(trace, "CleanupRetired")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, sidecarGeneration, sidecarState, sidecarQuality,
        sidecarValidated, sidecarAttachment, coreFilePublication,
        sidecarFilePublication, readCompleteness, crossFileAtomicityAssumed,
        retiredGenerations, leasedGenerations, replayPinnedGenerations,
        packageState, packageCoreComplete,
        packageSidecarComplete, packageImported, includePlanState,
        includeEvidence, includeEditState, readOnlyWrites>>

BeginExport ==
    /\ packageState = "none"
    /\ corePublicationState = "current"
    /\ TraceAvailable
    /\ packageState' = "building"
    /\ packageCoreComplete' = FALSE
    /\ packageSidecarComplete' = FALSE
    /\ packageImported' = FALSE
    /\ trace' = Append(trace, "BeginExport")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, sidecarGeneration, sidecarState, sidecarQuality,
        sidecarValidated, sidecarAttachment, coreFilePublication,
        sidecarFilePublication, readCompleteness, crossFileAtomicityAssumed,
        retiredGenerations, leasedGenerations, replayPinnedGenerations,
        removedGenerations, includePlanState, includeEvidence, includeEditState,
        readOnlyWrites>>

FinalizeExport ==
    /\ packageState = "building"
    /\ TraceAvailable
    /\ packageCoreComplete' = TRUE
    /\ packageSidecarComplete' =
        (sidecarState = "current"
            /\ sidecarGeneration = currentGeneration
            /\ sidecarValidated)
    /\ packageState' = IF packageSidecarComplete'
                       THEN "complete"
                       ELSE "partial"
    /\ trace' = Append(trace, "FinalizeExport")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, sidecarGeneration, sidecarState, sidecarQuality,
        sidecarValidated, sidecarAttachment, coreFilePublication,
        sidecarFilePublication, readCompleteness, crossFileAtomicityAssumed,
        retiredGenerations, leasedGenerations, replayPinnedGenerations,
        removedGenerations, packageImported, includePlanState, includeEvidence,
        includeEditState, readOnlyWrites>>

ImportCompletePackage ==
    /\ packageState = "complete"
    /\ packageCoreComplete
    /\ packageSidecarComplete
    /\ TraceAvailable
    /\ packageState' = "imported"
    /\ packageImported' = TRUE
    /\ trace' = Append(trace, "ImportCompletePackage")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, sidecarGeneration, sidecarState, sidecarQuality,
        sidecarValidated, sidecarAttachment, coreFilePublication,
        sidecarFilePublication, readCompleteness, crossFileAtomicityAssumed,
        retiredGenerations, leasedGenerations, replayPinnedGenerations,
        removedGenerations, packageCoreComplete, packageSidecarComplete,
        includePlanState, includeEvidence, includeEditState, readOnlyWrites>>

PlanIncludeHygiene ==
    /\ includePlanState = "none"
    /\ corePublicationState = "current"
    /\ TraceAvailable
    /\ includePlanState' = "planned"
    /\ includeEvidence' = "complete"
    /\ includeEditState' = "none"
    /\ trace' = Append(trace, "PlanIncludeHygiene")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, sidecarGeneration, sidecarState, sidecarQuality,
        sidecarValidated, sidecarAttachment, coreFilePublication,
        sidecarFilePublication, readCompleteness, crossFileAtomicityAssumed,
        retiredGenerations, leasedGenerations, replayPinnedGenerations,
        removedGenerations, packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includeEditState, readOnlyWrites>>

ValidateIncludeHygiene ==
    /\ includePlanState = "planned"
    /\ includeEvidence = "complete"
    /\ TraceAvailable
    /\ includePlanState' = "validated"
    /\ trace' = Append(trace, "ValidateIncludeHygiene")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, sidecarGeneration, sidecarState, sidecarQuality,
        sidecarValidated, sidecarAttachment, coreFilePublication,
        sidecarFilePublication, readCompleteness, crossFileAtomicityAssumed,
        retiredGenerations, leasedGenerations, replayPinnedGenerations,
        removedGenerations, packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includeEvidence, includeEditState, readOnlyWrites>>

ApplyIncludeHygiene ==
    /\ includePlanState = "validated"
    /\ includeEvidence = "complete"
    /\ TraceAvailable
    /\ includePlanState' = "applied"
    /\ includeEditState' = "changed"
    /\ trace' = Append(trace, "ApplyIncludeHygiene")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, sidecarGeneration, sidecarState, sidecarQuality,
        sidecarValidated, sidecarAttachment, coreFilePublication,
        sidecarFilePublication, readCompleteness, crossFileAtomicityAssumed,
        retiredGenerations, leasedGenerations, replayPinnedGenerations,
        removedGenerations, packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includeEvidence, readOnlyWrites>>

RejectIncompleteIncludeHygiene ==
    /\ includePlanState = "planned"
    /\ includeEvidence = "incomplete"
    /\ TraceAvailable
    /\ includePlanState' = "rejected"
    /\ trace' = Append(trace, "RejectIncompleteIncludeHygiene")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, sidecarGeneration, sidecarState, sidecarQuality,
        sidecarValidated, sidecarAttachment, coreFilePublication,
        sidecarFilePublication, readCompleteness, crossFileAtomicityAssumed,
        retiredGenerations, leasedGenerations, replayPinnedGenerations,
        removedGenerations, packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includeEvidence, includeEditState, readOnlyWrites>>

ReadOnlyProbe ==
    /\ readerStatus \in {"current", "partial", "unavailable"}
    /\ TraceAvailable
    /\ readOnlyWrites' = readOnlyWrites
    /\ trace' = Append(trace, "ReadOnlyProbe")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, sidecarGeneration, sidecarState, sidecarQuality,
        sidecarValidated, sidecarAttachment, coreFilePublication,
        sidecarFilePublication, readCompleteness, crossFileAtomicityAssumed,
        retiredGenerations, leasedGenerations, replayPinnedGenerations,
        removedGenerations, packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includePlanState, includeEvidence, includeEditState>>

Advance == StartOneTUUpdate \/ PrepareStagedArtifact
    \/ ValidateStagedArtifact \/ PublishCoreGeneration \/ InterruptPublication
    \/ BuildSidecar \/ PrepareSidecarArtifact \/ PublishSidecar
    \/ MarkSidecarMissing \/ MarkSidecarCorrupt \/ MarkSidecarIncompatible
    \/ BeginSupportedMigration \/ ValidateMigration \/ RejectMigration
    \/ CommitMigration \/ RollbackRejectedMigration \/ FinishMigration
    \/ RollbackCommittedMigration \/ InterruptMigration \/ RecoverPreviousMigration
    \/ OpenNewerReader \/ OpenIncompatibleReader
    \/ AcquireLease \/ ReleaseLease \/ PinReplay \/ UnpinReplay \/ CleanupRetired
    \/ BeginExport \/ FinalizeExport \/ ImportCompletePackage
    \/ PlanIncludeHygiene \/ ValidateIncludeHygiene \/ ApplyIncludeHygiene
    \/ RejectIncompleteIncludeHygiene \/ ReadOnlyProbe

NoOp == UNCHANGED vars
Next == Advance \/ NoOp

Fairness ==
    /\ WF_vars(PrepareStagedArtifact)
    /\ WF_vars(ValidateStagedArtifact)
    /\ WF_vars(PublishCoreGeneration)
    /\ WF_vars(InterruptPublication)
    /\ WF_vars(PublishSidecar)
    /\ WF_vars(ValidateMigration)
    /\ WF_vars(RejectMigration)
    /\ WF_vars(CommitMigration)
    /\ WF_vars(RollbackRejectedMigration)
    /\ WF_vars(FinishMigration)
    /\ WF_vars(RollbackCommittedMigration)
    /\ WF_vars(InterruptMigration)
    /\ WF_vars(RecoverPreviousMigration)
    /\ WF_vars(CleanupRetired)
    /\ WF_vars(FinalizeExport)
    /\ WF_vars(ImportCompletePackage)
    /\ WF_vars(ValidateIncludeHygiene)
    /\ WF_vars(ApplyIncludeHygiene)

Spec == Init /\ [][Next]_vars /\ Fairness

StorageEventuallySettles ==
    [](Len(trace) + 1 < TraceBound /\ corePublicationState = "staging" =>
        <> ((corePublicationState = "current" /\ stagedGeneration = 0)
            \/ Len(trace) + 1 >= TraceBound))
    /\ [](Len(trace) + 1 < TraceBound /\
         migrationPhase \in {"staging", "validated", "rejected", "committed",
                              "recovery-required"} =>
        <> (migrationPhase = "none" \/ Len(trace) + 1 >= TraceBound))
    /\ [](Len(trace) + 1 < TraceBound /\ packageState = "building" =>
        <> (packageState \in {"complete", "partial"}
            \/ Len(trace) + 1 >= TraceBound))

TypeInvariant ==
    /\ currentGeneration \in Nat
    /\ stagedGeneration \in Nat
    /\ generationState \in GenerationStates
    /\ currentArtifactQuality \in ArtifactQualities
    /\ stagedArtifactQuality \in ArtifactQualities
    /\ stagedValidated \in BOOLEAN
    /\ corePublicationState \in CorePublicationStates
    /\ extractedGeneration \in Nat
    /\ derivedGeneration \in Nat
    /\ coreSchema \in Nat
    /\ migrationPhase \in MigrationPhases
    /\ migrationSourceSchema \in Nat
    /\ migrationTargetSchema \in Nat
    /\ preMigrationGeneration \in Nat
    /\ migrationRecovery \in MigrationRecoveryStates
    /\ readerStatus \in ReadStatuses
    /\ readerCompatibility \in ReaderCompatibilityStates
    /\ sidecarGeneration \in Nat
    /\ sidecarState \in SidecarStates
    /\ sidecarQuality \in ArtifactQualities
    /\ sidecarValidated \in BOOLEAN
    /\ sidecarAttachment \in SidecarAttachments
    /\ coreFilePublication \in FilePublicationStates
    /\ sidecarFilePublication \in FilePublicationStates
    /\ readCompleteness \in ReadCompletenessStates
    /\ crossFileAtomicityAssumed \in BOOLEAN
    /\ retiredGenerations \subseteq Nat
    /\ leasedGenerations \subseteq Nat
    /\ replayPinnedGenerations \subseteq Nat
    /\ removedGenerations \subseteq Nat
    /\ packageState \in PackageStates
    /\ packageCoreComplete \in BOOLEAN
    /\ packageSidecarComplete \in BOOLEAN
    /\ packageImported \in BOOLEAN
    /\ includePlanState \in IncludePlanStates
    /\ includeEvidence \in IncludeEvidenceStates
    /\ includeEditState \in IncludeEditStates
    /\ readOnlyWrites = 0
    /\ Len(trace) <= TraceBound
    /\ WorkspaceId \in WorkspaceIds

CurrentGenerationInvariant ==
    /\ currentGeneration > 0
    /\ currentArtifactQuality = "valid"
    /\ corePublicationState \in {"current", "staging"}
    /\ currentGeneration = extractedGeneration
    /\ derivedGeneration = currentGeneration

NoInvalidCurrentInvariant ==
    /\ corePublicationState = "current"
        => /\ currentArtifactQuality = "valid"
           /\ currentGeneration > 0
    /\ generationState = "current" => currentArtifactQuality = "valid"

ReadOnlyStateInvariant ==
    /\ readOnlyWrites = 0
    /\ readCompleteness = "complete"
        => /\ readerStatus = "current"
           /\ sidecarState = "current"
           /\ sidecarAttachment = "attached"
           /\ sidecarGeneration = currentGeneration

MigrationInvariant ==
    /\ migrationPhase \in {"staging", "validated"}
        => /\ currentGeneration = preMigrationGeneration
           /\ coreSchema = migrationSourceSchema
    /\ migrationPhase = "recovery-required"
        => /\ currentGeneration = preMigrationGeneration
           /\ coreSchema \in {migrationSourceSchema, migrationTargetSchema}
    /\ migrationPhase = "committed"
        => /\ coreSchema = SupportedSchemaVersion
           /\ currentGeneration = preMigrationGeneration
           /\ migrationRecovery = "snapshot-retained"
    /\ migrationPhase = "rejected"
        => /\ coreSchema = migrationSourceSchema
           /\ currentGeneration = preMigrationGeneration
           /\ migrationRecovery = "snapshot-retained"
    /\ readerCompatibility = "newer"
        => /\ readerStatus = "unavailable"
    /\ readerCompatibility = "incompatible"
        => /\ readerStatus = "unavailable"

SidecarInvariant ==
    /\ readCompleteness = "complete"
        => /\ sidecarState = "current"
           /\ sidecarValidated
           /\ sidecarQuality = "valid"
           /\ sidecarAttachment = "attached"
           /\ sidecarGeneration = currentGeneration
           /\ sidecarFilePublication = "current"
    /\ sidecarState \in {"missing", "corrupt", "incompatible"}
        => readCompleteness \in {"partial", "unknown", "unavailable"}
    /\ sidecarState = "stale"
        => readCompleteness \in {"partial", "unknown", "unavailable"}
    /\ sidecarAttachment = "attached"
        => sidecarState = "current" /\ sidecarGeneration = currentGeneration

CrossFileAtomicityInvariant ==
    /\ readCompleteness = "complete"
        => /\ coreFilePublication = "current"
           /\ sidecarFilePublication = "current"
           /\ sidecarGeneration = currentGeneration
    /\ crossFileAtomicityAssumed
        => /\ coreFilePublication = sidecarFilePublication
           /\ sidecarGeneration = currentGeneration
           /\ readCompleteness = "complete"

CleanupSafetyInvariant ==
    /\ removedGenerations \subseteq retiredGenerations
    /\ currentGeneration \notin retiredGenerations
    /\ removedGenerations \cap
          (leasedGenerations \cup replayPinnedGenerations \cup
           {currentGeneration}) = {}
    /\ leasedGenerations \cup replayPinnedGenerations
          \subseteq retiredGenerations \ removedGenerations

PackageInvariant ==
    /\ packageState = "partial" => /\ packageCoreComplete
                              /\ ~packageSidecarComplete
                              /\ ~packageImported
    /\ packageImported => packageState = "imported" /\ packageCoreComplete
    /\ packageState = "complete" => packageCoreComplete /\ packageSidecarComplete

IncludeHygieneInvariant ==
    /\ includePlanState = "applied"
        => /\ includeEvidence = "complete"
           /\ includeEditState = "changed"
    /\ includeEditState = "changed" => includePlanState = "applied"

PublicationRecoveryInvariant ==
    /\ corePublicationState = "staging"
        => stagedGeneration > 0 /\ stagedGeneration # currentGeneration
    /\ migrationPhase = "recovery-required"
        => /\ readerStatus = "unavailable"
           /\ currentGeneration = preMigrationGeneration
           /\ coreSchema \in {migrationSourceSchema, migrationTargetSchema}

TraceInvariant ==
    /\ Len(trace) > 0
    /\ Head(trace) = "Init"

BoundedProgressInvariant ==
    Len(trace) = TraceBound
        => /\ corePublicationState # "staging"
           /\ migrationPhase \notin {"staging", "validated", "rejected",
                                      "committed", "recovery-required"}
           /\ packageState # "building"

=============================================================================
