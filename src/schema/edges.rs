/// CodexGraph edge kinds (AC-M1-3, AC-M2-7..AC-M2-12, AC-M4-2, AC-M5-2).
///
/// M1 base: `CONTAINS`, `HAS_METHOD`, `HAS_FIELD`, `INHERITS`, `USES`, `CALLS`.
/// M2 extensions (S14): `INCLUDES`, `OVERRIDES`, `INSTANTIATES`, `SPECIALIZES`, `FRIEND_OF`,
/// `ADL_CANDIDATE`.
/// M4 additions (S22): `BELONGS_TO_REPO`.  `EXTERNAL_REF` is added in S23.
/// M5 additions (S26): `EXPANDS_TO`.
/// v7 S2 additions: `RETURNS`, `HAS_PARAM`, `OF_TYPE`, `POINTS_TO`, `REFERS_TO`.
/// v7 S3 additions: `TEMPLATE_PARAM`, `TEMPLATE_ARG`, `CONSTRAINED_BY`.
/// v7 S5 additions: `ENUMERATOR_OF`, `UNDERLYING_TYPE`, `ALIAS_OF`, `USES_NAMESPACE`, `USES_DECLARATION`.
///
/// Adding new variants bumps `SCHEMA_VERSION` per ADR-9.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "SCREAMING_SNAKE_CASE")]
pub enum EdgeKind {
    // ── M1 base ─────────────────────────────────────────────────────────────
    /// A lexical containment relationship (e.g., class contains method, namespace contains class).
    Contains,
    /// A class directly owns a method declaration.
    HasMethod,
    /// A class directly owns a field declaration.
    HasField,
    /// Direct (non-virtual) or virtual base-class inheritance.
    Inherits,
    /// A type reference / type usage relationship (e.g., parameter type, return type, field type).
    Uses,
    /// A direct call from one function/method to another.
    Calls,

    // ── M2 extensions (S14) ─────────────────────────────────────────────────
    /// A translation unit (MODULE) or header (HEADER) includes another header.  AC-M2-7.
    Includes,
    /// A virtual method overrides a base-class virtual method; `vtable_slot` in attrs.  AC-M2-8.
    Overrides,
    /// A call site or context that instantiates a template.  AC-M2-9.
    Instantiates,
    /// A template specialization refers to its primary template.  AC-M2-10.
    Specializes,
    /// A class grants friend access to another entity.  AC-M2-11.
    FriendOf,
    /// An unresolved name that is a candidate for Argument-Dependent Lookup.  AC-M2-12.
    AdlCandidate,

    // ── M4 additions (S22) ──────────────────────────────────────────────────
    /// Links every indexed node to its owning REPO node.  AC-M4-2.
    ///
    /// Direction: `(node)-[:BELONGS_TO_REPO]->(repo)`.
    BelongsToRepo,

    // ── M4 additions (S23) ──────────────────────────────────────────────────
    /// A cross-repository reference resolved during Phase 5.  AC-M4-4.
    ///
    /// Direction: `(src_node)-[:EXTERNAL_REF {via: <orig_edge_kind>}]->(dst_node)`.
    /// `src_node` and `dst_node` live in different REPO nodes.
    /// `attrs_json` carries `{"via": "<ORIG_EDGE_KIND>"}`.
    ExternalRef,

    // ── M5 additions (S26) ──────────────────────────────────────────────────
    /// A top-level macro expansion at a call site.  AC-M5-2.
    ///
    /// Direction: `(call_site)-[:EXPANDS_TO]->(macro_def)`.
    /// `call_site` is the enclosing function/method USR, or the Module USR for
    /// file-scope expansions.  `macro_def` is a `MACRO` node.
    /// Only top-level expansions are emitted; nested macro expansions inside
    /// another macro's body are suppressed to prevent edge explosion (AC-M5-3).
    ExpandsTo,

    // ── v7 S2 additions ─────────────────────────────────────────────────────
    /// A function/method returns a type.
    ///
    /// Direction: `(Function|Method)-[:RETURNS]->(Type)`.
    /// Always emitted, including void returns and 0-param functions (S2-03).
    Returns,
    /// A function/method has a parameter (ordered).
    ///
    /// Direction: `(Function|Method)-[:HAS_PARAM {edge_index}]->(Parameter)`.
    /// `edge_index` mirrors target Parameter `param_index` (ADR-5).
    HasParam,
    /// A Parameter or Field has a type.
    ///
    /// Direction: `(Parameter|Field)-[:OF_TYPE]->(Type)`.
    OfType,
    /// A pointer type points to its pointee type.
    ///
    /// Direction: `(Type:ptr)-[:POINTS_TO]->(Type pointee)`.
    /// One declarator level only (ADR-2).
    PointsTo,
    /// A reference type refers to its referent type.
    ///
    /// Direction: `(Type:ref)-[:REFERS_TO]->(Type referent)`.
    /// One declarator level only (ADR-2).
    RefersTo,

    // ── v7 S3 additions ─────────────────────────────────────────────────────
    /// A template definition has a template parameter (ordered by `edge_index`).
    ///
    /// Direction: `(TemplateDef)-[:TEMPLATE_PARAM {edge_index}]->(Parameter)`.
    /// `Parameter.param_kind` is `"type"` | `"non_type"` | `"template"`.
    /// `edge_index` mirrors the target Parameter's `param_index`.
    TemplateParam,
    /// A template specialization has a positional template argument (ADR-5).
    ///
    /// Direction: `(Specialization)-[:TEMPLATE_ARG {edge_index}]->(TemplateArg)`.
    /// Each `TemplateArg` is a distinct positional node (distinct `in_id`) so
    /// IndraDB edge-identity `(out,type,in)` never collapses duplicate-type args.
    /// `edge_index` mirrors the target `TemplateArg.param_index`.
    TemplateArg,
    /// A template parameter or template definition is constrained by a concept
    /// (C++20 only; ADR-7).
    ///
    /// Direction: `(template_or_param)-[:CONSTRAINED_BY]->(Concept)`.
    /// Only emitted when the TU is parsed in C++20 mode and libclang exposes a
    /// `ConceptDecl` cursor.  Pre-C++20 SFINAE constraints are NOT modelled.
    ConstrainedBy,

    // ── v7 S5 additions ─────────────────────────────────────────────────────
    /// An enumerator constant belongs to its parent enum.
    ///
    /// Direction: `(Enumerator)-[:ENUMERATOR_OF]->(Enum)`.
    EnumeratorOf,
    /// An enum has an explicit underlying integer type.
    ///
    /// Direction: `(Enum)-[:UNDERLYING_TYPE]->(Type)`.
    /// Only emitted when an explicit underlying type is present.
    UnderlyingType,
    /// A typedef/type-alias is an alias for another type.
    ///
    /// Direction: `(Typedef)-[:ALIAS_OF]->(Type)`.
    /// Emitted per chain link (ADR-2 written-spelling); a chain `A→B→C→int`
    /// produces three distinct ALIAS_OF edges, one per typedef node.
    AliasOf,
    /// A `using namespace` directive introduces a namespace into the current scope.
    ///
    /// Direction: `(scope)-[:USES_NAMESPACE]->(Namespace)`.
    UsesNamespace,
    /// A `using` declaration introduces a specific name from another scope.
    ///
    /// Direction: `(scope)-[:USES_DECLARATION]->(symbol)`.
    /// Distinct from `UsesNamespace` (EC-04).  For `using Base::method`:
    /// dual-emitted alongside the existing OVERRIDES/HAS_METHOD path (OQ-8).
    UsesDeclaration,
}

impl EdgeKind {
    /// Canonical string representation stored in the Arrow `kind` column.
    pub fn as_str(self) -> &'static str {
        match self {
            EdgeKind::Contains => "CONTAINS",
            EdgeKind::HasMethod => "HAS_METHOD",
            EdgeKind::HasField => "HAS_FIELD",
            EdgeKind::Inherits => "INHERITS",
            EdgeKind::Uses => "USES",
            EdgeKind::Calls => "CALLS",
            EdgeKind::Includes => "INCLUDES",
            EdgeKind::Overrides => "OVERRIDES",
            EdgeKind::Instantiates => "INSTANTIATES",
            EdgeKind::Specializes => "SPECIALIZES",
            EdgeKind::FriendOf => "FRIEND_OF",
            EdgeKind::AdlCandidate => "ADL_CANDIDATE",
            EdgeKind::BelongsToRepo => "BELONGS_TO_REPO",
            EdgeKind::ExternalRef => "EXTERNAL_REF",
            EdgeKind::ExpandsTo => "EXPANDS_TO",
            // v7 S2:
            EdgeKind::Returns => "RETURNS",
            EdgeKind::HasParam => "HAS_PARAM",
            EdgeKind::OfType => "OF_TYPE",
            EdgeKind::PointsTo => "POINTS_TO",
            EdgeKind::RefersTo => "REFERS_TO",
            // v7 S3:
            EdgeKind::TemplateParam => "TEMPLATE_PARAM",
            EdgeKind::TemplateArg => "TEMPLATE_ARG",
            EdgeKind::ConstrainedBy => "CONSTRAINED_BY",
            // v7 S5:
            EdgeKind::EnumeratorOf => "ENUMERATOR_OF",
            EdgeKind::UnderlyingType => "UNDERLYING_TYPE",
            EdgeKind::AliasOf => "ALIAS_OF",
            EdgeKind::UsesNamespace => "USES_NAMESPACE",
            EdgeKind::UsesDeclaration => "USES_DECLARATION",
        }
    }

    /// All variants, in declaration order. Used to build Arrow dictionary values.
    pub fn all() -> &'static [EdgeKind] {
        &[
            EdgeKind::Contains,
            EdgeKind::HasMethod,
            EdgeKind::HasField,
            EdgeKind::Inherits,
            EdgeKind::Uses,
            EdgeKind::Calls,
            EdgeKind::Includes,
            EdgeKind::Overrides,
            EdgeKind::Instantiates,
            EdgeKind::Specializes,
            EdgeKind::FriendOf,
            EdgeKind::AdlCandidate,
            EdgeKind::BelongsToRepo,
            EdgeKind::ExternalRef,
            EdgeKind::ExpandsTo,
            // v7 S2:
            EdgeKind::Returns,
            EdgeKind::HasParam,
            EdgeKind::OfType,
            EdgeKind::PointsTo,
            EdgeKind::RefersTo,
            // v7 S3:
            EdgeKind::TemplateParam,
            EdgeKind::TemplateArg,
            EdgeKind::ConstrainedBy,
            // v7 S5:
            EdgeKind::EnumeratorOf,
            EdgeKind::UnderlyingType,
            EdgeKind::AliasOf,
            EdgeKind::UsesNamespace,
            EdgeKind::UsesDeclaration,
        ]
    }

    /// Parse from the Arrow string representation. Returns `None` for unknown variants.
    ///
    /// Unlike `std::str::FromStr`, this returns `Option` rather than `Result` because
    /// an unknown variant is always a logic error (not a user input error) and callers
    /// handle it with `expect` / `panic` rather than user-visible error messages.
    pub fn try_from_arrow_str(s: &str) -> Option<EdgeKind> {
        match s {
            "CONTAINS" => Some(EdgeKind::Contains),
            "HAS_METHOD" => Some(EdgeKind::HasMethod),
            "HAS_FIELD" => Some(EdgeKind::HasField),
            "INHERITS" => Some(EdgeKind::Inherits),
            "USES" => Some(EdgeKind::Uses),
            "CALLS" => Some(EdgeKind::Calls),
            "INCLUDES" => Some(EdgeKind::Includes),
            "OVERRIDES" => Some(EdgeKind::Overrides),
            "INSTANTIATES" => Some(EdgeKind::Instantiates),
            "SPECIALIZES" => Some(EdgeKind::Specializes),
            "FRIEND_OF" => Some(EdgeKind::FriendOf),
            "ADL_CANDIDATE" => Some(EdgeKind::AdlCandidate),
            "BELONGS_TO_REPO" => Some(EdgeKind::BelongsToRepo),
            "EXTERNAL_REF" => Some(EdgeKind::ExternalRef),
            "EXPANDS_TO" => Some(EdgeKind::ExpandsTo),
            // v7 S2:
            "RETURNS" => Some(EdgeKind::Returns),
            "HAS_PARAM" => Some(EdgeKind::HasParam),
            "OF_TYPE" => Some(EdgeKind::OfType),
            "POINTS_TO" => Some(EdgeKind::PointsTo),
            "REFERS_TO" => Some(EdgeKind::RefersTo),
            // v7 S3:
            "TEMPLATE_PARAM" => Some(EdgeKind::TemplateParam),
            "TEMPLATE_ARG" => Some(EdgeKind::TemplateArg),
            "CONSTRAINED_BY" => Some(EdgeKind::ConstrainedBy),
            // v7 S5:
            "ENUMERATOR_OF" => Some(EdgeKind::EnumeratorOf),
            "UNDERLYING_TYPE" => Some(EdgeKind::UnderlyingType),
            "ALIAS_OF" => Some(EdgeKind::AliasOf),
            "USES_NAMESPACE" => Some(EdgeKind::UsesNamespace),
            "USES_DECLARATION" => Some(EdgeKind::UsesDeclaration),
            _ => None,
        }
    }
}

impl std::fmt::Display for EdgeKind {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.as_str())
    }
}

/// A single edge record, matching the `edges.parquet` schema from ADR-3.
///
/// M8 (S40): two new optional columns promoted for USES edges per ADR-11/ADR-13.
/// Populated by the access classifier (S43); `None` for all non-USES edge kinds.
///
/// graph-symbol-ids (Story 3, v6): three new integer ID / routing fields.
/// - `src_id`: per-repo integer for `src_usr`; from source repo's `SymbolAllocator`.
/// - `dst_id`: per-repo integer for `dst_usr`; `None` mirrors `dst_usr: None` skip rule.
///   For intra-repo edges, source repo's map; for EXTERNAL_REF, destination repo's map.
/// - `dst_repo_name`: destination endpoint's repo name; `== repo_name` for intra-repo edges;
///   the cross-repo target for EXTERNAL_REF (ADR-1 point 5).
///
/// Sinks write `src_id`/`dst_id`/`dst_repo_name` and drop `src_usr`/`dst_usr`.
#[derive(Debug, Clone, PartialEq, serde::Serialize, serde::Deserialize)]
pub struct EdgeRecord {
    /// Source node USR.
    pub src_usr: String,
    /// Destination node USR; `None` when unresolved at emit time (Phase 1).
    pub dst_usr: Option<String>,
    /// Spelling captured by libclang when no USR is available; `None` otherwise.
    pub dst_placeholder: Option<String>,
    /// Edge kind.
    pub kind: EdgeKind,
    /// Set to `true` by Phase 3 when the destination USR is found in the current repo's USR map.
    pub resolved: bool,
    /// Set to `true` by Phase 3 when the destination USR is absent from the current repo's map,
    /// indicating a potential cross-repo reference.
    pub cross_repo_candidate: bool,
    /// Repository name from `[repo].name` in the config.
    pub repo_name: String,
    /// Edge attributes serialised as canonical JSON (vtable_slot, access, virtual, via…).
    pub attrs_json: String,
    /// Blake3 hash of the originating TU.
    pub tu_hash: [u8; 32],

    // ── M8 promoted fields (S40) ─────────────────────────────────────────────
    /// USES edge source access kind (e.g. `"read"`, `"write"`, `"call_arg"`).
    /// `Some` for USES edges only; `None` for all other edge kinds.
    /// Populated by `AccessKind::as_str()` from the access classifier (S43).
    pub source_association_type: Option<String>,
    /// USES edge target access kind.
    /// `Some` for USES edges only; `None` for all other edge kinds.
    pub target_association_type: Option<String>,

    // ── graph-symbol-ids integer ID fields (Story 3, v6) ─────────────────────
    /// Per-repo integer ID for `src_usr`; from source repo's `SymbolAllocator`.
    pub src_id: i64,
    /// Per-repo integer ID for `dst_usr`; `None` mirrors `dst_usr: None` skip rule.
    /// For intra-repo edges: source repo's `SymbolAllocator`.
    /// For EXTERNAL_REF: destination repo's `SymbolAllocator`.
    pub dst_id: Option<i64>,
    /// Destination endpoint's repository name.
    /// `== repo_name` for intra-repo edges; the cross-repo target repo for EXTERNAL_REF.
    pub dst_repo_name: String,

    // ── v7 S1 promoted fields ─────────────────────────────────────────────────
    /// C++ access specifier on `HasMethod`, `HasField`, and `Inherits` edges (ADR-3).
    ///
    /// Always emitted (including `"public"`) per OQ-9/ADR-3.
    /// Values: `"public"` | `"protected"` | `"private"`.
    /// `None` for all other edge kinds.
    pub access: Option<String>,

    // ── v7 S2 promoted fields ─────────────────────────────────────────────────
    /// Ordering index for `HAS_PARAM`, `TEMPLATE_PARAM`, and `TEMPLATE_ARG` edges (ADR-5).
    ///
    /// Mirrors the target Parameter node's `param_index`; consumers sort on this field
    /// for ordered traversal.  `None` for all other edge kinds.
    pub edge_index: Option<i64>,

    // ── v7 S4 promoted fields ─────────────────────────────────────────────────
    /// Whether the `Inherits` edge represents virtual inheritance (ADR-3/design §3.4).
    ///
    /// `Some(true)` when the base class is virtually inherited (e.g. `virtual public Base`).
    /// `Some(false)` for non-virtual inheritance.  `None` for all non-`Inherits` edge kinds.
    ///
    /// Pinned name `inherits_is_virtual` (NOT `is_virtual`) to prevent the schema_drift
    /// word-boundary matcher from conflating it with the node-level method `is_virtual`
    /// (design §3.4 / ADR-1).
    pub inherits_is_virtual: Option<bool>,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn all_variants_have_round_trip_str() {
        for &kind in EdgeKind::all() {
            let s = kind.as_str();
            let back = EdgeKind::try_from_arrow_str(s).expect("round-trip must succeed");
            assert_eq!(kind, back, "EdgeKind::{kind:?} round-trip failed");
        }
    }

    #[test]
    fn from_str_unknown_returns_none() {
        assert!(EdgeKind::try_from_arrow_str("UNKNOWN").is_none());
        assert!(EdgeKind::try_from_arrow_str("").is_none());
    }

    #[test]
    fn display_matches_as_str() {
        for &kind in EdgeKind::all() {
            assert_eq!(format!("{kind}"), kind.as_str());
        }
    }
}
