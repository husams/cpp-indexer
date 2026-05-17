/// Base CodexGraph node kinds (AC-M1-2).
///
/// M2 extensions (`NAMESPACE`, `TEMPLATE_DECL`, `SPECIALIZATION`, `TYPEDEF`, `ENUM`, `HEADER`,
/// `MACRO`) and the `REPO` node (M4) are added in later stories (S14, S22) without bumping
/// `SCHEMA_VERSION` for additive variants. Any structural change still requires a bump per ADR-9.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "SCREAMING_SNAKE_CASE")]
pub enum NodeKind {
    /// A C++ translation unit or header treated as a logical module boundary.
    Module,
    /// A `class` or `struct` declaration.
    Class,
    /// A free function or static member function.
    Function,
    /// A non-static member function.
    Method,
    /// A non-static data member.
    Field,
    /// A file-scope or namespace-scope variable (`extern`, `static`, plain).
    GlobalVariable,
}

impl NodeKind {
    /// Canonical string representation stored in the Arrow `kind` column.
    pub fn as_str(self) -> &'static str {
        match self {
            NodeKind::Module => "MODULE",
            NodeKind::Class => "CLASS",
            NodeKind::Function => "FUNCTION",
            NodeKind::Method => "METHOD",
            NodeKind::Field => "FIELD",
            NodeKind::GlobalVariable => "GLOBAL_VARIABLE",
        }
    }

    /// All variants, in declaration order. Used to build Arrow dictionary values.
    pub fn all() -> &'static [NodeKind] {
        &[
            NodeKind::Module,
            NodeKind::Class,
            NodeKind::Function,
            NodeKind::Method,
            NodeKind::Field,
            NodeKind::GlobalVariable,
        ]
    }

    /// Parse from the Arrow string representation. Returns `None` for unknown variants.
    ///
    /// Unlike `std::str::FromStr`, this returns `Option` rather than `Result` because
    /// an unknown variant is always a logic error (not a user input error) and the callers
    /// handle it with `expect` / `panic` rather than user-visible error messages.
    pub fn try_from_arrow_str(s: &str) -> Option<NodeKind> {
        match s {
            "MODULE" => Some(NodeKind::Module),
            "CLASS" => Some(NodeKind::Class),
            "FUNCTION" => Some(NodeKind::Function),
            "METHOD" => Some(NodeKind::Method),
            "FIELD" => Some(NodeKind::Field),
            "GLOBAL_VARIABLE" => Some(NodeKind::GlobalVariable),
            _ => None,
        }
    }
}

impl std::fmt::Display for NodeKind {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.as_str())
    }
}

/// A single node record, matching the `nodes.parquet` schema from ADR-3.
///
/// Optional fields use `Option<T>` to map to nullable Arrow columns.
#[derive(Debug, Clone, PartialEq, serde::Serialize, serde::Deserialize)]
pub struct NodeRecord {
    /// Global primary key (from `clang_getCursorUSR`). Never empty.
    pub usr: String,
    /// Node kind.
    pub kind: NodeKind,
    /// Display name (e.g. function/class name without qualification).
    pub name: String,
    /// Fully-qualified name (`namespace::class::name`).
    pub qualified_name: String,
    /// ABI-mangled name; `None` when unavailable (e.g. for classes/fields).
    pub mangled_name: Option<String>,
    /// Absolute path to the defining source file.
    pub file_path: String,
    /// 1-based line number; `None` if location is invalid.
    pub line: Option<u32>,
    /// 1-based column offset; `None` if location is invalid.
    pub col: Option<u32>,
    /// Repository name from `[repo].name` in the config.
    pub repo_name: String,
    /// Per-kind extra attributes serialised as canonical JSON (virtual, scoped, template_args…).
    pub attrs_json: String,
    /// `true` when the TU this node came from had libclang parse errors.
    pub partial: bool,
    /// Phase that produced this node: 1 (shallow) or 2 (decorated).
    pub phase: u8,
    /// Blake3 hash of `(source_bytes, args_string)` for the originating TU.
    pub tu_hash: [u8; 32],
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn all_variants_have_round_trip_str() {
        for &kind in NodeKind::all() {
            let s = kind.as_str();
            let back = NodeKind::try_from_arrow_str(s).expect("round-trip must succeed");
            assert_eq!(kind, back, "NodeKind::{kind:?} round-trip failed");
        }
    }

    #[test]
    fn from_str_unknown_returns_none() {
        assert!(NodeKind::try_from_arrow_str("UNKNOWN").is_none());
        assert!(NodeKind::try_from_arrow_str("").is_none());
    }

    #[test]
    fn display_matches_as_str() {
        for &kind in NodeKind::all() {
            assert_eq!(format!("{kind}"), kind.as_str());
        }
    }
}
