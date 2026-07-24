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
    trace
>>

TraceAvailable == Len(trace) < TraceBound

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
    /\ trace = <<"Init">>

ImportWorkspace ==
    /\ workspaceState = "empty"
    /\ TraceAvailable
    /\ workspaceState' = "imported"
    /\ trace' = Append(trace, "ImportWorkspace")
    /\ UNCHANGED <<configState, indexState, publicationState, transformState,
                    queryState, storageState, artifactState, failureMode,
                    generation, currentGeneration, migrationBaseline,
                    invalidated, evidenceState, queryResultState,
                    queryTruncated, queryWrites, identityMode>>

CaptureConfiguration ==
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
                    queryWrites, identityMode>>

StartIndexing ==
    /\ configState = "captured"
    /\ indexState \in {"empty", "stale", "failed"}
    /\ TraceAvailable
    /\ indexState' = "indexing"
    /\ failureMode' = "none"
    /\ trace' = Append(trace, "StartIndexing")
    /\ UNCHANGED <<workspaceState, configState, publicationState,
                    transformState, queryState, storageState, artifactState,
                    generation, currentGeneration, migrationBaseline,
                    invalidated, evidenceState, queryResultState,
                    queryTruncated, queryWrites, identityMode>>

IndexSuccessfully ==
    /\ indexState = "indexing"
    /\ TraceAvailable
    /\ indexState' = "indexed"
    /\ publicationState' = "candidate"
    /\ generation' = generation + 1
    /\ failureMode' = "none"
    /\ trace' = Append(trace, "IndexSuccessfully")
    /\ UNCHANGED <<workspaceState, configState, transformState, queryState,
                    storageState, artifactState, currentGeneration,
                    migrationBaseline, invalidated, evidenceState,
                    queryResultState, queryTruncated, queryWrites,
                    identityMode>>

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
                    queryTruncated, queryWrites, identityMode>>

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
    /\ trace' = Append(trace, "InterruptPublication")
    /\ UNCHANGED <<workspaceState, configState, transformState, queryState,
                    storageState, artifactState, generation, currentGeneration,
                    migrationBaseline, invalidated, evidenceState,
                    queryResultState, queryTruncated, queryWrites,
                    identityMode>>

InvalidateGeneration ==
    /\ publicationState = "current"
    /\ indexState = "current"
    /\ TraceAvailable
    /\ publicationState' = "stale"
    /\ indexState' = "stale"
    /\ invalidated' = TRUE
    /\ trace' = Append(trace, "InvalidateGeneration")
    /\ UNCHANGED <<workspaceState, configState, transformState, queryState,
                    storageState, artifactState, failureMode, generation,
                    currentGeneration, migrationBaseline, evidenceState,
                    queryResultState, queryTruncated, queryWrites,
                    identityMode>>

PlanTransform ==
    /\ publicationState = "current"
    /\ indexState = "current"
    /\ invalidated = FALSE
    /\ artifactState = "published"
    /\ TraceAvailable
    /\ transformState' = "planned"
    /\ evidenceState' \in EvidenceStates
    /\ failureMode' = "none"
    /\ trace' = Append(trace, "PlanTransform")
    /\ UNCHANGED <<workspaceState, configState, indexState, publicationState,
                    queryState, storageState, artifactState,
                    generation, currentGeneration, migrationBaseline,
                    invalidated, queryResultState, queryTruncated,
                    queryWrites, identityMode>>

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
                    queryWrites, identityMode>>

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
                    queryWrites, identityMode>>

StartQuery ==
    /\ publicationState = "current"
    /\ indexState = "current"
    /\ invalidated = FALSE
    /\ queryState = "idle"
    /\ TraceAvailable
    /\ queryState' = "running"
    /\ trace' = Append(trace, "StartQuery")
    /\ UNCHANGED <<workspaceState, configState, indexState, publicationState,
                    transformState, storageState, artifactState, failureMode,
                    generation, currentGeneration, migrationBaseline,
                    invalidated, evidenceState, queryResultState,
                    queryTruncated, queryWrites, identityMode>>

ReturnQuery ==
    /\ queryState = "running"
    /\ TraceAvailable
    /\ queryState' = "complete"
    /\ queryResultState' \in {"complete", "partial", "ambiguous", "unknown"}
    /\ queryTruncated' = (queryResultState' = "partial")
    /\ trace' = Append(trace, "ReturnQuery")
    /\ UNCHANGED <<workspaceState, configState, indexState, publicationState,
                    transformState, storageState, artifactState, failureMode,
                    generation, currentGeneration, migrationBaseline,
                    invalidated, evidenceState, queryWrites, identityMode>>

BeginMigration ==
    /\ storageState = "ready"
    /\ TraceAvailable
    /\ storageState' = "migrating"
    /\ migrationBaseline' = currentGeneration
    /\ trace' = Append(trace, "BeginMigration")
    /\ UNCHANGED <<workspaceState, configState, indexState, publicationState,
                    transformState, queryState, artifactState, failureMode,
                    generation, currentGeneration, invalidated, evidenceState,
                    queryResultState, queryTruncated, queryWrites, identityMode>>

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
                    queryWrites, identityMode>>

InterruptMigration ==
    /\ storageState = "migrating"
    /\ TraceAvailable
    /\ storageState' = "recovery-required"
    /\ failureMode' = "interrupted"
    /\ trace' = Append(trace, "InterruptMigration")
    /\ UNCHANGED <<workspaceState, configState, indexState, publicationState,
                    transformState, queryState, artifactState, generation,
                    currentGeneration, migrationBaseline, invalidated,
                    evidenceState, queryResultState, queryTruncated,
                    queryWrites, identityMode>>

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
                    queryWrites, identityMode>>

MergeSymbolsInUniverse ==
    /\ identityMode = "separate"
    /\ SemanticUniverseId = SharedUniverseId
    /\ TraceAvailable
    /\ identityMode' = "merged"
    /\ trace' = Append(trace, "MergeSymbolsInUniverse")
    /\ UNCHANGED <<workspaceState, configState, indexState, publicationState,
                    transformState, queryState, storageState, artifactState,
                    failureMode, generation, currentGeneration,
                    migrationBaseline, invalidated, evidenceState,
                    queryResultState, queryTruncated, queryWrites>>

SeparateSymbolsAcrossUniverses ==
    /\ identityMode = "merged"
    /\ TraceAvailable
    /\ identityMode' = "separate"
    /\ trace' = Append(trace, "SeparateSymbolsAcrossUniverses")
    /\ UNCHANGED <<workspaceState, configState, indexState, publicationState,
                    transformState, queryState, storageState, artifactState,
                    failureMode, generation, currentGeneration,
                    migrationBaseline, invalidated, evidenceState,
                    queryResultState, queryTruncated, queryWrites>>

NoOp == UNCHANGED vars

Advance == ImportWorkspace \/ CaptureConfiguration \/ StartIndexing
    \/ IndexSuccessfully \/ IndexFails \/ PublishGeneration
    \/ InterruptPublication \/ InvalidateGeneration \/ PlanTransform
    \/ ApplyTransform \/ RejectIncompleteTransform \/ StartQuery \/ ReturnQuery
    \/ BeginMigration \/ CompleteMigration \/ InterruptMigration
    \/ RecoverMigration \/ MergeSymbolsInUniverse
    \/ SeparateSymbolsAcrossUniverses

Next == Advance \/ NoOp

Fairness == WF_vars(Advance)

Spec == Init /\ [][Next]_vars /\ Fairness

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
    /\ generation \in Nat
    /\ currentGeneration \in Nat
    /\ migrationBaseline \in Nat
    /\ currentGeneration <= generation
    /\ queryWrites = 0
    /\ Len(trace) <= TraceBound
    /\ WorkspaceId \in WorkspaceIds
    /\ RepositoryId \in RepositoryIds
    /\ TranslationUnitId \in TranslationUnitIds
    /\ ConfigurationId \in ConfigurationIds
    /\ GenerationId \in GenerationIds
    /\ ArtifactId \in ArtifactIds
    /\ QueryId \in QueryIds

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

TraceInvariant ==
    /\ Len(trace) > 0
    /\ Head(trace) = "Init"

ProtectedInvariant ==
    /\ NoPartialGenerationInvariant
    /\ QueryHonestyInvariant
    /\ MigrationInvariant

=============================================================================
