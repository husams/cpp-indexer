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
    retiredGeneration,
    retiredGenerationState,
    leaseHeld,
    replayPinned,
    cleanupState,
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
    retiredGeneration,
    retiredGenerationState,
    leaseHeld,
    replayPinned,
    cleanupState,
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
     retiredGeneration |-> 0,
     retiredGenerationState |-> "none",
     leaseHeld |-> FALSE,
     replayPinned |-> FALSE,
     cleanupState |-> "kept",
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
    /\ retiredGeneration = state.retiredGeneration
    /\ retiredGenerationState = state.retiredGenerationState
    /\ leaseHeld = state.leaseHeld
    /\ replayPinned = state.replayPinned
    /\ cleanupState = state.cleanupState
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
     sidecarAttachment |-> "attached",
     coreFilePublication |-> "current",
     sidecarFilePublication |-> "current",
     readCompleteness |-> "complete",
     crossFileAtomicityAssumed |-> TRUE,
     retiredGeneration |-> InitialGeneration,
     retiredGenerationState |-> "stale",
     leaseHeld |-> FALSE,
     replayPinned |-> FALSE,
     cleanupState |-> "kept",
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

Init ==
    IF Scenario = "valid"
    THEN ValidInitialState
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
        /\ retiredGeneration = CrossFileAtomicitySeed.retiredGeneration
        /\ retiredGenerationState = CrossFileAtomicitySeed.retiredGenerationState
        /\ leaseHeld = CrossFileAtomicitySeed.leaseHeld
        /\ replayPinned = CrossFileAtomicitySeed.replayPinned
        /\ cleanupState = CrossFileAtomicitySeed.cleanupState
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
        /\ retiredGeneration = NewerReaderSeed.retiredGeneration
        /\ retiredGenerationState = NewerReaderSeed.retiredGenerationState
        /\ leaseHeld = NewerReaderSeed.leaseHeld
        /\ replayPinned = NewerReaderSeed.replayPinned
        /\ cleanupState = NewerReaderSeed.cleanupState
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
        /\ retiredGeneration = IncompatibleReaderSeed.retiredGeneration
        /\ retiredGenerationState = IncompatibleReaderSeed.retiredGenerationState
        /\ leaseHeld = IncompatibleReaderSeed.leaseHeld
        /\ replayPinned = IncompatibleReaderSeed.replayPinned
        /\ cleanupState = IncompatibleReaderSeed.cleanupState
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
        retiredGeneration, retiredGenerationState, leaseHeld, replayPinned,
        cleanupState, packageState, packageCoreComplete, packageSidecarComplete,
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
        retiredGeneration, retiredGenerationState, leaseHeld, replayPinned,
        cleanupState, packageState, packageCoreComplete, packageSidecarComplete,
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
        crossFileAtomicityAssumed, retiredGeneration, retiredGenerationState,
        leaseHeld, replayPinned, cleanupState, packageState,
        packageCoreComplete, packageSidecarComplete, packageImported,
        includePlanState, includeEvidence, includeEditState, readOnlyWrites>>

PublishCoreGeneration ==
    /\ generationState = "validated"
    /\ stagedValidated
    /\ stagedArtifactQuality = "valid"
    /\ corePublicationState = "staging"
    /\ migrationPhase = "none"
    /\ TraceAvailable
    /\ retiredGeneration' = currentGeneration
    /\ retiredGenerationState' = "stale"
    /\ currentGeneration' = stagedGeneration
    /\ currentArtifactQuality' = stagedArtifactQuality
    /\ generationState' = "current"
    /\ stagedGeneration' = 0
    /\ corePublicationState' = "current"
    /\ extractedGeneration' = currentGeneration'
    /\ derivedGeneration' = currentGeneration'
    /\ coreFilePublication' = "current"
    /\ readCompleteness' = IF SidecarCompleteFor(currentGeneration')
                           THEN "complete"
                           ELSE "partial"
    /\ trace' = Append(trace, "PublishCoreGeneration")
    /\ UNCHANGED <<stagedArtifactQuality, stagedValidated, coreSchema,
        migrationPhase,
        migrationSourceSchema, migrationTargetSchema, preMigrationGeneration,
        migrationRecovery, readerStatus, readerCompatibility, sidecarGeneration,
        sidecarState, sidecarQuality, sidecarValidated, sidecarAttachment,
        sidecarFilePublication, crossFileAtomicityAssumed, leaseHeld,
        replayPinned, cleanupState, packageState, packageCoreComplete,
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
        sidecarFilePublication, crossFileAtomicityAssumed, retiredGeneration,
        retiredGenerationState, leaseHeld, replayPinned, cleanupState,
        packageState, packageCoreComplete, packageSidecarComplete, packageImported,
        includePlanState, includeEvidence, includeEditState, readOnlyWrites>>

BuildSidecar ==
    /\ stagedGeneration > 0
    /\ generationState = "validated"
    /\ sidecarState = "absent"
    /\ ProgressTraceAvailable
    /\ sidecarGeneration' = stagedGeneration
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
        retiredGeneration, retiredGenerationState, leaseHeld, replayPinned,
        cleanupState, packageState, packageCoreComplete, packageSidecarComplete,
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
        crossFileAtomicityAssumed, retiredGeneration, retiredGenerationState,
        leaseHeld, replayPinned, cleanupState, packageState,
        packageCoreComplete, packageSidecarComplete, packageImported,
        includePlanState, includeEvidence, includeEditState, readOnlyWrites>>

PublishSidecar ==
    /\ sidecarState = "validated"
    /\ sidecarValidated
    /\ sidecarQuality = "valid"
    /\ sidecarGeneration = currentGeneration
    /\ corePublicationState = "current"
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
        retiredGeneration, retiredGenerationState, leaseHeld, replayPinned,
        cleanupState, packageState, packageCoreComplete, packageSidecarComplete,
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
        retiredGeneration, retiredGenerationState, leaseHeld, replayPinned,
        cleanupState, packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includePlanState, includeEvidence, includeEditState,
        readOnlyWrites>>

MarkSidecarCorrupt ==
    /\ sidecarState = "current"
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
        crossFileAtomicityAssumed, retiredGeneration, retiredGenerationState,
        leaseHeld, replayPinned, cleanupState, packageState,
        packageCoreComplete, packageSidecarComplete, packageImported,
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
        retiredGeneration, retiredGenerationState, leaseHeld, replayPinned,
        cleanupState, packageState, packageCoreComplete, packageSidecarComplete,
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
        crossFileAtomicityAssumed, retiredGeneration, retiredGenerationState,
        leaseHeld, replayPinned, cleanupState, packageState,
        packageCoreComplete, packageSidecarComplete, packageImported,
        includePlanState, includeEvidence, includeEditState, readOnlyWrites>>

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
        readCompleteness, crossFileAtomicityAssumed, retiredGeneration,
        retiredGenerationState, leaseHeld, replayPinned, cleanupState,
        packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includePlanState, includeEvidence, includeEditState,
        readOnlyWrites>>

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
        crossFileAtomicityAssumed, retiredGeneration, retiredGenerationState,
        leaseHeld, replayPinned, cleanupState, packageState,
        packageCoreComplete, packageSidecarComplete, packageImported,
        includePlanState, includeEvidence, includeEditState, readOnlyWrites>>

InterruptMigration ==
    /\ migrationPhase \in {"staging", "validated"}
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
        sidecarFilePublication, crossFileAtomicityAssumed, retiredGeneration,
        retiredGenerationState, leaseHeld, replayPinned, cleanupState,
        packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includePlanState, includeEvidence, includeEditState,
        readOnlyWrites>>

RecoverPreviousMigration ==
    /\ migrationPhase = "recovery-required"
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
    /\ trace' = Append(trace, "RecoverPreviousMigration")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationSourceSchema, migrationTargetSchema, preMigrationGeneration,
        sidecarGeneration, sidecarState, sidecarQuality, sidecarValidated,
        sidecarAttachment, coreFilePublication, sidecarFilePublication,
        crossFileAtomicityAssumed, retiredGeneration, retiredGenerationState,
        leaseHeld, replayPinned, cleanupState, packageState,
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
        retiredGeneration, retiredGenerationState, leaseHeld, replayPinned,
        cleanupState, packageState, packageCoreComplete, packageSidecarComplete,
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
        retiredGeneration, retiredGenerationState, leaseHeld, replayPinned,
        cleanupState, packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includePlanState, includeEvidence, includeEditState,
        readOnlyWrites>>

AcquireLease ==
    /\ retiredGeneration > 0
    /\ retiredGenerationState = "stale"
    /\ cleanupState = "kept"
    /\ TraceAvailable
    /\ leaseHeld' = TRUE
    /\ trace' = Append(trace, "AcquireLease")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, sidecarGeneration, sidecarState, sidecarQuality,
        sidecarValidated, sidecarAttachment, coreFilePublication,
        sidecarFilePublication, readCompleteness, crossFileAtomicityAssumed,
        retiredGeneration, retiredGenerationState, replayPinned, cleanupState,
        packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includePlanState, includeEvidence, includeEditState,
        readOnlyWrites>>

ReleaseLease ==
    /\ leaseHeld
    /\ TraceAvailable
    /\ leaseHeld' = FALSE
    /\ trace' = Append(trace, "ReleaseLease")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, sidecarGeneration, sidecarState, sidecarQuality,
        sidecarValidated, sidecarAttachment, coreFilePublication,
        sidecarFilePublication, readCompleteness, crossFileAtomicityAssumed,
        retiredGeneration, retiredGenerationState, replayPinned, cleanupState,
        packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includePlanState, includeEvidence, includeEditState,
        readOnlyWrites>>

PinReplay ==
    /\ retiredGeneration > 0
    /\ retiredGenerationState = "stale"
    /\ cleanupState = "kept"
    /\ TraceAvailable
    /\ replayPinned' = TRUE
    /\ trace' = Append(trace, "PinReplay")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, sidecarGeneration, sidecarState, sidecarQuality,
        sidecarValidated, sidecarAttachment, coreFilePublication,
        sidecarFilePublication, readCompleteness, crossFileAtomicityAssumed,
        retiredGeneration, retiredGenerationState, leaseHeld, cleanupState,
        packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includePlanState, includeEvidence, includeEditState,
        readOnlyWrites>>

UnpinReplay ==
    /\ replayPinned
    /\ TraceAvailable
    /\ replayPinned' = FALSE
    /\ trace' = Append(trace, "UnpinReplay")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, sidecarGeneration, sidecarState, sidecarQuality,
        sidecarValidated, sidecarAttachment, coreFilePublication,
        sidecarFilePublication, readCompleteness, crossFileAtomicityAssumed,
        retiredGeneration, retiredGenerationState, leaseHeld, cleanupState,
        packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includePlanState, includeEvidence, includeEditState,
        readOnlyWrites>>

CleanupRetired ==
    /\ retiredGeneration > 0
    /\ retiredGenerationState = "stale"
    /\ retiredGeneration # currentGeneration
    /\ ~leaseHeld
    /\ ~replayPinned
    /\ cleanupState = "kept"
    /\ TraceAvailable
    /\ retiredGenerationState' = "retired"
    /\ cleanupState' = "removed"
    /\ trace' = Append(trace, "CleanupRetired")
    /\ UNCHANGED <<currentGeneration, stagedGeneration, generationState,
        currentArtifactQuality, stagedArtifactQuality, stagedValidated,
        corePublicationState, extractedGeneration, derivedGeneration, coreSchema,
        migrationPhase, migrationSourceSchema, migrationTargetSchema,
        preMigrationGeneration, migrationRecovery, readerStatus,
        readerCompatibility, sidecarGeneration, sidecarState, sidecarQuality,
        sidecarValidated, sidecarAttachment, coreFilePublication,
        sidecarFilePublication, readCompleteness, crossFileAtomicityAssumed,
        retiredGeneration, leaseHeld, replayPinned, packageState, packageCoreComplete,
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
        retiredGeneration, retiredGenerationState, leaseHeld, replayPinned,
        cleanupState, includePlanState, includeEvidence, includeEditState,
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
        retiredGeneration, retiredGenerationState, leaseHeld, replayPinned,
        cleanupState, packageImported, includePlanState, includeEvidence,
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
        retiredGeneration, retiredGenerationState, leaseHeld, replayPinned,
        cleanupState, packageCoreComplete, packageSidecarComplete,
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
        retiredGeneration, retiredGenerationState, leaseHeld, replayPinned,
        cleanupState, packageState, packageCoreComplete, packageSidecarComplete,
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
        retiredGeneration, retiredGenerationState, leaseHeld, replayPinned,
        cleanupState, packageState, packageCoreComplete, packageSidecarComplete,
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
        retiredGeneration, retiredGenerationState, leaseHeld, replayPinned,
        cleanupState, packageState, packageCoreComplete, packageSidecarComplete,
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
        retiredGeneration, retiredGenerationState, leaseHeld, replayPinned,
        cleanupState, packageState, packageCoreComplete, packageSidecarComplete,
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
        retiredGeneration, retiredGenerationState, leaseHeld, replayPinned,
        cleanupState, packageState, packageCoreComplete, packageSidecarComplete,
        packageImported, includePlanState, includeEvidence, includeEditState>>

Advance == StartOneTUUpdate \/ PrepareStagedArtifact
    \/ ValidateStagedArtifact \/ PublishCoreGeneration \/ InterruptPublication
    \/ BuildSidecar \/ PrepareSidecarArtifact \/ PublishSidecar
    \/ MarkSidecarMissing \/ MarkSidecarCorrupt
    \/ BeginSupportedMigration \/ ValidateMigration \/ CommitMigration
    \/ FinishMigration \/ InterruptMigration \/ RecoverPreviousMigration
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
    /\ WF_vars(CommitMigration)
    /\ WF_vars(FinishMigration)
    /\ WF_vars(RecoverPreviousMigration)
    /\ WF_vars(CleanupRetired)
    /\ WF_vars(FinalizeExport)
    /\ WF_vars(ImportCompletePackage)
    /\ WF_vars(ValidateIncludeHygiene)
    /\ WF_vars(ApplyIncludeHygiene)

Spec == Init /\ [][Next]_vars /\ Fairness

StorageEventuallySettles ==
    [](corePublicationState = "staging" =>
        <> (corePublicationState = "current" /\ stagedGeneration = 0))
    /\ [](migrationPhase \in {"staging", "validated", "recovery-required"} =>
        <> (migrationPhase = "none"))
    /\ [](packageState = "building" =>
        <> (packageState \in {"complete", "partial"}))

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
    /\ retiredGeneration \in Nat
    /\ retiredGenerationState \in RetiredStates
    /\ leaseHeld \in BOOLEAN
    /\ replayPinned \in BOOLEAN
    /\ cleanupState \in CleanupStates
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
    /\ migrationPhase \in {"staging", "validated", "recovery-required"}
        => /\ currentGeneration = preMigrationGeneration
           /\ coreSchema = migrationSourceSchema
    /\ migrationPhase = "committed"
        => /\ coreSchema = SupportedSchemaVersion
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
    /\ cleanupState = "removed"
        => /\ retiredGenerationState = "retired"
           /\ retiredGeneration # currentGeneration
           /\ ~leaseHeld
           /\ ~replayPinned
    /\ (retiredGeneration = currentGeneration
        \/ leaseHeld \/ replayPinned)
        => cleanupState = "kept"

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
           /\ coreSchema = migrationSourceSchema

TraceInvariant ==
    /\ Len(trace) > 0
    /\ Head(trace) = "Init"

BoundedProgressInvariant ==
    Len(trace) = TraceBound
        => /\ corePublicationState # "staging"
           /\ migrationPhase \notin {"staging", "validated", "recovery-required"}
           /\ packageState # "building"

=============================================================================
