/// CodexGraph node kinds (AC-M1-2, AC-M2-1..AC-M2-6, AC-M4-1, AC-M5-1).
///
/// M1 base: `MODULE`, `CLASS`, `FUNCTION`, `METHOD`, `FIELD`, `GLOBAL_VARIABLE`.
/// M2 extensions (S14): `NAMESPACE`, `TEMPLATE_DECL`, `SPECIALIZATION`, `TYPEDEF`, `ENUM`, `HEADER`.
/// M4 additions (S22): `REPO`.
/// M5 additions (S26): `MACRO`.
/// v7 S2 additions: `TYPE`, `PARAMETER`.
/// v7 S3 additions: `TEMPLATE_ARG`, `CONCEPT`.
/// v7 S5 additions: `ENUMERATOR`.
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

    // ── v7 S2 additions ─────────────────────────────────────────────────────
    /// A C++ type (canonical or written-spelling deduped; ADR-2).
    ///
    /// USR is synthesised as `"type:<written-spelling>"` (ADR-2).
    /// cv-qualifiers participate: `const int` and `int` are distinct nodes.
    Type,
    /// A function/method parameter or template parameter.
    ///
    /// USR: `"param:<owner-usr>:<index>"` for function params,
    ///      `"tparam:<template-usr>:<index>"` for template params (S3).
    Parameter,

    // ── v7 S3 additions ─────────────────────────────────────────────────────
    /// A positional template argument node (ADR-5).
    ///
    /// Each template argument at a specialization becomes ONE positional node
    /// (distinct from S2's `Parameter`) to avoid IndraDB edge-identity collapse
    /// (see ADR-5 for the correctness argument).
    ///
    /// USR: `"targ:<specialization-usr>:<index>"`.
    /// Native columns: `param_index` (0-based position), `param_kind`
    /// (`"type"` | `"non_type"` | `"template"` | `"expression"`), `type_spelling`
    /// (written value; type-kind args also emit OF_TYPE → Type node).
    TemplateArg,
    /// A C++20 concept declaration (`concept Printable = …`; ADR-7).
    ///
    /// Only emitted when the TU is parsed in C++20 mode and libclang exposes a
    /// `ConceptDecl` cursor.  Pre-C++20 SFINAE constraints are NOT modelled
    /// (documented fidelity gap; see SCHEMA.md §Fidelity).
    ///
    /// USR: real libclang USR from the `CXCursor_ConceptDecl` entity.
    Concept,

    // ── v7 S5 additions ─────────────────────────────────────────────────────
    /// A single enumeration constant (enumerator) within an `enum` or `enum class`.
    ///
    /// USR: real libclang USR (enumerators always receive USRs).
    /// Native column: `enum_value: Option<i64>` — the signed integer constant value.
    /// Emits ENUMERATOR_OF → parent Enum node.
    Enumerator,
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
            // v7 S2:
            NodeKind::Type => "TYPE",
            NodeKind::Parameter => "PARAMETER",
            // v7 S3:
            NodeKind::TemplateArg => "TEMPLATE_ARG",
            NodeKind::Concept => "CONCEPT",
            // v7 S5:
            NodeKind::Enumerator => "ENUMERATOR",
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
            // v7 S2:
            NodeKind::Type,
            NodeKind::Parameter,
            // v7 S3:
            NodeKind::TemplateArg,
            NodeKind::Concept,
            // v7 S5:
            NodeKind::Enumerator,
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
            // v7 S2:
            "TYPE" => Some(NodeKind::Type),
            "PARAMETER" => Some(NodeKind::Parameter),
            // v7 S3:
            "TEMPLATE_ARG" => Some(NodeKind::TemplateArg),
            "CONCEPT" => Some(NodeKind::Concept),
            // v7 S5:
            "ENUMERATOR" => Some(NodeKind::Enumerator),
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

    // ── v7 S1 promoted fields ─────────────────────────────────────────────────
    /// `true` when the Field/GlobalVariable is declared `const`; `None` for other kinds.
    pub is_const: Option<bool>,
    /// `true` when the Field/GlobalVariable is declared `constexpr`; `None` for other kinds.
    ///
    /// NOTE: libclang 18 does not expose a direct `is_constexpr` API for variable cursors.
    /// This field is always emitted as `Some(false)` until a future libclang upgrade
    /// provides the CXCursor_isConstexpr flag.  See follow-up task in implementation-notes.
    pub is_constexpr: Option<bool>,
    /// Storage class of a Field or GlobalVariable declaration; `None` for other kinds.
    ///
    /// Values: `"auto"` | `"static"` | `"extern"` | `"thread_local"` | `"register"` | `"none"`.
    pub storage_class: Option<String>,

    // ── v7 S2 promoted fields ─────────────────────────────────────────────────
    /// `true` when the Function/Method/Class/TemplateDef node is a template;
    /// `None` for other kinds.
    pub is_template: Option<bool>,
    /// `true` when the Function/Method is declared `noexcept`; `None` for other kinds.
    ///
    /// NOTE: clang-rs 2.0.0 does not expose a direct `is_noexcept` API for cursors.
    /// This field is always emitted as `Some(false)` for Functions/Methods until a
    /// future libclang/clang-rs upgrade exposes CXCursor_isNoexcept.
    /// See follow-up in implementation-notes.
    pub is_noexcept: Option<bool>,
    /// `true` when the Method is declared `override`; `None` for non-Method kinds.
    ///
    /// NOTE: clang-rs 2.0.0 does not expose a direct `is_override` API.
    /// This field is always emitted as `Some(false)` for Methods until libclang exposes it.
    /// See follow-up in implementation-notes.
    pub is_override: Option<bool>,
    /// `true` when the Function/Method is declared `= delete`; `None` for other kinds.
    ///
    /// NOTE: clang-rs 2.0.0 does not expose `is_deleted()` (no `clang_CXXMethod_isDeleted`
    /// binding).  This field is always emitted as `Some(false)` until the binding is added.
    /// See follow-up in implementation-notes.
    pub is_deleted: Option<bool>,
    /// `true` when the Function/Method is declared `= default`; `None` for other kinds.
    ///
    /// NOTE: requires the `clang_3_9` feature which is not currently enabled in Cargo.toml.
    /// This field is always emitted as `Some(false)` until `clang_3_9` is enabled.
    /// See follow-up in implementation-notes.
    pub is_defaulted: Option<bool>,
    /// CV-qualifiers string for a Method; `None` for other kinds.
    ///
    /// Values: `"const"`, `"volatile"`, `"const volatile"`, or `""` (empty = unqualified).
    pub cv_qualifiers: Option<String>,
    /// Ref-qualifier string for a Method; `None` for other kinds.
    ///
    /// Values: `"&"` (lvalue), `"&&"` (rvalue), or `""` (empty = none).
    pub ref_qualifier: Option<String>,
    /// `true` when the Class is declared `final`; `None` for other kinds.
    ///
    /// NOTE: clang-rs 2.0.0 does not expose `is_final()`. This field is always emitted
    /// as `Some(false)` until the binding is available.  See follow-up in implementation-notes.
    pub is_final: Option<bool>,
    /// `true` when the Class contains at least one pure-virtual method; `None` for other kinds.
    pub is_abstract: Option<bool>,
    /// Record kind for Class nodes; `None` for other kinds.
    ///
    /// Values: `"class"` | `"struct"` | `"union"`.
    pub record_kind: Option<String>,
    /// Written-spelling of the type for Type/Parameter nodes; `None` for other kinds.
    pub type_spelling: Option<String>,
    /// 0-based parameter index for Parameter nodes; `None` for other kinds.
    pub param_index: Option<i64>,
    /// Parameter kind for Parameter nodes; `None` for other kinds.
    ///
    /// Values: `"type"` | `"non_type"` | `"template"` | `"value"`.
    pub param_kind: Option<String>,

    // ── v7 S5 promoted fields ─────────────────────────────────────────────────
    /// Signed integer constant value for Enumerator nodes; `None` for other kinds.
    ///
    /// Populated from `clang_getEnumConstantDeclValue` (signed i64).
    pub enum_value: Option<i64>,
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
