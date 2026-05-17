/// CodexGraph node kinds (AC-M1-2, AC-M2-1..AC-M2-6, AC-M4-1).
///
/// M1 base: `MODULE`, `CLASS`, `FUNCTION`, `METHOD`, `FIELD`, `GLOBAL_VARIABLE`.
/// M2 extensions (S14): `NAMESPACE`, `TEMPLATE_DECL`, `SPECIALIZATION`, `TYPEDEF`, `ENUM`, `HEADER`.
/// M4 additions (S22): `REPO`.
/// `MACRO` is added in S26.
///
/// Adding new variants bumps `SCHEMA_VERSION` per ADR-9 (bump policy: any change to
/// `NodeKind` or `EdgeKind` variants requires a version bump in the same PR).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "SCREAMING_SNAKE_CASE")]
pub enum NodeKind {
    // ── M1 base ─────────────────────────────────────────────────────────────
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

    // ── M2 extensions (S14) ─────────────────────────────────────────────────
    /// A named C++ namespace (`namespace foo { … }`).  AC-M2-1.
    Namespace,
    /// A class or function template declaration (not yet specialised).  AC-M2-2.
    TemplateDef,
    /// A concrete template specialization or instantiation.  AC-M2-3.
    Specialization,
    /// A `typedef` or `using` type alias.  AC-M2-5.
    Typedef,
    /// An `enum` or `enum class` declaration.  AC-M2-6.
    Enum,
    /// A header file referenced via an `#include` directive.  AC-M2-4.
    Header,

    // ── M4 additions (S22) ──────────────────────────────────────────────────
    /// A source-code repository; one REPO node per indexed repo.  AC-M4-1.
    ///
    /// Attributes carried in `attrs_json`:
    /// - `root_path`: absolute path to the repo root on disk.
    /// - `commit_sha`: full Git commit SHA at index time.
    /// - `commit_date`: ISO-8601 date of that commit.
    /// - `sink`: backend name (`"neo4j"` or `"indradb"`).  Used by Phase 5
    ///   to detect heterogeneous-sink configurations (AC-M4-3 enforcement in S23).
    Repo,
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
            NodeKind::Namespace => "NAMESPACE",
            NodeKind::TemplateDef => "TEMPLATE_DECL",
            NodeKind::Specialization => "SPECIALIZATION",
            NodeKind::Typedef => "TYPEDEF",
            NodeKind::Enum => "ENUM",
            NodeKind::Header => "HEADER",
            NodeKind::Repo => "REPO",
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
            NodeKind::Namespace,
            NodeKind::TemplateDef,
            NodeKind::Specialization,
            NodeKind::Typedef,
            NodeKind::Enum,
            NodeKind::Header,
            NodeKind::Repo,
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
            "NAMESPACE" => Some(NodeKind::Namespace),
            "TEMPLATE_DECL" => Some(NodeKind::TemplateDef),
            "SPECIALIZATION" => Some(NodeKind::Specialization),
            "TYPEDEF" => Some(NodeKind::Typedef),
            "ENUM" => Some(NodeKind::Enum),
            "HEADER" => Some(NodeKind::Header),
            "REPO" => Some(NodeKind::Repo),
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
