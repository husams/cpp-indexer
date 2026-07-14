#include "ast/symbol_extractor.hpp"

#include "ast/decl_flags.hpp"
#include "ast/kind_map.hpp"
#include "ast/location.hpp"
#include "ast/names.hpp"
#include "ast/usr.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"

namespace cidx::ast {

namespace {

// Parent USR: the semantic parent unless it is the TU (to_symbol,
// ast_symbols.cpp:24-33).
std::optional<std::string> parent_usr_of(const clang::NamedDecl *decl) {
  const clang::DeclContext *dc = decl->getDeclContext();
  if (dc == nullptr)
    return std::nullopt;
  const auto *parent = llvm::dyn_cast<clang::Decl>(dc);
  if (parent == nullptr || llvm::isa<clang::TranslationUnitDecl>(parent))
    return std::nullopt;
  std::string usr = usr_for_decl(parent);
  if (usr.empty())
    return std::nullopt;
  return usr;
}

} // namespace

SymbolExtractor::SymbolExtractor(const clang::ASTContext &context)
    : context_(context) {}

std::optional<SymbolRecord>
SymbolExtractor::extract(const clang::NamedDecl *decl) const {
  const int kind = cidx_symbol_kind(decl);
  if (kind < 0)
    return std::nullopt;

  std::string usr = usr_for_decl(decl);
  if (usr.empty())
    return std::nullopt; // no USR -> not indexable

  const bool is_def = is_definition(decl);

  SymbolRecord sym;
  sym.file = expansion_loc(context_, decl->getLocation()).file;
  sym.usr = std::move(usr);
  sym.spelling = spelling(decl);
  sym.kind = kind;
  std::string qual = qualified_name(context_, decl);
  if (!qual.empty())
    sym.qual_name = std::move(qual);
  sym.display_name = display_name(context_, decl);
  sym.type_info = type_info(context_, decl);

  // (line, col)..(end_line, end_col) slices the WHOLE declaration: extent
  // start/end, not the identifying spelling location (to_symbol).
  const ExpansionLoc start = extent_start(context_, decl->getSourceRange());
  sym.line = start.line;
  sym.col = start.col;
  const ExpansionLoc end = extent_end(context_, decl->getSourceRange());
  sym.end_line = end.line;
  sym.end_col = end.col;

  // A declaration cursor records itself as the decl site; definitions leave
  // the decl fields for the upsert to keep (to_symbol).
  if (!is_def) {
    const ExpansionLoc loc = expansion_loc(context_, decl->getLocation());
    sym.decl_line = loc.line;
    sym.decl_col = loc.col;
  }

  sym.is_definition = is_def;
  sym.is_pure = is_pure_virtual_method(decl);
  sym.is_static = is_static_method(decl);
  sym.linkage = linkage_name(decl);
  sym.access = access_name(decl);
  sym.parent_usr = parent_usr_of(decl);
  sym.resolved = is_def;
  return sym;
}

} // namespace cidx::ast
