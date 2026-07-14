#include "ast/symbol_visitor.hpp"

#include "ast/location.hpp"
#include "ast/symbol_emitter.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/Basic/SourceManager.h"

namespace cidx::ast {

namespace {

// The decl is a template's PATTERN (templated decl); libclang represents the
// pair as one FunctionTemplate/ClassTemplate cursor, so the pattern itself is
// not a symbol. A ClassTemplatePartialSpecializationDecl is NOT a pattern
// (getDescribedClassTemplate() is null on it): it is retained as a
// first-class template symbol.
bool is_template_pattern(const clang::NamedDecl *decl) {
  if (const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(decl))
    return fn->getDescribedFunctionTemplate() != nullptr;
  if (const auto *rec = llvm::dyn_cast<clang::CXXRecordDecl>(decl))
    return rec->getDescribedClassTemplate() != nullptr;
  if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
    return var->getDescribedVarTemplate() != nullptr;
  if (const auto *alias = llvm::dyn_cast<clang::TypeAliasDecl>(decl))
    return alias->getDescribedAliasTemplate() != nullptr;
  return false;
}

// Body-scoped named type declarations that ARE indexed even though they live
// inside a function/method body (is_local_symbol_kind, ast_cursor.cpp:96-103).
// Local variables and local function declarations are absent by design.
bool is_local_symbol_decl(const clang::NamedDecl *decl) {
  switch (decl->getKind()) {
  case clang::Decl::Typedef:
  case clang::Decl::TypeAlias:
  case clang::Decl::Enum:
  case clang::Decl::EnumConstant:
  case clang::Decl::Record:
  case clang::Decl::CXXRecord:
  case clang::Decl::Field:
  case clang::Decl::CXXMethod:
  case clang::Decl::CXXConstructor:
  case clang::Decl::CXXDestructor:
    return true;
  default:
    return false;
  }
}

} // namespace

SymbolVisitor::SymbolVisitor(clang::ASTContext &context, SymbolEmitter &out,
                             std::string target_file)
    : context_(context), source_manager_(context.getSourceManager()),
      extractor_(context), out_(out), target_file_(std::move(target_file)) {}

bool SymbolVisitor::should_emit(const clang::NamedDecl *decl) const {
  // cidx's symbol phase covers the main file AND owned (non-system) headers,
  // each under its own file_id; system headers are skipped entirely
  // (_ignore_system_headers). In per-file mode only the target file's decls
  // are emitted (for_file_cursors' pruning); in whole-TU mode records carry
  // their file and the parity merger replays cidx's ordering.
  const clang::SourceLocation loc =
      source_manager_.getExpansionLoc(decl->getLocation());
  if (loc.isInvalid() || source_manager_.isInSystemHeader(loc))
    return false;
  if (source_manager_.getFilename(loc).empty())
    return false;
  if (!target_file_.empty() &&
      expansion_loc(context_, decl->getLocation()).file != target_file_)
    return false;

  if (is_template_pattern(decl))
    return false;

  // Body-scope policy: within a function/method only named-type locals are
  // symbols (for_body_local_symbols); local vars feed reference sites, not
  // the symbol table.
  if (decl->getParentFunctionOrMethod() != nullptr &&
      !is_local_symbol_decl(decl))
    return false;

  return true;
}

bool SymbolVisitor::VisitNamedDecl(clang::NamedDecl *decl) {
  if (!should_emit(decl))
    return true;
  if (std::optional<SymbolRecord> sym = extractor_.extract(decl))
    out_.emit(*sym);
  return true;
}

// Explicit instantiations (`template double twice<double>(double);`) are
// never lexical decls — they exist only in the template's specializations()
// list, which the default traversal skips. Emit them here so an uncalled
// explicit instantiation is still a symbol. should_emit gates each one on the
// pattern's location, matching where the instantiated declaration points.
bool SymbolVisitor::VisitFunctionTemplateDecl(
    clang::FunctionTemplateDecl *decl) {
  for (const clang::FunctionDecl *fd : decl->specializations()) {
    const clang::TemplateSpecializationKind tsk =
        fd->getTemplateSpecializationKind();
    if (tsk != clang::TSK_ExplicitInstantiationDeclaration &&
        tsk != clang::TSK_ExplicitInstantiationDefinition)
      continue;
    if (!should_emit(fd))
      continue;
    if (std::optional<SymbolRecord> sym = extractor_.extract(fd))
      out_.emit(*sym);
  }
  return true;
}

} // namespace cidx::ast
