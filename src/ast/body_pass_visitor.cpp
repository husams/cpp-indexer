#include "ast/body_pass_visitor.hpp"

#include "ast/body_walker.hpp"
#include "ast/edge_sink.hpp"
#include "ast/location.hpp"
#include "ast/usr.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/Basic/SourceManager.h"

namespace cidx::lt {

BodyPassVisitor::BodyPassVisitor(clang::ASTContext &context, EdgeSink &sink,
                                 std::string target_file, int64_t file_id)
    : context_(context), sink_(sink), target_file_(std::move(target_file)),
      file_id_(file_id) {}

bool BodyPassVisitor::VisitFunctionDecl(clang::FunctionDecl *decl) {
  // Definitions with an actual body, in the target file. The symbol row keyed
  // by this decl's USR: a templated pattern's body belongs to its
  // FUNCTION_TEMPLATE cursor (same USR either way).
  if (!decl->doesThisDeclarationHaveABody())
    return true;
  // for_file_cursors stops at bodies: a LOCAL class's methods never get their
  // own body walk (they are covered by the enclosing function's descent).
  if (decl->getParentFunctionOrMethod() != nullptr)
    return true;
  const ExpansionLoc loc = expansion_loc(context_, decl->getLocation());
  if (loc.file != target_file_)
    return true;
  const clang::NamedDecl *keyed = decl;
  if (const clang::FunctionTemplateDecl *ft =
          decl->getDescribedFunctionTemplate())
    keyed = ft;
  const std::string usr = usr_for_decl(keyed);
  if (usr.empty())
    return true;
  const auto fn_sym = sink_.lookup_symbol_id(usr);
  if (!fn_sym)
    return true;

  const clang::SourceRange range = keyed->getSourceRange();
  const ExpansionLoc start = extent_start(context_, range);
  const ExpansionLoc end = extent_end(context_, range);
  const int64_t def_id = sink_.get_or_create_definition(
      *fn_sym, file_id_, start.line, start.col, end.line, end.col,
      std::nullopt);
  BodyWalker walker(context_, sink_, *fn_sym, file_id_);
  walker.walk(decl);
  sink_.copy_body_edges_to_def_edge(def_id, *fn_sym);
  return true;
}

} // namespace cidx::lt
