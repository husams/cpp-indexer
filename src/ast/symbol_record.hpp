// SymbolRecord: the full symbol-row payload (mirrors cidx::Symbol / the
// `symbol` table at schema 28). Plain data only — no clang types leak out of
// the extraction layer.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace cidx::ast {

struct SymbolRecord {
  std::string file; // expansion-loc file owning this cursor (file_id analogue)
  std::string usr;
  std::string spelling;
  int kind = -1; // CXCursorKind integer (kind_map.hpp)
  std::optional<std::string> qual_name;
  std::optional<std::string> display_name;
  std::optional<std::string> type_info;
  int64_t line = 0; // extent start (whole declaration slice)
  int64_t col = 0;
  int64_t end_line = 0; // one past the last token (libclang extent end)
  int64_t end_col = 0;
  std::optional<int64_t> decl_line; // spelling location; declarations only
  std::optional<int64_t> decl_col;
  std::optional<std::string> decl_path; // unregistered declaration source
  bool is_definition = false;
  bool is_pure = false;
  bool is_static = false;
  bool is_instantiation = false; // from TemplateSpecializationKind
  bool is_named_instance = false;
  std::optional<std::string> callable_kind;
  std::optional<std::string> template_origin;
  std::optional<std::string> template_form;
  std::optional<std::string> linkage;
  std::optional<std::string> access;
  std::optional<std::string> parent_usr;
  std::optional<std::string> const_value; // v33: evaluated constant initializer
                                          // (variable) or enumerator value
  bool resolved = false;                  // definition resolves the symbol
  // Portable identity dimensions. Empty values select the recorder's current
  // partition defaults and never imply a database row id.
  std::string semantic_universe;
  std::string normalized_configuration;
  std::optional<std::string> identity_source;
  std::optional<std::string> identity_translation_unit;
  std::optional<std::string> local_anchor;
  // Storage kind name is carried explicitly for database-free candidate
  // indexes. Visitors may leave it empty when kind has a standard mapping.
  std::string kind_name;
};

} // namespace cidx::ast
