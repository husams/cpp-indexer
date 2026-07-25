// Plain row structs mirroring the Python dataclasses (design §5.1).
// compile_options is the decoded JSON array (util/json_min); id fields hold
// the SQLite rowid once a row has been read back.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cidx {

using SymbolValue = std::variant<std::nullptr_t, int64_t, double, std::string>;

struct IndexIdentity {
  int schema_version = 0;
  std::optional<std::string> source_revision;
  std::optional<std::string> source_fingerprint;
  std::optional<std::string> index_config;
  std::optional<std::string> index_config_fingerprint;
  std::string freshness = "unverifiable";
  std::string workspace = "workspace:memory";
};

struct DefinitionRow {
  int64_t symbol_id = -1;
  std::optional<int64_t> file_id;
  std::optional<int64_t> line, col, end_line, end_col;
  std::optional<std::string> init_text;
};

struct EdgeSiteRow {
  int64_t edge_id = -1;
  std::optional<int64_t> file_id;
  std::optional<int64_t> line;
  std::optional<int64_t> col;
  bool conditional = false;
  std::optional<std::string> args_sig;
  std::optional<std::string> recv_src_kind;
  std::optional<std::string> recv_type_usr;
  std::optional<std::string> recv_decl_usr;
  std::optional<int64_t> recv_param_pos;
  std::optional<int64_t> recv_type_is_value;
};

// v35: an explicit declared program/dependency universe. The key is the
// portable part of a symbol identity; id is database-local only.
struct SemanticUniverse {
  int64_t id = -1;
  std::string key;
  std::string name;
  std::string policy = "explicit";
};

struct Component {
  int64_t id = -1;
  std::string name;
  std::string path;                   // base path (no version segment)
  std::string kind;                   // 'repo' | 'external'
  std::optional<std::string> version; // v14: nullable; NULL = unversioned
  std::optional<int64_t>
      repository_id; // v23: owning repository; NULL = ungrouped
  std::optional<int64_t>
      semantic_universe_id; // v35: explicit scope for ungrouped components
};

// Input payload for component creation. The adapter must preserve the
// version even though it is not part of the generated component identity.
struct ComponentWriteRecord {
  std::string name;
  std::string path;
  std::string kind;
  std::optional<std::string> version;
};

// v23: a logical code base grouping >=1 components, with switchable clones.
struct Repository {
  int64_t id = -1;
  std::string name;
  std::string kind;                            // 'repo' | 'external'
  std::optional<std::string> remote_url;       // git origin URL when known
  std::optional<int64_t> active_clone_id;      // -> clone.id; NULL if none yet
  std::optional<int64_t> semantic_universe_id; // v35: declared program universe
};

// Input payload for repository creation, including its declared universe.
struct RepositoryWriteRecord {
  std::string name;
  std::string kind;
  std::optional<std::string> remote_url;
  std::optional<int64_t> semantic_universe_id;
};

// v23: one checkout/worktree directory of a repository.
struct Clone {
  int64_t id = -1;
  int64_t repository_id = -1;
  std::string path; // absolute checkout/worktree root
  std::optional<std::string> label;
};

// v14: label registry row
struct Label {
  int64_t id = -1;
  std::string name; // label key, e.g. 'libfoo-include'
  std::string path; // stored verbatim; may contain $VAR
};

struct Directory {
  int64_t id = -1;
  int64_t component_id = -1;
  std::string path; // relative to component.path; '' = root
};

struct File {
  int64_t id = -1;
  int64_t directory_id = -1;
  std::string name;
  std::optional<double> mtime;
  std::optional<std::string> md5;
  std::optional<std::vector<std::string>> compile_options; // decoded JSON
  std::optional<std::string> driver;
  bool indexed = false;
  std::optional<std::string> indexed_at;
  bool args_overridden = false; // flags hand-edited via `cidx file`
};

// v15: one captured parse diagnostic (severity >= warning), keyed by the TU's
// file row. Locationless diagnostics leave file_path/line/col unset (NULL).
struct Diagnostic {
  int64_t id = -1;
  int64_t file_id = -1;
  int severity = 0; // clang: 2=warning, 3=error, 4=fatal
  std::string spelling;
  std::optional<std::string> file_path;
  std::optional<int64_t> line;
  std::optional<int64_t> col;
};

struct Symbol {
  std::string usr;
  std::string spelling;
  std::string kind; // one of the 17 kSymbolKinds
  std::optional<std::string> qual_name;
  std::optional<std::string> display_name;
  std::optional<std::string> type_info;
  std::optional<int64_t> file_id;
  std::optional<int64_t> line;
  std::optional<int64_t> col;
  std::optional<int64_t> end_line; // v25: end of the symbol's own extent at
  std::optional<int64_t>
      end_col; // (line, col); (line..end_line) slices it whole
  std::optional<int64_t> decl_file_id;
  std::optional<int64_t> decl_line;
  std::optional<int64_t> decl_col;
  std::optional<std::string> decl_path; // raw decl path for an unregistered
                                        // (system/stdlib) target -- see schema
  bool is_definition = false;
  bool is_pure = false;
  bool is_static = false; // v12: C++ static member function. Free functions and
                          // non-methods are false; a file-scope `static` free
                          // function is reflected by linkage='internal'.
  bool is_instantiation =
      false; // v13: implicit template-instantiation node
             // (X<int> type node or X<int>::member); its
             // definition is expressed via instantiates edge.
  std::optional<std::string> callable_kind;
  std::optional<std::string> template_origin;
  std::optional<std::string> template_form;
  std::optional<std::string> linkage;
  std::optional<std::string> access;
  std::optional<std::string> parent_usr;
  bool resolved = false;
  int64_t multi_def = 0; // v27: number of definitions (bodies); >1 == redefined
                         // per backend. Set at resolve, not by add_symbol.
  std::optional<std::string> const_value; // v33: evaluated constant initializer
                                          // (variable) or enumerator value;
                                          // NULL for runtime initializers
  int64_t semantic_universe_id = -1;      // v35: database-local scope row
  std::string identity_key; // v35: portable scope-keyed semantic identity
  // Transient producer hint; never persisted as a column.
  std::optional<std::string> identity_source;
  // Transient translation-unit/build identity; never persisted as a column.
  std::optional<std::string> identity_translation_unit;
  int64_t id = -1;
};

struct GraphEdgeRow {
  int64_t eid = -1;
  int64_t src_id = -1;
  int64_t dst_id = -1;
  int64_t ekind = 0;
  int64_t ecount = 0;
  int64_t rawcount = 0;
  std::optional<int64_t> base_access;
  std::optional<int64_t> is_virtual;
  Symbol sym;
};

// Complete input payload for minting a reference/stub symbol. These fields
// mirror Storage::mint_symbol_id so a port migration cannot lose identity,
// declaration, linkage, or semantic-universe information.
struct SymbolIdentityRecord {
  std::string usr;
  std::string spelling;
  std::string qual_name;
  std::string display_name;
  std::string kind = "function";
  std::optional<int64_t> decl_file_id;
  std::optional<int64_t> decl_line;
  std::optional<int64_t> decl_col;
  std::optional<std::string> decl_path;
  bool is_instantiation = false;
  bool is_named_instance = false;
  std::optional<std::string> type_info;
  std::optional<int64_t> semantic_universe_id;
  std::optional<std::string> identity_source;
  std::optional<std::string> linkage;
  std::optional<std::string> identity_translation_unit;
};

// -- v7 graph layer records ---------------------------------------------------

struct Edge {
  int64_t src_id = -1;
  int64_t dst_id = -1;
  int64_t kind = 0; // edge_kind.id
  int64_t count = 1;
  std::optional<int64_t> base_access; // inherits
  std::optional<int64_t> is_virtual;  // inherits (0/1)
  std::optional<int64_t> vtable_slot; // overrides (reserved)
  int64_t id = -1;
};

struct EdgeSite {
  int64_t edge_id = -1;
  std::optional<int64_t> file_id;
  std::optional<int64_t> line;
  std::optional<int64_t> col;
  int64_t conditional = 0;
  std::optional<std::string> args_sig;
  // Phase 2: receiver provenance for virtual dispatch
  std::optional<std::string> recv_src_kind;
  std::optional<std::string> recv_type_usr;
  std::optional<std::string> recv_decl_usr;
  std::optional<int64_t>
      recv_param_pos; // 0-based index of receiver in callee params
  std::optional<int64_t>
      recv_type_is_value; // v11: receiver held by value (1) else 0/NULL
};

struct CallArg {
  int64_t edge_id = -1;
  int64_t file_id = -1;
  int64_t line = 0;
  int64_t col = 0;
  int64_t position = 0;
  std::string src_kind; // local|construct|member|global|call_result|unknown
  std::optional<std::string> type_usr;
  std::optional<std::string> decl_usr;
  std::optional<std::string> callee_usr;
  std::optional<int64_t>
      type_is_value; // v11: arg held by value (1) else 0/NULL
};

struct TemplateParam {
  int64_t owner_id = -1;
  int64_t position = 0;
  int64_t param_kind = 0;
  std::optional<std::string> name;
  std::optional<std::string> default_txt;
  std::optional<int64_t> type_id;
  std::optional<int64_t> default_type_id;
  std::optional<int64_t> default_ref_id;
};

struct TemplateArg {
  int64_t owner_id = -1;
  int64_t position = 0;
  int64_t pack_index = -1;
  int64_t arg_kind = 0;
  std::optional<int64_t> ref_id;
  std::optional<std::string> literal;
  std::optional<int64_t> type_id;
};

// -- v30 signature/type tier records
// -------------------------------------------

// One normalized type shape (type_node row). Identity is `type_key`, a
// deterministic structural encoding of the Clang type (see ast/type_graph.cpp
// for the grammar); `spelling` is display-only. `decl_usr` names the
// record/enum/alias declaration this layer resolves to (NULL for builtins,
// pointers, ...). `canonical_id` links a sugared node to its canonical shape
// (NULL when the node is itself canonical).
struct TypeNode {
  int64_t id = -1;
  std::string type_key;
  std::string spelling;
  int64_t kind = 0; // type_kind.id (1=builtin .. 14=pack-expansion)
  bool is_const = false;
  bool is_volatile = false;
  bool is_restrict = false;
  std::optional<std::string> decl_usr;
  std::optional<int64_t> canonical_id;
  std::optional<std::string> extent;
};

struct EntityNode {
  int64_t id = -1;
  int64_t kind = 0;
  std::string kind_name;
};

struct EntityEdge {
  int64_t src_id = -1;
  int64_t dst_id = -1;
  int64_t kind = 0;
  std::string kind_name;
  int64_t count = 0;
  std::optional<int64_t> via_member_id;
  int64_t multiplicity = 1;
  int64_t access = 0;
  int64_t is_virtual = 0;
  std::optional<int64_t> create_form;
  int64_t partial = 0;
};

struct TypeEdge {
  int64_t src_id = -1;
  int64_t kind = 0;
  int64_t position = 0;
  int64_t dst_id = -1;
};

// type_kind ids (seeded in the type_kind table; mirrored in storage.py)
inline constexpr int64_t kTypeKindBuiltin = 1;
inline constexpr int64_t kTypeKindRecord = 2;
inline constexpr int64_t kTypeKindEnum = 3;
inline constexpr int64_t kTypeKindAlias = 4;
inline constexpr int64_t kTypeKindPointer = 5;
inline constexpr int64_t kTypeKindLValueRef = 6;
inline constexpr int64_t kTypeKindRValueRef = 7;
inline constexpr int64_t kTypeKindArray = 8;
inline constexpr int64_t kTypeKindFunction = 9;
inline constexpr int64_t kTypeKindTemplateParam = 10;
inline constexpr int64_t kTypeKindOther = 11;
inline constexpr int64_t kTypeKindMemberDataPointer = 12;
inline constexpr int64_t kTypeKindMemberFunctionPointer = 13;
inline constexpr int64_t kTypeKindPackExpansion = 14;

// type_edge kinds (seeded in type_edge_kind; mirrored in storage.py)
inline constexpr int64_t kTypeEdgePointee = 1;     // pointer/reference -> inner
inline constexpr int64_t kTypeEdgeElement = 2;     // array -> element
inline constexpr int64_t kTypeEdgeAliasOf = 3;     // alias -> one-step target
inline constexpr int64_t kTypeEdgeReturnType = 4;  // function type -> return
inline constexpr int64_t kTypeEdgeParamType = 5;   // function type -> param i
inline constexpr int64_t kTypeEdgeTemplateArg = 6; // specialization -> arg i
inline constexpr int64_t kTypeEdgeMemberOwner = 7; // member pointer -> owner
inline constexpr int64_t kTypeEdgeMemberComponent = 8; // member pointer -> type

// symbol_type kinds (seeded in symbol_type_kind; mirrored in storage.py)
inline constexpr int64_t kSymbolTypeReturns = 1;    // callable -> return type
inline constexpr int64_t kSymbolTypeOfType = 2;     // variable/field -> type
inline constexpr int64_t kSymbolTypeUnderlying = 3; // typedef/alias -> target

// One parameter of a callable symbol. Identity is (owner_id, position); the
// name and source site are optional attributes (unnamed parameters, built
// declarations). type_id points into type_node.
struct Parameter {
  int64_t owner_id = -1;
  int64_t position = 0;
  int64_t pack_index = -1;
  std::optional<std::string> name;
  std::optional<int64_t> type_id;
  std::optional<int64_t> declared_type_id;
  std::optional<int64_t> adjusted_type_id;
  std::optional<std::string> default_text;
  std::optional<std::string> default_origin;
  std::optional<std::string> reference_semantics;
  std::optional<int64_t> file_id;
  std::optional<int64_t> line;
  std::optional<int64_t> col;
};

// -- v31 include tier records
// --------------------------------------------------

// One normalized compilation configuration. `digest` is the stable identity
// used by plan-freshness checks; it is computed by include_config_digest()
// over the same fields stored here, so a plan can be revalidated without the
// original compile database.
struct IncludeConfig {
  int64_t id = -1;
  int64_t tu_file_id = -1;
  std::string digest;
  std::optional<std::string> driver;
  std::optional<std::string> working_dir;
  std::vector<std::string> arguments;
  std::optional<std::string> lang_mode; // "c" | "c++"
  std::optional<std::string> resource_dir;
  std::optional<int64_t> translation_unit_config_id;
};

enum class TranslationUnitConfigState : std::uint8_t {
  registered,
  unregistered,
  ambiguous,
  stale,
  unavailable,
};

struct TranslationUnitConfig {
  int64_t id = -1;
  std::string descriptor_hash;
  std::string descriptor_json;
  std::optional<std::string> driver;
  std::optional<std::string> working_dir;
  std::optional<std::string> language;
  std::optional<std::string> standard;
  std::optional<std::string> target;
  std::vector<std::string> abi_options;
  std::optional<std::string> sysroot;
  std::optional<std::string> resource_dir;
  std::vector<std::string> include_paths;
  std::vector<std::string> macro_state;
  std::vector<std::string> relevant_environment;
  std::vector<std::string> generated_inputs;
  std::optional<std::string> diagnostics_policy;
  std::vector<std::string> arguments;
  TranslationUnitConfigState state = TranslationUnitConfigState::registered;
  // State of this file's association with the descriptor. It can become
  // stale while the descriptor remains reusable for another TU.
  TranslationUnitConfigState association_state =
      TranslationUnitConfigState::registered;
};

struct FileConfigApplicability {
  int64_t file_id = -1;
  int64_t config_id = -1;
  std::string role = "header";
  TranslationUnitConfigState state = TranslationUnitConfigState::registered;
  std::optional<std::string> reason;
};

// A collapsed file->file include relation under one configuration.
// dst_file_id is nullopt when the target is a system header, unowned by any
// component, or unresolved; dst_path always carries the path as opened (or the
// written spelling for an unresolved directive).
struct IncludeEdge {
  int64_t id = -1;
  int64_t src_file_id = -1;
  std::optional<int64_t> dst_file_id;
  std::string dst_path;
  int64_t config_id = -1;
  bool is_system = false;
  bool is_generated = false;
  int64_t count = 1;
};

struct ConfiguredIncludeEdges {
  std::vector<IncludeEdge> edges;
  bool coverage_complete = false;
};

enum class FactCoverage : std::uint8_t { one, all, invariant };

// Configuration-qualified semantic facts.  `coverage_complete=false` is an
// explicit unknown result: at least one requested configuration has not
// produced a usable fact generation for this file.
struct ConfiguredSymbols {
  std::vector<Symbol> symbols;
  bool coverage_complete = false;
};

struct ConfiguredFactIds {
  std::vector<int64_t> ids;
  bool coverage_complete = false;
};

// include_directive_kind ids (seeded in the table; mirrored in storage.py)
inline constexpr int64_t kIncludeDirectiveInclude = 1;
inline constexpr int64_t kIncludeDirectiveIncludeNext = 2;
inline constexpr int64_t kIncludeDirectiveImport = 3;
inline constexpr int64_t kIncludeDirectiveIncludeMacros = 4;
inline constexpr int64_t kIncludeDirectiveUnknown = 5;

// One directive occurrence. [begin_offset, end_offset) is the exact removal
// range in the source buffer, measured in bytes from the start of the file so
// replacements never depend on line renumbering.
struct IncludeSite {
  int64_t id = -1;
  int64_t edge_id = -1;
  int64_t line = 0;
  int64_t col = 0;
  int64_t begin_offset = 0;
  int64_t end_offset = 0;
  std::string spelling; // as written, without <> or ""
  bool is_angled = false;
  int64_t directive = kIncludeDirectiveInclude;
  std::string cond_fingerprint; // "" = unconditional top level
  bool resolved = true;
  bool guarded = false;
};

// A macro expanded in `src_file_id` whose definition lives in `def_path`.
struct IncludeMacroUse {
  int64_t src_file_id = -1;
  std::string def_path;
  std::string name;
  int64_t config_id = -1;
  int64_t count = 1;
};

struct Stats {
  int64_t components = 0;
  int64_t directories = 0;
  int64_t files = 0;
  int64_t files_indexed = 0;
  int64_t symbols = 0;
  int64_t symbols_unresolved = 0;
  std::map<std::string, int64_t> symbols_by_kind;
  int64_t edges = 0;
  std::map<std::string, int64_t> edges_by_kind;
};

// Query-facing graph rows. These records deliberately live beside the other
// domain values instead of reusing Storage's SQLite-shaped nested rows.
struct GraphEdgeRecord {
  int64_t edge_id = -1;
  int64_t src_id = -1;
  int64_t dst_id = -1;
  int64_t kind = 0;
  int64_t count = 0;
  int64_t raw_count = 0;
  std::optional<int64_t> base_access;
  std::optional<int64_t> is_virtual;
  Symbol target;
};

struct GraphEdgeSiteRecord {
  int64_t edge_id = -1;
  std::optional<int64_t> file_id;
  std::optional<int64_t> line;
  std::optional<int64_t> col;
  bool conditional = false;
  std::optional<std::string> args_sig;
  std::optional<std::string> recv_src_kind;
  std::optional<std::string> recv_type_usr;
  std::optional<std::string> recv_decl_usr;
  std::optional<int64_t> recv_param_pos;
  std::optional<int64_t> recv_type_is_value;
};

struct DefinitionRecord {
  int64_t symbol_id = -1;
  std::optional<int64_t> file_id;
  std::optional<int64_t> line;
  std::optional<int64_t> col;
  std::optional<int64_t> end_line;
  std::optional<int64_t> end_col;
  std::optional<std::string> init_text;
};

} // namespace cidx
