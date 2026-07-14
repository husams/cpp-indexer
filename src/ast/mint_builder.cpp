#include "ast/mint_builder.hpp"

#include "ast/edge_sink.hpp"
#include "ast/kind_map.hpp"
#include "ast/location.hpp"
#include "ast/names.hpp"
#include "ast/usr.hpp"

#include "clang/AST/Decl.h"

namespace cidx::ast {

MintBuilder::MintBuilder(const clang::ASTContext &context, EdgeSink &sink)
    : context_(context), sink_(sink) {}

std::optional<MintRequest>
MintBuilder::build(const clang::NamedDecl *decl) const {
  std::string usr = usr_for_decl(decl);
  if (usr.empty())
    return std::nullopt;

  MintRequest req;
  req.usr = std::move(usr);
  req.spelling = spelling(decl);
  req.qual_name = qualified_name(context_, decl);
  req.display_name = display_name(context_, decl).value_or(std::string());
  req.kind_name = cidx_stub_kind_name(decl);

  // ref_decl_loc (ast_cursor.cpp:161): the decl's own location; registered
  // file -> decl_file_id, otherwise keep the raw path so system/stdlib
  // targets stay located instead of @<no-location>.
  const ExpansionLoc loc = expansion_loc(context_, decl->getLocation());
  if (!loc.file.empty()) {
    req.decl_line = loc.line;
    req.decl_col = loc.col;
    if (std::optional<int64_t> fid = sink_.file_id_for_path(loc.file))
      req.decl_file_id = fid;
    else
      req.decl_path = loc.file;
  }
  return req;
}

} // namespace cidx::ast
