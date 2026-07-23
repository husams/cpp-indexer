// Plain-data payloads exchanged between the edge visitors and the EdgeSink:
// the decl-pass records (edge / template_param / template_arg rows and the
// mint_symbol_id() parameters) and the body-pass records (edge_site /
// call_arg rows and value provenance). No clang types leak out.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace cidx::ast {

struct EdgeRecord {
  int64_t src_id = 0;
  int64_t dst_id = 0;
  int64_t kind = 0; // edge_kind id (1=calls .. 18=dispatch_calls, 19=alias_of)
  int64_t count = 1;
  std::optional<int64_t>
      base_access;                   // inherits: 1=public 2=protected 3=private
  std::optional<int64_t> is_virtual; // inherits: virtual base
};

// mint_symbol_id() payload: a USR-keyed stub for a symbol that may not be
// indexed yet (external base, overridden method in another file, ...).
struct MintRequest {
  std::string usr;
  std::string spelling;
  std::string qual_name;
  std::string display_name;
  std::optional<std::string> type_info; // cursor type / prototype, when known
  std::string kind_name; // storage kind NAME ("class", "function", ...)
  std::optional<int64_t> decl_file_id;
  std::optional<int64_t> decl_line;
  std::optional<int64_t> decl_col;
  std::optional<std::string> decl_path; // unregistered (system) header path
  bool is_instantiation = false;        // implicit template-instantiation node
  bool is_named_instance = false;       // X<B> minted from alias/member/local
};

struct TemplateParamRecord {
  int64_t owner_id = 0;
  int64_t position = 0;
  int64_t param_kind = 0; // 1=type 2=non-type 3=template
  std::optional<std::string> name;
  std::optional<std::string> default_txt;
  std::optional<int64_t> type_id;
  std::optional<int64_t> default_type_id;
  std::optional<int64_t> default_ref_id;
};

struct TemplateArgRecord {
  int64_t owner_id = 0;
  int64_t position = 0;
  int64_t pack_index = -1;
  int64_t arg_kind = 0; // 1=type, 2=integral, else raw CXTemplateArgumentKind
  std::optional<int64_t> ref_id;
  std::optional<std::string> literal;
  std::optional<int64_t> type_id;
};

// -- v30 signature/type tier records
// -------------------------------------------

// intern_type_node() payload: one normalized type shape (mirrors
// cidx::TypeNode minus the id). Identity is type_key; kind/edge-kind codes are
// the kTypeKind*/kTypeEdge*/kSymbolType* constants (storage/records.hpp).
struct TypeNodeRecord {
  std::string type_key;
  std::string spelling;
  int64_t kind = 0;
  bool is_const = false;
  bool is_volatile = false;
  bool is_restrict = false;
  std::optional<std::string> decl_usr;
  std::optional<int64_t> canonical_id;
};

// One parameter of a callable (replace_parameters payload).
struct ParameterRecord {
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

// Candidate row for template-arg reference resolution (subset of Symbol).
struct TypeArgCandidate {
  int64_t id = 0;
  std::string kind_name;
  bool is_instantiation = false;
};

// ---- body-pass records ------------------------------------------------------

struct EdgeSiteRecord {
  int64_t edge_id = 0;
  int64_t file_id = 0;
  int64_t line = 0;
  int64_t col = 0;
  int64_t conditional = 0;
  std::optional<std::string> recv_src_kind;
  std::optional<std::string> recv_type_usr;
  std::optional<std::string> recv_decl_usr;
  std::optional<int64_t> recv_param_pos;
  std::optional<int64_t> recv_type_is_value;
};

struct CallArgRecord {
  int64_t edge_id = 0;
  int64_t file_id = 0;
  int64_t line = 0;
  int64_t col = 0;
  int64_t position = 0;
  std::string src_kind;
  std::optional<std::string> type_usr;
  std::optional<std::string> decl_usr;
  std::optional<std::string> callee_usr;
  std::optional<int64_t> type_is_value;
};

// classify_value_source result (ast_body.cpp ValueSource).
struct ValueSource {
  std::string
      src_kind; // local|construct|member|global|call_result|literal|this|unknown
  std::string type_usr;
  std::string decl_usr;
  std::string callee_usr; // call_result only
};

} // namespace cidx::ast
