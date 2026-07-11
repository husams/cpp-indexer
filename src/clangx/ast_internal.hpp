#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <clang-c/Index.h>

#include "clangx/ast.hpp"
#include "storage/storage.hpp"

namespace cidx::ast_detail {

struct ExpansionLoc {
  CXFile file = nullptr;
  unsigned line = 0;
  unsigned col = 0;
};

struct RefDeclLoc {
  std::optional<int64_t> file_id;
  std::optional<int64_t> line;
  std::optional<int64_t> col;
  std::optional<std::string> path;
};

const char *kind_name(CXCursorKind kind);
std::string stub_kind(CXCursor cursor);
bool is_function_like(CXCursorKind kind);
bool is_invalid_kind(CXCursorKind kind);

ExpansionLoc cursor_location(CXCursor cursor);
ExpansionLoc cursor_extent_start(CXCursor cursor);
ExpansionLoc cursor_extent_end(CXCursor cursor);
RefDeclLoc ref_decl_loc(Storage &db, CXCursor cursor);
void emit_type_use(Storage &db, int64_t src_id, CXType type,
                   int64_t file_id, CXCursor site_cursor, int conditional);
std::optional<std::string> linkage_name(CXLinkageKind linkage);
std::optional<std::string> access_name(CX_CXXAccessSpecifier access);
std::string qualified_name(CXCursor cursor);
void for_body_local_symbols(CXCursor fn_cursor, const std::string &filename,
                            const std::function<void(CXCursor)> &fn);
void emit_namespace_uses(Storage &db, const ParsedTu &tu,
                         const std::string &filename, int64_t file_id);

bool is_cond_cursor(CXCursorKind kind);
bool is_explicit_instantiation(CXCursor cursor);
int index_cursor_template_args(Storage &db, int64_t owner_id,
                               CXCursor cursor);
void index_method_template_args_from_tokens(Storage &db, int64_t owner_id,
                                            CXCursor cursor,
                                            const std::string &display_name);
std::optional<int64_t>
resolve_template_arg_ref_id(Storage &db,
                            const std::optional<std::string> &literal,
                            CXCursor context_cursor);
void mint_instance_from_type(Storage &db, CXType type);
void mint_named_instance(Storage &db, CXCursor cursor);
void emit_static_init_def_edges(Storage &db, CXCursor cursor,
                                int64_t def_id);
std::optional<std::string> static_var_init_text(CXCursor cursor);

} // namespace cidx::ast_detail
