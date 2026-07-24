---------------------- MODULE CidxWorkspaceLifecycle -----------------------

EXTENDS CidxTypes, FiniteSets, CidxProtected

(***************************************************************************)
(* The workspace is the declared boundary for semantic identity.  Repository *)
(* paths, clone locations, and translation-unit configuration are evidence    *)
(* inside that boundary; none is a substitute for the boundary itself.       *)
(***************************************************************************)

CONSTANTS
    WorkspaceId,
    UniverseIds,
    InputIds,
    RevisionIds,
    ToolCatalogIds,
    TransformId,
    Scenario

VARIABLES
    phase,
    workspace,
    universes,
    repositories,
    sources,
    translationUnits,
    configurations,
    symbols,
    symbolAppearances,
    facts,
    generations,
    currentGeneration,
    requiredRevision,
    invalidatedInputs,
    readStatus

vars == <<
    phase,
    workspace,
    universes,
    repositories,
    sources,
    translationUnits,
    configurations,
    symbols,
    symbolAppearances,
    facts,
    generations,
    currentGeneration,
    requiredRevision,
    invalidatedInputs,
    readStatus
>>

NoCurrent == "none"

SeedScenarioIds == {
    "cross-universe-conflation",
    "partial-publication",
    "missing-invalidation",
    "stale-as-current"
}

GenerationRecord == [
    id: GenerationIds,
    workspace: {WorkspaceId},
    status: GenerationStatuses,
    inputRevision: RevisionIds,
    toolCatalog: ToolCatalogIds,
    configurations: SUBSET ConfigurationIds,
    inputs: SUBSET InputIds,
    outputs: SUBSET ArtifactIds,
    complete: BOOLEAN,
    validated: BOOLEAN
]

RepositoryRecord == [
    id: RepositoryIds,
    universe: UniverseIds
]

SourceRecord == [
    id: SourceIds,
    repository: RepositoryIds,
    kind: SourceKinds
]

TranslationUnitRecord == [
    id: TranslationUnitIds,
    source: SourceIds,
    configuration: ConfigurationIds
]

ConfigurationRecord == [
    id: ConfigurationIds,
    descriptor: STRING
]

SymbolRecord == [
    id: SymbolIds,
    universe: UniverseIds,
    usr: STRING,
    kind: SymbolKinds
]

AppearanceRecord == [
    id: InputIds,
    source: SourceIds,
    symbol: SymbolIds,
    configuration: ConfigurationIds
]

FactRecord == [
    id: InputIds,
    symbol: SymbolIds,
    configurations: SUBSET ConfigurationIds,
    invariant: BOOLEAN
]

FixtureWorkspace == [
    id |-> WorkspaceId,
    universes |-> UniverseIds,
    repositories |-> RepositoryIds,
    sources |-> SourceIds,
    translationUnits |-> TranslationUnitIds,
    configurations |-> ConfigurationIds,
    generations |-> GenerationIds
]

FixtureUniverses == {
    [id |-> "universe-1", workspace |-> WorkspaceId],
    [id |-> "universe-2", workspace |-> WorkspaceId]
}

FixtureRepositories == {
    [id |-> "repository-1", universe |-> "universe-1"],
    [id |-> "repository-2", universe |-> "universe-1"],
    [id |-> "repository-3", universe |-> "universe-2"]
}

FixtureSources == {
    [id |-> "source-1", repository |-> "repository-1", kind |-> "header"],
    [id |-> "source-2", repository |-> "repository-2", kind |-> "source"],
    [id |-> "source-3", repository |-> "repository-3", kind |-> "source"]
}

FixtureTranslationUnits == {
    [id |-> "tu-1", source |-> "source-1", configuration |-> "config-1"],
    [id |-> "tu-2", source |-> "source-1", configuration |-> "config-2"],
    [id |-> "tu-3", source |-> "source-2", configuration |-> "config-1"],
    [id |-> "tu-4", source |-> "source-3", configuration |-> "config-1"]
}

FixtureConfigurations == {
    [id |-> "config-1", descriptor |-> "clang-target-a"],
    [id |-> "config-2", descriptor |-> "clang-target-b"]
}

FixtureSymbols == {
    [id |-> "symbol-shared", universe |-> "universe-1",
        usr |-> "usr::external", kind |-> "function"],
    [id |-> "symbol-isolated", universe |-> "universe-2",
        usr |-> "usr::external", kind |-> "function"]
}

FixtureAppearances == {
    [id |-> "appearance-1", source |-> "source-1",
        symbol |-> "symbol-shared", configuration |-> "config-1"],
    [id |-> "appearance-2", source |-> "source-1",
        symbol |-> "symbol-shared", configuration |-> "config-2"],
    [id |-> "appearance-3", source |-> "source-2",
        symbol |-> "symbol-shared", configuration |-> "config-1"],
    [id |-> "appearance-4", source |-> "source-3",
        symbol |-> "symbol-isolated", configuration |-> "config-1"]
}

FixtureFacts == {
    [id |-> "fact-scoped", symbol |-> "symbol-shared",
        configurations |-> {"config-1"}, invariant |-> FALSE],
    [id |-> "fact-invariant", symbol |-> "symbol-shared",
        configurations |-> {"config-1", "config-2"}, invariant |-> TRUE],
    [id |-> "fact-isolated", symbol |-> "symbol-isolated",
        configurations |-> {"config-1"}, invariant |-> FALSE]
}

FixtureGeneration == [
    id |-> "generation-0",
    workspace |-> WorkspaceId,
    status |-> "current",
    inputRevision |-> "revision-0",
    toolCatalog |-> "catalog-0",
    configurations |-> {"config-1", "config-2"},
    inputs |-> {"input-source-1", "input-config-1", "input-config-2"},
    outputs |-> {"artifact-0"},
    complete |-> TRUE,
    validated |-> TRUE
]

ReplacementGeneration == [
    id |-> "generation-1",
    workspace |-> WorkspaceId,
    status |-> "extracting",
    inputRevision |-> "revision-1",
    toolCatalog |-> "catalog-0",
    configurations |-> {"config-1", "config-2"},
    inputs |-> {"input-source-1", "input-config-1", "input-config-2"},
    outputs |-> {},
    complete |-> FALSE,
    validated |-> FALSE
]

ImportedReplacement == [ReplacementGeneration EXCEPT !.status = "imported"]

CapturingReplacement == [ReplacementGeneration EXCEPT !.status = "capturing"]

FixtureGenerations == {FixtureGeneration}

GenerationById(id, records) == CHOOSE g \in records : g.id = id

ReplaceGeneration(id, replacement, records) ==
    (records \ {GenerationById(id, records)}) \cup {replacement}

RepositoryById(id) == CHOOSE r \in repositories : r.id = id
SourceById(id) == CHOOSE s \in sources : s.id = id
SymbolById(id) == CHOOSE s \in symbols : s.id = id

UniverseOfSource(sourceId) ==
    (RepositoryById(SourceById(sourceId).repository)).universe

UniverseOfSymbol(symbolId) == SymbolById(symbolId).universe

ApplicableConfigurations(symbolId) ==
    { a.configuration :
        a \in { b \in symbolAppearances : b.symbol = symbolId } }

CurrentRecords == { g \in generations : g.status = "current" }

ValidInitialState ==
    /\ Scenario = "valid"
    /\ phase = "ready"
    /\ workspace = FixtureWorkspace
    /\ universes = FixtureUniverses
    /\ repositories = FixtureRepositories
    /\ sources = FixtureSources
    /\ translationUnits = FixtureTranslationUnits
    /\ configurations = FixtureConfigurations
    /\ symbols = FixtureSymbols
    /\ symbolAppearances = FixtureAppearances
    /\ facts = FixtureFacts
    /\ generations = FixtureGenerations
    /\ currentGeneration = "generation-0"
    /\ requiredRevision = "revision-0"
    /\ invalidatedInputs = {}
    /\ readStatus = "current"

CrossUniverseConflationSeed ==
    FixtureAppearances \cup {
        [id |-> "appearance-bad", source |-> "source-3",
            symbol |-> "symbol-shared", configuration |-> "config-1"]
    }

PartialPublicationSeed == {
    [FixtureGeneration EXCEPT
        !.status = "stale"],
    [ReplacementGeneration EXCEPT
        !.status = "current"]
}

MissingInvalidationSeed == FixtureGenerations

StaleAsCurrentSeed == {
    [FixtureGeneration EXCEPT !.status = "stale"]
}

SeededState(scenario) ==
    CASE
        scenario = "cross-universe-conflation" -> [
            phase |-> "ready",
            workspace |-> FixtureWorkspace,
            universes |-> FixtureUniverses,
            repositories |-> FixtureRepositories,
            sources |-> FixtureSources,
            translationUnits |-> FixtureTranslationUnits,
            configurations |-> FixtureConfigurations,
            symbols |-> FixtureSymbols,
            symbolAppearances |-> CrossUniverseConflationSeed,
            facts |-> FixtureFacts,
            generations |-> FixtureGenerations,
            currentGeneration |-> "generation-0",
            requiredRevision |-> "revision-0",
            invalidatedInputs |-> {},
            readStatus |-> "current"]
        [] scenario = "partial-publication" -> [
            phase |-> "ready",
            workspace |-> FixtureWorkspace,
            universes |-> FixtureUniverses,
            repositories |-> FixtureRepositories,
            sources |-> FixtureSources,
            translationUnits |-> FixtureTranslationUnits,
            configurations |-> FixtureConfigurations,
            symbols |-> FixtureSymbols,
            symbolAppearances |-> FixtureAppearances,
            facts |-> FixtureFacts,
            generations |-> PartialPublicationSeed,
            currentGeneration |-> "generation-1",
            requiredRevision |-> "revision-1",
            invalidatedInputs |-> {},
            readStatus |-> "current"]
        [] scenario = "missing-invalidation" -> [
            phase |-> "ready",
            workspace |-> FixtureWorkspace,
            universes |-> FixtureUniverses,
            repositories |-> FixtureRepositories,
            sources |-> FixtureSources,
            translationUnits |-> FixtureTranslationUnits,
            configurations |-> FixtureConfigurations,
            symbols |-> FixtureSymbols,
            symbolAppearances |-> FixtureAppearances,
            facts |-> FixtureFacts,
            generations |-> MissingInvalidationSeed,
            currentGeneration |-> "generation-0",
            requiredRevision |-> "revision-1",
            invalidatedInputs |-> {"input-source-1"},
            readStatus |-> "current"]
        [] scenario = "stale-as-current" -> [
            phase |-> "ready",
            workspace |-> FixtureWorkspace,
            universes |-> FixtureUniverses,
            repositories |-> FixtureRepositories,
            sources |-> FixtureSources,
            translationUnits |-> FixtureTranslationUnits,
            configurations |-> FixtureConfigurations,
            symbols |-> FixtureSymbols,
            symbolAppearances |-> FixtureAppearances,
            facts |-> FixtureFacts,
            generations |-> StaleAsCurrentSeed,
            currentGeneration |-> "generation-0",
            requiredRevision |-> "revision-0",
            invalidatedInputs |-> {},
            readStatus |-> "current"]

Init ==
    IF Scenario = "valid"
    THEN ValidInitialState
    ELSE
        /\ Scenario \in SeedScenarioIds
        /\ phase = SeededState(Scenario).phase
        /\ workspace = SeededState(Scenario).workspace
        /\ universes = SeededState(Scenario).universes
        /\ repositories = SeededState(Scenario).repositories
        /\ sources = SeededState(Scenario).sources
        /\ translationUnits = SeededState(Scenario).translationUnits
        /\ configurations = SeededState(Scenario).configurations
        /\ symbols = SeededState(Scenario).symbols
        /\ symbolAppearances = SeededState(Scenario).symbolAppearances
        /\ facts = SeededState(Scenario).facts
        /\ generations = SeededState(Scenario).generations
        /\ currentGeneration = SeededState(Scenario).currentGeneration
        /\ requiredRevision = SeededState(Scenario).requiredRevision
        /\ invalidatedInputs = SeededState(Scenario).invalidatedInputs
        /\ readStatus = SeededState(Scenario).readStatus

ImportReplacement ==
    /\ Scenario = "valid"
    /\ phase = "ready"
    /\ currentGeneration = "generation-0"
    /\ readStatus = "current"
    /\ "generation-1" \notin {g.id : g \in generations}
    /\ phase' = "imported"
    /\ generations' = generations \cup {ImportedReplacement}
    /\ UNCHANGED <<workspace, universes, repositories, sources,
        translationUnits, configurations, symbols, symbolAppearances, facts,
        currentGeneration, requiredRevision, invalidatedInputs, readStatus>>

CaptureConfiguration ==
    /\ phase = "imported"
    /\ \E g \in generations : g.id = "generation-1" /\ g.status = "imported"
    /\ phase' = "capturing"
    /\ generations' = ReplaceGeneration(
        "generation-1",
        [GenerationById("generation-1", generations) EXCEPT !.status = "capturing"],
        generations)
    /\ UNCHANGED <<workspace, universes, repositories, sources,
        translationUnits, configurations, symbols, symbolAppearances, facts,
        currentGeneration, requiredRevision, invalidatedInputs, readStatus>>

StartExtraction ==
    /\ Scenario = "valid"
    /\ phase = "capturing"
    /\ currentGeneration = "generation-0"
    /\ readStatus = "current"
    /\ \E g \in generations : g.id = "generation-1" /\ g.status = "capturing"
    /\ phase' = "building"
    /\ generations' = ReplaceGeneration(
        "generation-0",
        [GenerationById("generation-0", generations) EXCEPT !.status = "stale"],
        generations) \cup {
            [GenerationById("generation-1", generations) EXCEPT !.status = "extracting"]
        }
    /\ currentGeneration' = NoCurrent
    /\ requiredRevision' = "revision-1"
    /\ invalidatedInputs' = {"input-source-1"}
    /\ readStatus' = "stale"
    /\ UNCHANGED <<workspace, universes, repositories, sources,
        translationUnits, configurations, symbols, symbolAppearances, facts>>

BeginValidation ==
    /\ phase = "building"
    /\ \E g \in generations : g.id = "generation-1" /\ g.status = "extracting"
    /\ phase' = "validating"
    /\ generations' = ReplaceGeneration(
        "generation-1",
        [GenerationById("generation-1", generations) EXCEPT
            !.status = "validating",
            !.complete = TRUE,
            !.validated = TRUE],
        generations)
    /\ readStatus' = "partial"
    /\ UNCHANGED <<workspace, universes, repositories, sources,
        translationUnits, configurations, symbols, symbolAppearances, facts,
        currentGeneration, requiredRevision, invalidatedInputs>>

PublishReplacement ==
    /\ phase = "validating"
    /\ \E g \in generations :
        /\ g.id = "generation-1"
        /\ g.status = "validating"
        /\ g.complete
        /\ g.validated
        /\ g.inputRevision = requiredRevision
    /\ phase' = "published"
    /\ generations' = ReplaceGeneration(
        "generation-1",
        [GenerationById("generation-1", generations) EXCEPT
            !.status = "published",
            !.outputs = {"artifact-1"}],
        generations)
    /\ currentGeneration' = NoCurrent
    /\ requiredRevision' = "revision-1"
    /\ invalidatedInputs' = {"input-source-1"}
    /\ readStatus' = "stale"
    /\ UNCHANGED <<workspace, universes, repositories, sources,
        translationUnits, configurations, symbols, symbolAppearances, facts>>

MakeCurrent ==
    /\ phase = "published"
    /\ \E g \in generations :
        /\ g.id = "generation-1"
        /\ g.status = "published"
        /\ g.complete
        /\ g.validated
        /\ g.inputRevision = requiredRevision
    /\ phase' = "ready"
    /\ generations' = ReplaceGeneration(
        "generation-1",
        [GenerationById("generation-1", generations) EXCEPT !.status = "current"],
        generations)
    /\ currentGeneration' = "generation-1"
    /\ requiredRevision' = "revision-1"
    /\ invalidatedInputs' = {}
    /\ readStatus' = "current"
    /\ UNCHANGED <<workspace, universes, repositories, sources,
        translationUnits, configurations, symbols, symbolAppearances, facts>>

FailReplacement ==
    /\ phase = "building" \/ phase = "validating"
    /\ \E g \in generations : g.id = "generation-1" /\ g.status \in {"extracting", "validating"}
    /\ phase' = "ready"
    /\ generations' = ReplaceGeneration(
        "generation-1",
        [GenerationById("generation-1", generations) EXCEPT !.status = "failed"],
        generations)
    /\ currentGeneration' = NoCurrent
    /\ invalidatedInputs' = {"input-source-1"}
    /\ readStatus' = "unavailable"
    /\ UNCHANGED <<workspace, universes, repositories, sources,
        translationUnits, configurations, symbols, symbolAppearances, facts,
        requiredRevision>>

RetireOldGeneration ==
    /\ phase = "ready"
    /\ currentGeneration = "generation-1"
    /\ \E g \in generations : g.id = "generation-0" /\ g.status = "stale"
    /\ generations' = ReplaceGeneration(
        "generation-0",
        [GenerationById("generation-0", generations) EXCEPT !.status = "retired"],
        generations)
    /\ UNCHANGED <<phase, workspace, universes, repositories, sources,
        translationUnits, configurations, symbols, symbolAppearances, facts,
        currentGeneration, requiredRevision, invalidatedInputs, readStatus>>

LifecycleStep == ImportReplacement \/ CaptureConfiguration \/ StartExtraction
    \/ BeginValidation \/ PublishReplacement \/ MakeCurrent \/ FailReplacement
    \/ RetireOldGeneration

NoOp == UNCHANGED vars

Next == LifecycleStep \/ NoOp

Fairness == WF_vars(LifecycleStep)

Spec == Init /\ [][Next]_vars /\ Fairness

LifecycleTypeInvariant ==
    /\ workspace.id = WorkspaceId
    /\ workspace.universes = UniverseIds
    /\ \A u \in universes : u.id \in UniverseIds /\ u.workspace = WorkspaceId
    /\ \A r \in repositories : r.id \in RepositoryIds /\ r.universe \in UniverseIds
    /\ \A s \in sources : s.id \in SourceIds /\ s.repository \in RepositoryIds
    /\ \A tu \in translationUnits :
        /\ tu.id \in TranslationUnitIds
        /\ tu.source \in SourceIds
        /\ tu.configuration \in ConfigurationIds
    /\ \A c \in configurations :
        /\ c.id \in ConfigurationIds
        /\ c.descriptor \in STRING
    /\ \A s \in symbols :
        /\ s.id \in SymbolIds
        /\ s.universe \in UniverseIds
        /\ s.kind \in SymbolKinds
    /\ \A g \in generations :
        /\ g.id \in GenerationIds
        /\ g.workspace = WorkspaceId
        /\ g.status \in GenerationStatuses
        /\ g.inputRevision \in RevisionIds
        /\ g.toolCatalog \in ToolCatalogIds
        /\ g.configurations \subseteq ConfigurationIds
        /\ g.inputs \subseteq InputIds
        /\ g.outputs \subseteq ArtifactIds
        /\ g.complete \in BOOLEAN
        /\ g.validated \in BOOLEAN
    /\ readStatus \in ReadStatuses

WorkspaceContainmentInvariant ==
    /\ \A r \in repositories : r.id \in workspace.repositories
    /\ \A s \in sources : s.id \in workspace.sources
    /\ \A tu \in translationUnits : tu.id \in workspace.translationUnits
    /\ \A c \in configurations : c.id \in workspace.configurations
    /\ \A g \in generations : g.id \in workspace.generations
    /\ \A tu \in translationUnits :
        /\ \E s \in sources : s.id = tu.source
        /\ \E c \in configurations : c.id = tu.configuration

ScopedSymbolIdentityInvariant ==
    /\ \A a \in symbolAppearances :
        /\ \E s \in sources :
            /\ \E sym \in symbols :
                /\ s.id = a.source
                /\ sym.id = a.symbol
                /\ UniverseOfSource(s.id) = UniverseOfSymbol(sym.id)
    /\ \A sym1 \in symbols :
        \A sym2 \in symbols :
            (sym1.usr = sym2.usr /\ sym1.universe = sym2.universe)
                => sym1.id = sym2.id

ConfigurationApplicabilityInvariant ==
    /\ \A a \in symbolAppearances :
        /\ \E tu \in translationUnits :
            tu.source = a.source /\ tu.configuration = a.configuration
    /\ \A f \in facts :
        /\ f.configurations # {}
        /\ f.configurations \subseteq ApplicableConfigurations(f.symbol)
        /\ (f.invariant => f.configurations = ApplicableConfigurations(f.symbol))

NoPartialPublicationInvariant ==
    \A g \in generations :
        g.status = "current" =>
            /\ g.complete
            /\ g.validated
            /\ g.outputs # {}

GenerationPublicationInvariant ==
    /\ Cardinality(CurrentRecords) <= 1
    /\ (currentGeneration = NoCurrent => CurrentRecords = {})
    /\ (currentGeneration # NoCurrent =>
        /\ currentGeneration \in GenerationIds
        /\ \E g \in CurrentRecords : g.id = currentGeneration)
    /\ (readStatus = "current" =>
        /\ currentGeneration # NoCurrent
        /\ \E g \in generations :
            /\ g.id = currentGeneration
            /\ g.status = "current"
            /\ g.complete
            /\ g.validated)

FailedGenerationInvariant ==
    \A g \in generations : g.status = "failed" => g.id # currentGeneration

InvalidationInvariant ==
    readStatus = "current" =>
        /\ currentGeneration # NoCurrent
        /\ \E g \in generations :
            /\ g.id = currentGeneration
            /\ g.status = "current"
            /\ g.inputRevision = requiredRevision
        /\ invalidatedInputs = {}

ReadHonestyInvariant ==
    /\ (readStatus = "current" => currentGeneration # NoCurrent)
    /\ (currentGeneration = NoCurrent =>
        readStatus \in {"stale", "partial", "unavailable"})
    /\ (readStatus = "partial" => currentGeneration = NoCurrent)

ProtectedInvariant ==
    /\ ProtectedGenerationStatuses(generations)
    /\ ScopedSymbolIdentityInvariant
    /\ NoPartialPublicationInvariant
    /\ InvalidationInvariant

=============================================================================
