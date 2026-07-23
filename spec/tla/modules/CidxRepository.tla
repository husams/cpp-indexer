--------------------------- MODULE CidxRepository ---------------------------

EXTENDS CidxTypes, FiniteSets, CidxProtected

CONSTANTS
    WorkspaceId,
    RepositoryId,
    ComponentId,
    SourceId,
    TranslationUnitId,
    ConfigurationId,
    SymbolId,
    TypeId,
    RelationId,
    EvidenceId,
    GenerationId,
    TransformId,
    ArtifactId,
    QueryId,
    ResultId

VARIABLES
    phase,
    workspaces,
    repositories,
    components,
    sources,
    translationUnits,
    configurations,
    symbols,
    types,
    relations,
    evidences,
    generations,
    transforms,
    artifacts,
    queries,
    results

vars == <<
    phase,
    workspaces,
    repositories,
    components,
    sources,
    translationUnits,
    configurations,
    symbols,
    types,
    relations,
    evidences,
    generations,
    transforms,
    artifacts,
    queries,
    results
>>

FixtureWorkspace == [
    id |-> WorkspaceId,
    repositories |-> {RepositoryId},
    components |-> {ComponentId},
    sources |-> {SourceId},
    translationUnits |-> {TranslationUnitId},
    configurations |-> {ConfigurationId},
    generations |-> {GenerationId}
]

FixtureRepository == [
    id |-> RepositoryId,
    workspace |-> WorkspaceId,
    components |-> {ComponentId},
    sources |-> {SourceId}
]

FixtureComponent == [
    id |-> ComponentId,
    repository |-> RepositoryId,
    sources |-> {SourceId},
    translationUnits |-> {TranslationUnitId}
]

FixtureSource == [
    id |-> SourceId,
    repository |-> RepositoryId,
    component |-> ComponentId,
    kind |-> "source"
]

FixtureTranslationUnit == [
    id |-> TranslationUnitId,
    source |-> SourceId,
    configuration |-> ConfigurationId
]

FixtureConfiguration == [
    id |-> ConfigurationId,
    driver |-> "abstract-driver",
    flags |-> <<"-std=abstract">>
]

FixtureSymbol == [
    id |-> SymbolId,
    source |-> SourceId,
    kind |-> "function",
    name |-> "fixture"
]

FixtureType == [
    id |-> TypeId,
    kind |-> "builtin",
    spelling |-> "abstract-type"
]

FixtureRelation == [
    id |-> RelationId,
    kind |-> "uses",
    source |-> SymbolId,
    target |-> TypeId,
    evidence |-> {EvidenceId}
]

FixtureEvidence == [
    id |-> EvidenceId,
    kind |-> "source",
    source |-> SourceId,
    line |-> 1,
    trust |-> "derived"
]

FixtureGeneration == [
    id |-> GenerationId,
    kind |-> "index",
    transform |-> TransformId,
    inputs |-> {TranslationUnitId},
    outputs |-> {ArtifactId},
    status |-> "observed"
]

FixtureTransform == [
    id |-> TransformId,
    kind |-> "extract",
    version |-> "fixture-1",
    generated |-> FALSE
]

FixtureArtifact == [
    id |-> ArtifactId,
    kind |-> "index",
    generation |-> GenerationId,
    contentHash |-> "fixture-hash"
]

FixtureQuery == [
    id |-> QueryId,
    kind |-> "navigation",
    target |-> SymbolId
]

FixtureResult == [
    id |-> ResultId,
    query |-> QueryId,
    status |-> "observed",
    evidence |-> {EvidenceId},
    assumptions |-> {}
]

Init ==
    /\ phase = "empty"
    /\ workspaces = {}
    /\ repositories = {}
    /\ components = {}
    /\ sources = {}
    /\ translationUnits = {}
    /\ configurations = {}
    /\ symbols = {}
    /\ types = {}
    /\ relations = {}
    /\ evidences = {}
    /\ generations = {}
    /\ transforms = {}
    /\ artifacts = {}
    /\ queries = {}
    /\ results = {}

InitializeRepository ==
    /\ phase = "empty"
    /\ phase' = "ready"
    /\ workspaces' = {FixtureWorkspace}
    /\ repositories' = {FixtureRepository}
    /\ components' = {FixtureComponent}
    /\ sources' = {FixtureSource}
    /\ translationUnits' = {FixtureTranslationUnit}
    /\ configurations' = {FixtureConfiguration}
    /\ symbols' = {FixtureSymbol}
    /\ types' = {FixtureType}
    /\ relations' = {FixtureRelation}
    /\ evidences' = {FixtureEvidence}
    /\ generations' = {FixtureGeneration}
    /\ transforms' = {FixtureTransform}
    /\ artifacts' = {FixtureArtifact}
    /\ queries' = {FixtureQuery}
    /\ results' = {FixtureResult}

NoOp ==
    /\ phase = "ready"
    /\ UNCHANGED vars

Next == InitializeRepository \/ NoOp

Fairness == WF_vars(InitializeRepository)

Spec == Init /\ [][Next]_vars /\ Fairness

(* Every value in the repository state is one of the shared abstract types. *)
TypeInvariant ==
    /\ \A w \in workspaces : IsWorkspace(w)
    /\ \A r \in repositories : IsRepository(r)
    /\ \A c \in components : IsComponent(c)
    /\ \A s \in sources : IsSource(s)
    /\ \A tu \in translationUnits : IsTranslationUnit(tu)
    /\ \A c \in configurations : IsConfiguration(c)
    /\ \A s \in symbols : IsSymbol(s)
    /\ \A t \in types : IsType(t)
    /\ \A r \in relations : IsRelation(r)
    /\ \A e \in evidences : IsEvidence(e)
    /\ \A g \in generations : IsGeneration(g)
    /\ \A t \in transforms : IsTransform(t)
    /\ \A a \in artifacts : IsArtifact(a)
    /\ \A q \in queries : IsQuery(q)
    /\ \A r \in results : IsResult(r)

RepositoryStructureInvariant ==
    \/ phase = "empty"
    \/ ( /\ phase = "ready"
        /\ workspaces = {FixtureWorkspace}
        /\ repositories = {FixtureRepository}
        /\ components = {FixtureComponent}
        /\ sources = {FixtureSource}
        /\ translationUnits = {FixtureTranslationUnit}
        /\ configurations = {FixtureConfiguration}
        /\ symbols = {FixtureSymbol}
        /\ types = {FixtureType}
        /\ relations = {FixtureRelation}
        /\ evidences = {FixtureEvidence}
        /\ generations = {FixtureGeneration}
        /\ transforms = {FixtureTransform}
        /\ artifacts = {FixtureArtifact}
        /\ queries = {FixtureQuery}
        /\ results = {FixtureResult} )

SharedStatusInvariant ==
    /\ \A g \in generations : g.status \in ResultStatuses
    /\ \A r \in results : r.status \in ResultStatuses

ProtectedInvariant ==
    /\ ProtectedStatuses(generations)
    /\ ProtectedStatuses(results)
    /\ ProtectedEvidence(evidences)

=============================================================================
