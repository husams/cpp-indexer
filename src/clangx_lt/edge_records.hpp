// Plain-data payloads exchanged between the edge visitors and the EdgeSink.
// Mirrors the `edge` / `template_param` / `template_arg` rows and the
// mint_symbol_id() parameters (storage.cpp) — no clang types leak out.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace cidx::lt {

struct EdgeRecord {
  int64_t src_id = 0;
  int64_t dst_id = 0;
  int64_t kind = 0; // edge_kind id (1=calls .. 18=dispatch_calls)
  int64_t count = 1;
  std::optional<int64_t> base_access; // inherits: 1=public 2=protected 3=private
  std::optional<int64_t> is_virtual;  // inherits: virtual base
};

// mint_symbol_id() payload: a USR-keyed stub for a symbol that may not be
// indexed yet (external base, overridden method in another file, ...).
struct MintRequest {
  std::string usr;
  std::string spelling;
  std::string qual_name;
  std::string display_name;
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
};

struct TemplateArgRecord {
  int64_t owner_id = 0;
  int64_t position = 0;
  int64_t arg_kind = 0; // 1=type, 2=integral, else raw CXTemplateArgumentKind
  std::optional<int64_t> ref_id;
  std::optional<std::string> literal;
};

// Candidate row for template-arg reference resolution (subset of Symbol).
struct TypeArgCandidate {
  int64_t id = 0;
  std::string kind_name;
  bool is_instantiation = false;
};

} // namespace cidx::lt
