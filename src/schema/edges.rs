/// Base CodexGraph edge kinds (AC-M1-3).
///
/// M2 extensions (`INCLUDES`, `OVERRIDES`, `INSTANTIATES`, `SPECIALIZES`, `FRIEND_OF`,
/// `ADL_CANDIDATE`) and M4 additions (`BELONGS_TO_REPO`, `EXTERNAL_REF`) are added in later
/// stories without bumping `SCHEMA_VERSION` for additive variants.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "SCREAMING_SNAKE_CASE")]
pub enum EdgeKind {
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
