#include "ast/symbol_extractor.hpp"

#include "ast/clang_compat.hpp"
#include "ast/decl_flags.hpp"
#include "ast/kind_map.hpp"
#include "ast/location.hpp"
#include "ast/names.hpp"
#include "ast/usr.hpp"

#include "clang/AST/APValue.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "llvm/Support/raw_ostream.h"

namespace cidx::ast {

namespace {

// v33: values longer than this are dropped rather than truncated -- a sliced
// constant (half an array aggregate) would read as a different, wrong value.
constexpr std::size_t kMaxConstValueLen = 512;

// v33: the evaluated constant value of a variable initializer or an
// enumerator. Clang's constant evaluator does all the arithmetic (constexpr,
// consteval calls, `if consteval` bodies included); this only records its
// printed result. Variables whose initializer isn't a constant expression --
// and anything dependent -- record nothing.
std::optional<std::string> const_value_of(const clang::ASTContext &context,
                                          const clang::NamedDecl *decl) {
  if (const auto *enumerator = llvm::dyn_cast<clang::EnumConstantDecl>(decl)) {
    return compat::integral_to_string(enumerator->getInitVal());
  }
  const auto *var = llvm::dyn_cast<clang::VarDecl>(decl);
  if (var == nullptr || llvm::isa<clang::ParmVarDecl>(var) || !var->hasInit() ||
      var->isTemplated() || var->getType()->isDependentType()) {
    return std::nullopt;
  }
  // evaluateValue caches the result on the decl and returns nullptr when the
  // initializer needs runtime evaluation.
  const clang::APValue *value = var->evaluateValue();
  if (value == nullptr || !value->hasValue()) {
    return std::nullopt;
  }
  std::string text;
  llvm::raw_string_ostream os(text);
  value->printPretty(os, context, var->getType());
  if (text.empty() || text.size() > kMaxConstValueLen) {
    return std::nullopt;
  }
  return text;
}

// (line, col)..(end_line, end_col) slices the WHOLE declaration: extent
// start/end, not the identifying spelling location (to_symbol). A declaration
// cursor records itself as the decl site; definitions leave the decl fields
// for the upsert to keep (to_symbol).
void fill_extent(const clang::ASTContext &context, const clang::NamedDecl *decl,
                 bool is_def, const ExpansionLoc *declaration_location,
                 SymbolRecord &sym) {
  const ExpansionLoc start = extent_start(context, decl->getSourceRange());
  sym.line = start.line;
  sym.col = start.col;
  const ExpansionLoc end = extent_end(context, decl->getSourceRange());
  sym.end_line = end.line;
  sym.end_col = end.col;
  if (!is_def) {
    const ExpansionLoc loc = declaration_location != nullptr
                                 ? *declaration_location
                                 : expansion_loc(context, decl->getLocation());
    sym.decl_line = loc.line;
    sym.decl_col = loc.col;
  }
}

// Parent USR: the semantic parent unless it is the TU (to_symbol,
// ast_symbols.cpp:24-33).
std::optional<std::string> parent_usr_of(const clang::NamedDecl *decl) {
  const clang::DeclContext *dc = decl->getDeclContext();
  if (dc == nullptr) {
    return std::nullopt;
  }
  const auto *parent = llvm::dyn_cast<clang::Decl>(dc);
  if (parent == nullptr || llvm::isa<clang::TranslationUnitDecl>(parent)) {
    return std::nullopt;
  }
  std::string usr = usr_for_decl(parent);
  if (usr.empty()) {
    return std::nullopt;
  }
  return usr;
}

} // namespace

SymbolExtractor::SymbolExtractor(const clang::ASTContext &context)
    : context_(context) {}

std::optional<SymbolRecord>
SymbolExtractor::extract(const clang::NamedDecl *decl) const {
  return extract(decl, expansion_loc(context_, decl->getLocation()));
}

std::optional<SymbolRecord>
SymbolExtractor::extract(const clang::NamedDecl *decl,
                         const ExpansionLoc &declaration_location) const {
  const int kind = cidx_symbol_kind(decl);
  if (kind < 0) {
    return std::nullopt;
  }

  std::string usr = usr_for_decl(decl);
  if (usr.empty()) {
    return std::nullopt; // no USR -> not indexable
  }

  const bool is_def = is_definition(decl);

  SymbolRecord sym;
  sym.file = declaration_location.file;
  sym.usr = std::move(usr);
  sym.spelling = spelling(decl);
  sym.kind = kind;
  std::string qual = qualified_name(context_, decl);
  if (!qual.empty()) {
    sym.qual_name = std::move(qual);
  }
  sym.display_name = display_name(context_, decl);
  sym.type_info = type_info(context_, decl);
  fill_extent(context_, decl, is_def, &declaration_location, sym);

  sym.is_definition = is_def;
  sym.is_pure = is_pure_virtual_method(decl);
  sym.is_static = is_static_method(decl);
  sym.is_instantiation = is_template_instantiation(decl);
  sym.callable_kind = callable_kind_name(decl);
  sym.template_origin = template_origin_name(context_, decl);
  sym.template_form = template_form_name(decl);
  sym.linkage = linkage_name(decl);
  sym.access = access_name(decl);
  sym.parent_usr = parent_usr_of(decl);
  sym.const_value = const_value_of(context_, decl);
  sym.resolved = is_def;
  return sym;
}

} // namespace cidx::ast
