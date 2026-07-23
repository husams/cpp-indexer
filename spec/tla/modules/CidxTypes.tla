------------------------------ MODULE CidxTypes ------------------------------

EXTENDS Naturals, Sequences

(***************************************************************************)
(* CIDX's specification vocabulary.  These are abstract identifiers and    *)
(* records; no C++, SQLite, compiler, or file-layout representation is a   *)
(* semantic axiom.  Concrete finite sets are supplied by model configs.     *)
(***************************************************************************)

CONSTANTS
    WorkspaceIds,
    RepositoryIds,
    ComponentIds,
    SourceIds,
    TranslationUnitIds,
    ConfigurationIds,
    SymbolIds,
    TypeIds,
    RelationIds,
    EvidenceIds,
    GenerationIds,
    TransformIds,
    ArtifactIds,
    QueryIds,
    ResultIds

WorkspaceKinds == {"workspace"}
RepositoryKinds == {"repository"}
ComponentKinds == {"component"}
SourceKinds == {"source", "header"}
TranslationUnitKinds == {"translation-unit"}
ConfigurationKinds == {"configuration"}
SymbolKinds == {"namespace", "record", "function", "method", "field", "variable"}
TypeKinds == {"builtin", "record", "enum", "pointer", "reference", "function"}
RelationKinds == {"contains", "declares", "uses", "calls", "inherits", "defines"}
EvidenceKinds == {"source", "test", "trace", "model", "assumption"}
GenerationKinds == {"index", "resolve", "query"}
TransformKinds == {"parse", "extract", "resolve", "materialize", "answer"}
ArtifactKinds == {"index", "graph", "result", "diagnostic"}
QueryKinds == {"search", "navigation", "impact", "verification"}
TrustLevels == {"observed", "derived", "trusted-assumption"}

(* One definition, reused by every later specification module. *)
ResultStatuses == {
    "suggested",
    "observed",
    "inferred",
    "bounded-verified",
    "proved-under-assumptions",
    "refuted",
    "unknown"
}

ElementIds == WorkspaceIds
    \cup RepositoryIds
    \cup ComponentIds
    \cup SourceIds
    \cup TranslationUnitIds
    \cup ConfigurationIds
    \cup SymbolIds
    \cup TypeIds
    \cup RelationIds
    \cup EvidenceIds
    \cup GenerationIds
    \cup TransformIds
    \cup ArtifactIds
    \cup QueryIds
    \cup ResultIds

IsWorkspace(w) == w \in [
    id: WorkspaceIds,
    repositories: SUBSET RepositoryIds,
    components: SUBSET ComponentIds,
    sources: SUBSET SourceIds,
    translationUnits: SUBSET TranslationUnitIds,
    configurations: SUBSET ConfigurationIds,
    generations: SUBSET GenerationIds
]

IsRepository(r) == r \in [
    id: RepositoryIds,
    workspace: WorkspaceIds,
    components: SUBSET ComponentIds,
    sources: SUBSET SourceIds
]

IsComponent(c) == c \in [
    id: ComponentIds,
    repository: RepositoryIds,
    sources: SUBSET SourceIds,
    translationUnits: SUBSET TranslationUnitIds
]

IsSource(s) == s \in [
    id: SourceIds,
    repository: RepositoryIds,
    component: ComponentIds,
    kind: SourceKinds
]

IsTranslationUnit(tu) == tu \in [
    id: TranslationUnitIds,
    source: SourceIds,
    configuration: ConfigurationIds
]

IsConfiguration(c) == c \in [
    id: ConfigurationIds,
    driver: STRING,
    flags: Seq(STRING)
]

IsSymbol(s) == s \in [
    id: SymbolIds,
    source: SourceIds,
    kind: SymbolKinds,
    name: STRING
]

IsType(t) == t \in [
    id: TypeIds,
    kind: TypeKinds,
    spelling: STRING
]

IsRelation(r) == r \in [
    id: RelationIds,
    kind: RelationKinds,
    source: ElementIds,
    target: ElementIds,
    evidence: SUBSET EvidenceIds
]

IsEvidence(e) == e \in [
    id: EvidenceIds,
    kind: EvidenceKinds,
    source: SourceIds,
    line: Nat,
    trust: TrustLevels
]

IsGeneration(g) == g \in [
    id: GenerationIds,
    kind: GenerationKinds,
    transform: TransformIds,
    inputs: SUBSET ElementIds,
    outputs: SUBSET ArtifactIds,
    status: ResultStatuses
]

IsTransform(t) == t \in [
    id: TransformIds,
    kind: TransformKinds,
    version: STRING,
    generated: BOOLEAN
]

IsArtifact(a) == a \in [
    id: ArtifactIds,
    kind: ArtifactKinds,
    generation: GenerationIds,
    contentHash: STRING
]

IsQuery(q) == q \in [
    id: QueryIds,
    kind: QueryKinds,
    target: ElementIds
]

IsResult(r) == r \in [
    id: ResultIds,
    query: QueryIds,
    status: ResultStatuses,
    evidence: SUBSET EvidenceIds,
    assumptions: SUBSET EvidenceIds
]

=============================================================================
