// Schema-prompt code generator (AC-M6-1, AC-M6-2).
//
// This module is compiled both as part of the `cpp_indexer` crate (for unit tests
// that verify the data tables stay in sync with the enum definitions) and
// `include!`'d into `build.rs` (where it cannot import crate items).
//
// Inner doc comments (`//!`) are intentionally avoided here because this file is
// also `include!`'d into build.rs where inner doc comments cause a compile error.
//
// Determinism guarantee:
// `generate_schema` is a pure function of the constant tables defined here.
// It must produce byte-identical output on every call and every platform.
// Rules enforced:
// - No `HashMap` iteration (order undefined).
// - No timestamps or env-dependent values.
// - All string output uses `\n` (Unix line endings) unconditionally.
// - Node and edge entries are emitted in declaration order (the const arrays).

// ── Node kind table ──────────────────────────────────────────────────────────
//
// Each entry is (canonical_str, doc).
// MUST stay in sync with `NodeKind::all()` + `NodeKind::as_str()`.
// Verified by `tests::node_table_matches_enum` and `tests::edge_table_matches_enum`.
#[cfg_attr(not(test), allow(dead_code))]
const NODE_KINDS: &[(&str, &str)] = &[
    // M1 base
    (
        "MODULE",
        "A C++ translation unit or header treated as a logical module boundary.",
    ),
    ("CLASS", "A `class` or `struct` declaration."),
    ("FUNCTION", "A free function or static member function."),
    ("METHOD", "A non-static member function."),
    ("FIELD", "A non-static data member."),
    (
        "GLOBAL_VARIABLE",
        "A file-scope or namespace-scope variable (`extern`, `static`, plain).",
    ),
    // M2 extensions
    (
        "NAMESPACE",
        "A named C++ namespace (`namespace foo { … }`).",
    ),
    (
        "TEMPLATE_DECL",
        "A class or function template declaration (not yet specialised).",
    ),
    (
        "SPECIALIZATION",
        "A concrete template specialization or instantiation.",
    ),
    ("TYPEDEF", "A `typedef` or `using` type alias."),
    ("ENUM", "An `enum` or `enum class` declaration."),
    (
        "HEADER",
        "A header file referenced via an `#include` directive.",
    ),
    // M4 additions
    (
        "REPO",
        "A source-code repository; one REPO node per indexed repo.",
    ),
    // M5 additions
    ("MACRO", "A preprocessor macro definition (`#define`)."),
    // v7 S2 additions
    (
        "TYPE",
        "A C++ type node (written-spelling deduped; ADR-2). USR: `type:<written-spelling>`.",
    ),
    (
        "PARAMETER",
        "A function/method parameter or template parameter. USR: `param:<owner-usr>:<index>`.",
    ),
    // v7 S3 additions
    (
        "TEMPLATE_ARG",
        "A positional template argument node (ADR-5). USR: `targ:<specialization-usr>:<index>`. Carries param_index, param_kind, type_spelling; type-kind args also emit OF_TYPE to a Type node.",
    ),
    (
        "CONCEPT",
        "A C++20 concept declaration. USR: real libclang USR. Only emitted in C++20 mode (ADR-7).",
    ),
    // v7 S5 additions
    (
        "ENUMERATOR",
        "A single enumeration constant within an enum or enum class. USR: real libclang USR. Carries enum_value (signed i64).",
    ),
];

// ── Edge kind table ──────────────────────────────────────────────────────────
//
// Each entry is (canonical_str, doc).
// MUST stay in sync with `EdgeKind::all()` + `EdgeKind::as_str()`.
#[cfg_attr(not(test), allow(dead_code))]
const EDGE_KINDS: &[(&str, &str)] = &[
    // M1 base
    ("CONTAINS", "Lexical containment (e.g., class contains method, namespace contains class)."),
    ("HAS_METHOD", "A class directly owns a method declaration."),
    ("HAS_FIELD", "A class directly owns a field declaration."),
    ("INHERITS", "Direct (non-virtual) or virtual base-class inheritance."),
    ("USES", "A type reference / type usage relationship (parameter type, return type, field type)."),
    ("CALLS", "A direct call from one function/method to another."),
    // M2 extensions
    ("INCLUDES", "A translation unit (MODULE) or header (HEADER) includes another header."),
    ("OVERRIDES", "A virtual method overrides a base-class virtual method; `vtable_slot` in attrs."),
    ("INSTANTIATES", "A call site or context that instantiates a template."),
    ("SPECIALIZES", "A template specialization refers to its primary template."),
    ("FRIEND_OF", "A class grants friend access to another entity."),
    ("ADL_CANDIDATE", "An unresolved name that is a candidate for Argument-Dependent Lookup."),
    // M4 additions
    ("BELONGS_TO_REPO", "Links every indexed node to its owning REPO node. Direction: (node)-[:BELONGS_TO_REPO]->(repo)."),
    ("EXTERNAL_REF", "A cross-repository reference resolved during Phase 5. Direction: (src)-[:EXTERNAL_REF {via: <orig_edge_kind>}]->(dst)."),
    // M5 additions
    ("EXPANDS_TO", "A top-level macro expansion at a call site. Direction: (call_site)-[:EXPANDS_TO]->(macro_def)."),
    // v7 S2 additions
    ("RETURNS", "A function/method returns a type. Direction: (Function|Method)-[:RETURNS]->(Type)."),
    ("HAS_PARAM", "A function/method has a parameter (ordered by edge_index). Direction: (Function|Method)-[:HAS_PARAM {edge_index}]->(Parameter)."),
    ("OF_TYPE", "A Parameter or Field has a type. Direction: (Parameter|Field)-[:OF_TYPE]->(Type)."),
    ("POINTS_TO", "A pointer type points to its pointee type. Direction: (Type:ptr)-[:POINTS_TO]->(Type)."),
    ("REFERS_TO", "A reference type refers to its referent type. Direction: (Type:ref)-[:REFERS_TO]->(Type)."),
    // v7 S3 additions
    ("TEMPLATE_PARAM", "A template definition has a template parameter (ordered). Direction: (TemplateDef)-[:TEMPLATE_PARAM {edge_index}]->(Parameter)."),
    ("TEMPLATE_ARG", "A template specialization has a positional template argument (ADR-5). Direction: (Specialization)-[:TEMPLATE_ARG {edge_index}]->(TemplateArg)."),
    ("CONSTRAINED_BY", "A template or parameter is constrained by a concept (C++20 only; ADR-7). Direction: (template_or_param)-[:CONSTRAINED_BY]->(Concept)."),
    // v7 S5 additions
    ("ENUMERATOR_OF", "An enumerator constant belongs to its parent enum. Direction: (Enumerator)-[:ENUMERATOR_OF]->(Enum)."),
    ("UNDERLYING_TYPE", "An enum has an underlying integer type. Direction: (Enum)-[:UNDERLYING_TYPE]->(Type)."),
    ("ALIAS_OF", "A typedef/type-alias is an alias for another type (one link per chain; ADR-2). Direction: (Typedef)-[:ALIAS_OF]->(Type)."),
    ("USES_NAMESPACE", "A using-directive introduces a namespace into scope. Direction: (scope)-[:USES_NAMESPACE]->(Namespace)."),
    ("USES_DECLARATION", "A using-declaration introduces a specific name from another scope. Direction: (scope)-[:USES_DECLARATION]->(symbol)."),
];

/// Emit the MCP system-prompt schema text.
///
/// The output is a deterministic, human-readable description of all node kinds,
/// edge kinds, and the schema version. It is written to
/// `prompt/graph_database/cpp/schema.txt` by `build.rs` and consumed by cpp-mcp
/// at agent startup.
///
/// The embedded version comment (`# schema-version: cxg-schema-vN`) enables
/// cpp-mcp to compare against the live graph's `SchemaVersion` node (AC-M6-5).
pub fn generate_schema(schema_version_tag: &str) -> String {
    let mut out = String::with_capacity(4096);

    // Header comment — parseable by cpp-mcp to extract the embedded version.
    out.push_str("# cpp-indexer graph schema — MCP system prompt\n");
    out.push_str(
        "# DO NOT EDIT — generated by build.rs from src/schema/{nodes,edges,version}.rs\n",
    );
    out.push_str(&format!("# schema-version: {schema_version_tag}\n"));
    out.push('\n');

    // ── Node kinds ────────────────────────────────────────────────────────────
    out.push_str("## Node kinds\n\n");
    out.push_str(
        "Each node has a `kind` property (one of the values below) and a USR primary key.\n\n",
    );
    for (name, doc) in NODE_KINDS {
        out.push_str(&format!("### {name}\n{doc}\n\n"));
    }

    // ── Edge kinds ────────────────────────────────────────────────────────────
    out.push_str("## Edge kinds\n\n");
    out.push_str("Edges connect nodes. Each edge has a `kind` property, `src_usr`, `dst_usr`, and optional `attrs_json`.\n\n");
    for (name, doc) in EDGE_KINDS {
        out.push_str(&format!("### {name}\n{doc}\n\n"));
    }

    // ── Key properties ────────────────────────────────────────────────────────
    out.push_str("## Key node properties\n\n");
    out.push_str("- `usr` (string): global primary key from libclang (clang_getCursorUSR). For MACRO nodes, synthesised as `macro:<file>:<name>`.\n");
    out.push_str("- `kind` (string): one of the node kind names above.\n");
    out.push_str("- `name` (string): display name without qualification.\n");
    out.push_str(
        "- `qualified_name` (string): fully-qualified name (e.g. `namespace::class::name`).\n",
    );
    out.push_str("- `mangled_name` (string|null): ABI-mangled name; null when unavailable.\n");
    out.push_str("- `file_path` (string): absolute path to the defining source file.\n");
    out.push_str("- `line` (int|null): 1-based line number.\n");
    out.push_str("- `col` (int|null): 1-based column offset.\n");
    out.push_str("- `repo_name` (string): name of the owning repository.\n");
    out.push_str("- `attrs_json` (string): per-kind extra attributes as canonical JSON.\n");
    out.push_str("- `partial` (bool): true when the TU had libclang parse errors.\n");
    out.push_str("- `return_type` (string|null): type returned by a function or method.\n");
    out.push_str("- `params` (list|null): parameters for a function or method.\n");
    out.push_str("- `signature` (string|null): full function/method signature.\n");
    out.push_str("- `code` (string|null): source code snippet.\n");
    out.push_str("- `code_truncated` (bool|null): true if source code snippet was truncated.\n");
    out.push_str(
        "- `template_params` (list|null): template parameters for template declarations.\n",
    );
    out.push_str("- `template_args` (list|null): template arguments for specializations.\n");
    out.push_str("- `is_virtual` (bool|null): true if method is virtual.\n");
    out.push_str("- `is_pure_virtual` (bool|null): true if method is pure virtual.\n");
    out.push_str("- `is_static` (bool|null): true if function/method is static.\n");
    // v7 S1 node fields
    out.push_str(
        "- `is_const` (bool|null): true if Field/GlobalVariable is const; null for other kinds.\n",
    );
    out.push_str("- `is_constexpr` (bool|null): true if Field/GlobalVariable is constexpr; null for other kinds.\n");
    out.push_str("- `storage_class` (string|null): storage class of Field/GlobalVariable (\"auto\"|\"static\"|\"extern\"|\"thread_local\"|\"register\"|\"none\"); null for other kinds.\n");
    // v7 S2 node fields
    out.push_str("- `is_template` (bool|null): true if Function/Method/Class/TemplateDef is a template; null for other kinds.\n");
    out.push_str(
        "- `is_noexcept` (bool|null): true if Function/Method is noexcept; null for other kinds.\n",
    );
    out.push_str(
        "- `is_override` (bool|null): true if Method is override; null for other kinds.\n",
    );
    out.push_str(
        "- `is_deleted` (bool|null): true if Function/Method is deleted; null for other kinds.\n",
    );
    out.push_str("- `is_defaulted` (bool|null): true if Function/Method is defaulted; null for other kinds.\n");
    out.push_str("- `cv_qualifiers` (string|null): CV-qualifiers of Method (\"const\"|\"volatile\"|\"const volatile\"|empty); null for other kinds.\n");
    out.push_str("- `ref_qualifier` (string|null): ref-qualifier of Method (\"&\"|\"&&\"|empty); null for other kinds.\n");
    out.push_str("- `is_final` (bool|null): true if Class is final; null for other kinds.\n");
    out.push_str("- `is_abstract` (bool|null): true if Class is abstract; null for other kinds.\n");
    out.push_str("- `record_kind` (string|null): record kind of Class (\"class\"|\"struct\"|\"union\"); null for other kinds.\n");
    out.push_str("- `type_spelling` (string|null): written-spelling of the type for Type/Parameter/TemplateArg nodes; null for other kinds.\n");
    out.push_str("- `param_index` (int|null): 0-based parameter index for Parameter/TemplateArg nodes; null for other kinds.\n");
    out.push_str("- `param_kind` (string|null): parameter kind for Parameter/TemplateArg nodes (\"type\"|\"non_type\"|\"template\"|\"value\"); null for other kinds.\n");
    // v7 S5 node fields
    out.push_str("- `enum_value` (int|null): signed integer constant value for Enumerator nodes; null for other kinds.\n");
    out.push('\n');

    // ── Key edge properties ───────────────────────────────────────────────────
    out.push_str("## Key edge properties\n\n");
    out.push_str("- `src_usr` (string): source node USR.\n");
    out.push_str(
        "- `dst_usr` (string|null): destination node USR; null when unresolved at emit time.\n",
    );
    out.push_str("- `kind` (string): one of the edge kind names above.\n");
    out.push_str(
        "- `resolved` (bool): true when dst_usr was found in the current repo's USR map.\n",
    );
    out.push_str("- `cross_repo_candidate` (bool): true when dst_usr is absent from the current repo's map.\n");
    out.push_str("- `attrs_json` (string): edge attributes as canonical JSON (vtable_slot, access, virtual, via…).\n");
    out.push_str("- `source_association_type` (string|null): source-side access classification for USES edges.\n");
    out.push_str("- `target_association_type` (string|null): target-side access classification for USES edges.\n");
    // v7 S1 edge fields
    out.push_str("- `access` (string|null): C++ access specifier on HasMethod/HasField/Inherits edges (\"public\"|\"protected\"|\"private\"); null for other kinds.\n");
    // v7 S2 edge fields
    out.push_str("- `edge_index` (int|null): ordering index for HAS_PARAM/TEMPLATE_PARAM/TEMPLATE_ARG edges; null for other kinds.\n");
    // v7 S4 edge fields
    out.push_str("- `inherits_is_virtual` (bool|null): true when Inherits edge is virtual inheritance; null for other kinds.\n");
    out.push('\n');

    out
}

// ── Tests (compiled only as part of the crate, not in build.rs include!) ────
#[cfg(test)]
mod tests {
    use super::*;
    use crate::schema::{edges::EdgeKind, nodes::NodeKind};

    /// Verify the node table is in sync with `NodeKind::all()` + `as_str()`.
    #[test]
    fn node_table_matches_enum() {
        let enum_strs: Vec<&str> = NodeKind::all().iter().map(|k| k.as_str()).collect();
        let table_strs: Vec<&str> = NODE_KINDS.iter().map(|(s, _)| *s).collect();
        assert_eq!(
            enum_strs, table_strs,
            "NODE_KINDS table is out of sync with NodeKind::all(). \
             Update src/prompt/codegen.rs to match nodes.rs."
        );
    }

    /// Verify the edge table is in sync with `EdgeKind::all()` + `as_str()`.
    #[test]
    fn edge_table_matches_enum() {
        let enum_strs: Vec<&str> = EdgeKind::all().iter().map(|k| k.as_str()).collect();
        let table_strs: Vec<&str> = EDGE_KINDS.iter().map(|(s, _)| *s).collect();
        assert_eq!(
            enum_strs, table_strs,
            "EDGE_KINDS table is out of sync with EdgeKind::all(). \
             Update src/prompt/codegen.rs to match edges.rs."
        );
    }

    /// `generate_schema` must be a pure function: identical output on repeated calls.
    #[test]
    fn generate_schema_is_stable() {
        let a = generate_schema("cxg-schema-v4");
        let b = generate_schema("cxg-schema-v4");
        assert_eq!(a, b, "generate_schema must be deterministic");
    }

    /// Output must start with the expected header and embed the version tag.
    #[test]
    fn generate_schema_embeds_version() {
        let tag = "cxg-schema-v4";
        let out = generate_schema(tag);
        assert!(
            out.contains(&format!("# schema-version: {tag}")),
            "schema.txt must contain '# schema-version: {tag}'"
        );
    }

    /// Output must use Unix line endings only.
    #[test]
    fn generate_schema_unix_line_endings() {
        let out = generate_schema("cxg-schema-v4");
        assert!(
            !out.contains('\r'),
            "schema.txt must not contain carriage returns"
        );
    }

    /// Every node kind name must appear in the output.
    #[test]
    fn generate_schema_contains_all_node_kinds() {
        let out = generate_schema("cxg-schema-v4");
        for (name, _) in NODE_KINDS {
            assert!(out.contains(name), "schema.txt missing node kind: {name}");
        }
    }

    /// Every edge kind name must appear in the output.
    #[test]
    fn generate_schema_contains_all_edge_kinds() {
        let out = generate_schema("cxg-schema-v4");
        for (name, _) in EDGE_KINDS {
            assert!(out.contains(name), "schema.txt missing edge kind: {name}");
        }
    }
}
