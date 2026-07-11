#include "clangx_lt/symbol_visitor.hpp"

#include "clangx_lt/kind_map.hpp"
#include "clangx_lt/symbol_emitter.hpp"
#include "clangx_lt/usr.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/SourceManager.h"

namespace cidx::lt {

SymbolVisitor::SymbolVisitor(clang::ASTContext &context, SymbolEmitter &out)
    : context_(context), source_manager_(context.getSourceManager()),
      out_(out) {}

bool SymbolVisitor::VisitNamedDecl(clang::NamedDecl *decl) {
  // cidx only indexes decls whose expansion location is the target file.
  const clang::SourceLocation loc =
      source_manager_.getExpansionLoc(decl->getLocation());
  if (loc.isInvalid())
    return true;
  if (!source_manager_.isWrittenInMainFile(loc))
    return true;

  const int kind = cidx_symbol_kind(decl);
  if (kind < 0)
    return true;

  std::string usr = usr_for_decl(decl);
  if (usr.empty())
    return true;

  out_.emit(SymbolRecord{std::move(usr), kind, decl->getQualifiedNameAsString()});
  return true;
}

} // namespace cidx::lt
