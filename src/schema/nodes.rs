/// CodexGraph node kinds (AC-M1-2, AC-M2-1..AC-M2-6, AC-M4-1, AC-M5-1).
///
/// M1 base: `MODULE`, `CLASS`, `FUNCTION`, `METHOD`, `FIELD`, `GLOBAL_VARIABLE`.
/// M2 extensions (S14): `NAMESPACE`, `TEMPLATE_DECL`, `SPECIALIZATION`, `TYPEDEF`, `ENUM`, `HEADER`.
/// M4 additions (S22): `REPO`.
/// M5 additions (S26): `MACRO`.
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

    // ── M5 additions (S26) ──────────────────────────────────────────────────
    /// A preprocessor macro definition (`#define`).  AC-M5-1.
    ///
    /// Attributes carried in `attrs_json`:
    /// - `params`: JSON array of parameter name strings for function-like macros;
    ///   `null` for object-like macros.
    /// - `is_function_like`: `true` for function-like macros, `false` otherwise.
    /// - `is_builtin`: `true` when libclang reports the macro as builtin.
    ///
    /// USR is synthesised as `"macro:<file>:<name>"` because libclang does not
    /// assign USRs to `MacroDefinition` entities.
    Macro,
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
            NodeKind::Macro => "MACRO",
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
            NodeKind::Macro,
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
            "MACRO" => Some(NodeKind::Macro),
            _ => None,
        }
    }
}

impl std::fmt::Display for NodeKind {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.as_str())
    }
}

/// A function parameter extracted from a FUNCTION or METHOD node (ADR-14).
///
/// `type_` is renamed to `"type"` on the wire (JSON / Arrow / Bolt / IndraDB) because `type` is
/// a Rust keyword; `#[serde(rename = "type")]` handles the JSON boundary.
#[derive(Debug, Clone, PartialEq, serde::Serialize, serde::Deserialize)]
pub struct Param {
    /// Parameter name as it appears in source.
    pub name: String,
    /// Display type name as returned by libclang (e.g. `"int"`, `"const std::string &"`).
    #[serde(rename = "type")]
    pub type_: String,
}

/// A template parameter extracted from a TEMPLATE_DECL node (ADR-14).
#[derive(Debug, Clone, PartialEq, serde::Serialize, serde::Deserialize)]
pub struct TemplateParam {
    /// Parameter name as it appears in source (e.g. `"T"`, `"N"`).
    pub name: String,
    /// Parameter kind: `"type"`, `"non_type"`, or `"template"`.
    pub kind: String,
    /// Default argument expression, if present (e.g. `"int"`, `"0"`).
    pub default: Option<String>,
}

/// A template argument extracted from a SPECIALIZATION node (ADR-14).
#[derive(Debug, Clone, PartialEq, serde::Serialize, serde::Deserialize)]
pub struct TemplateArg {
    /// Argument kind: `"type"`, `"integral"`, `"template"`, or `"expression"`.
    pub kind: String,
    /// Argument value as a display string (e.g. `"int"`, `"42"`).
    pub value: String,
}

/// A single node record, matching the `nodes.parquet` schema from ADR-3.
///
/// Optional fields use `Option<T>` to map to nullable Arrow columns.
///
/// M8 (S40): ten new optional columns promoted from `attrs_json` per ADR-11. None of these
/// are dual-written into `attrs_json` (AC-S40-6). Applicable kinds per design.md §3.2:
/// - `return_type`, `params`, `signature`, `code`, `code_truncated`: FUNCTION, METHOD
/// - `is_static`: FUNCTION, METHOD
/// - `template_params`: TEMPLATE_DECL
/// - `template_args`: SPECIALIZATION
/// - `is_virtual`, `is_pure_virtual`: METHOD only
///
/// graph-symbol-ids (Story 3, v6): two new integer ID columns.
/// - `symbol_id`: per-repo integer from `SymbolAllocator::get_or_insert_symbol(usr)`.
///   Retained `usr` for Phase-5 USR matching (D1).
/// - `file_id`: per-repo integer from `SymbolAllocator::get_or_insert_file(file_path)`.
///   Retained `file_path` for Phase-5 staging reads.
///
/// Sinks write `symbol_id`/`file_id` to the durable graph and drop the string fields.
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
    /// Per-kind extra attributes serialised as canonical JSON (long-tail bag; promoted fields
    /// MUST NOT appear here — see AC-S40-6 and ADR-11 §3).
    pub attrs_json: String,
    /// `true` when the TU this node came from had libclang parse errors.
    pub partial: bool,
    /// Phase that produced this node: 1 (shallow) or 2 (decorated).
    pub phase: u8,
    /// Blake3 hash of `(source_bytes, args_string)` for the originating TU.
    pub tu_hash: [u8; 32],

    // ── M8 promoted fields (S40) ─────────────────────────────────────────────
    /// Return type display string; `Some` for FUNCTION/METHOD, `None` for all other kinds.
    pub return_type: Option<String>,
    /// Parameter list; `Some` for FUNCTION/METHOD, `None` for all other kinds.
    /// An empty `Vec` means a function with no parameters (distinct from `None`).
    pub params: Option<Vec<Param>>,
    /// Function signature string (e.g. `"void(int, const char*) const"`);
    /// `Some` for FUNCTION/METHOD, `None` otherwise.
    pub signature: Option<String>,
    /// Verbatim source snippet for the declaration body, capped at 32 KiB (ADR-12).
    /// `None` when the body exceeds the cap or the kind is not FUNCTION/METHOD.
    pub code: Option<String>,
    /// `Some(true)` when the body was truncated; `Some(false)` when it fits within the cap.
    /// `None` for kinds where `code` is not extracted.
    pub code_truncated: Option<bool>,
    /// Template parameter list; `Some` for TEMPLATE_DECL, `None` otherwise.
    pub template_params: Option<Vec<TemplateParam>>,
    /// Template argument list; `Some` for SPECIALIZATION, `None` otherwise.
    pub template_args: Option<Vec<TemplateArg>>,
    /// `true` when the method is declared `virtual`; `None` for non-METHOD kinds.
    pub is_virtual: Option<bool>,
    /// `true` when the method is pure virtual (`= 0`); `None` for non-METHOD kinds.
    pub is_pure_virtual: Option<bool>,
    /// `true` when the function/method is declared `static`; `None` for other kinds.
    pub is_static: Option<bool>,

    // ── graph-symbol-ids integer ID fields (Story 3, v6) ─────────────────────
    /// Per-repo integer ID for `usr`; from `SymbolAllocator::get_or_insert_symbol`.
    /// Populated during Phase 1; sinks key the durable graph on this field (not `usr`).
    pub symbol_id: i64,
    /// Per-repo integer ID for `file_path`; from `SymbolAllocator::get_or_insert_file`.
    /// Populated during Phase 1; sinks store this instead of the path string.
    pub file_id: i64,
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
