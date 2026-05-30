/// CodexGraph edge kinds (AC-M1-3, AC-M2-7..AC-M2-12, AC-M4-2, AC-M5-2).
///
/// M1 base: `CONTAINS`, `HAS_METHOD`, `HAS_FIELD`, `INHERITS`, `USES`, `CALLS`.
/// M2 extensions (S14): `INCLUDES`, `OVERRIDES`, `INSTANTIATES`, `SPECIALIZES`, `FRIEND_OF`,
/// `ADL_CANDIDATE`.
/// M4 additions (S22): `BELONGS_TO_REPO`.  `EXTERNAL_REF` is added in S23.
/// M5 additions (S26): `EXPANDS_TO`.
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
