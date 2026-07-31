#include "ast/symbol_visitor.hpp"

#include "ast/instantiation_edges.hpp"
#include "ast/location.hpp"
#include "ast/pass_registry.hpp"
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
  if (const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
    return fn->getDescribedFunctionTemplate() != nullptr;
  }
  if (const auto *rec = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
    return rec->getDescribedClassTemplate() != nullptr;
  }
  if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl)) {
    return var->getDescribedVarTemplate() != nullptr;
  }
  if (const auto *alias = llvm::dyn_cast<clang::TypeAliasDecl>(decl)) {
    return alias->getDescribedAliasTemplate() != nullptr;
  }
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
                             std::string target_file, PassMetrics *metrics,
                             FileRouter router)
    : context_(context), source_manager_(context.getSourceManager()),
      extractor_(context), out_(out), target_file_(std::move(target_file)),
      metrics_(metrics), router_(std::move(router)) {}

bool SymbolVisitor::VisitDecl(clang::Decl * /*decl*/) {
  if (metrics_ != nullptr) {
    metrics_->note_visited();
  }
  return true;
}

bool SymbolVisitor::should_emit(const clang::NamedDecl *decl) {
  // cidx's symbol phase covers the main file AND owned (non-system) headers,
  // each under its own file_id; system headers are skipped entirely
  // (_ignore_system_headers). In per-file mode only the target file's decls
  // are emitted (for_file_cursors' pruning); in whole-TU mode records carry
  // their file and the parity merger replays cidx's ordering.
  candidate_location_.reset();
  const clang::SourceLocation loc =
      source_manager_.getExpansionLoc(decl->getLocation());
  if (loc.isInvalid() || source_manager_.isInSystemHeader(loc)) {
    return false;
  }
  const unsigned file_key = source_manager_.getFileID(loc).getHashValue();
  ExpansionLoc location;
  if (const auto cached = expansion_files_.find(file_key);
      cached != expansion_files_.end()) {
    location.file = cached->second;
    location.line = source_manager_.getExpansionLineNumber(loc);
    location.col = source_manager_.getExpansionColumnNumber(loc);
  } else {
    location = expansion_loc_from_expanded(source_manager_, loc);
    expansion_files_.emplace(file_key, location.file);
  }
  if (location.file.empty()) {
    return false;
  }
  if (!target_file_.empty() && location.file != target_file_) {
    return false;
  }
  if (router_ && !router_(location.file)) {
    return false;
  }

  if (is_template_pattern(decl)) {
    return false;
  }

  // Body-scope policy: within a function/method only named-type locals are
  // symbols (for_body_local_symbols); local vars feed reference sites, not
  // the symbol table.
  if (decl->getParentFunctionOrMethod() != nullptr &&
      !is_local_symbol_decl(decl)) {
    return false;
  }

  candidate_location_ = location;
  return true;
}

bool SymbolVisitor::VisitNamedDecl(clang::NamedDecl *decl) {
  if (!should_emit(decl)) {
    return true;
  }
  if (!candidate_location_) {
    return true;
  }
  if (std::optional<SymbolRecord> sym =
          extractor_.extract(decl, *candidate_location_)) {
    out_.emit(*sym);
  }
  return true;
}

// Explicit instantiations (`template double twice<double>(double);`,
// `template void PlainMember<int>::run();`) are never lexical decls — they
// exist only in specialization lists, which the default traversal skips. The
// decl they create points at the template PATTERN, so ownership, location,
// and cleanup are anchored to getPointOfInstantiation().
//
// Precision of that anchor is bounded by Clang's data model: a function
// explicit-instantiation statement produces NO node of its own (the
// RecursiveASTVisitor FIXME "once they are represented as dedicated nodes"
// is still open in LLVM 22) and the specialization carries a SINGLE,
// first-write-wins PointOfInstantiation slot. The anchor is therefore the
// specialization's FIRST materialization point in the TU: a preceding
// implicit use, or the `extern template` declaration when one precedes the
// definition. That first point is where ownership and cleanup live; when
// the statement holding it is removed, reindexing re-anchors the symbol to
// the next remaining point. The definition-directive's own location is not
// recoverable from the AST when an earlier point exists.
void SymbolVisitor::emit_explicit_instantiation(const clang::FunctionDecl *fd) {
  const clang::SourceLocation poi = fd->getPointOfInstantiation();
  if (poi.isInvalid() ||
      source_manager_.isInSystemHeader(source_manager_.getExpansionLoc(poi))) {
    return;
  }
  const ExpansionLoc loc = expansion_loc(context_, poi);
  if (loc.file.empty()) {
    return;
  }
  if (!target_file_.empty() && loc.file != target_file_) {
    return;
  }
  if (router_ && !router_(loc.file)) {
    return;
  }
  std::optional<SymbolRecord> sym = extractor_.extract(fd);
  if (!sym) {
    return;
  }
  sym->file = loc.file;
  sym->line = loc.line;
  sym->col = loc.col;
  sym->end_line = loc.line;
  sym->end_col = loc.col;
  if (sym->decl_line) { // declaration-only instantiations record the POI too
    sym->decl_line = loc.line;
    sym->decl_col = loc.col;
  }
  out_.emit(*sym);
}

bool SymbolVisitor::VisitFunctionTemplateDecl(
    clang::FunctionTemplateDecl *decl) {
  for_each_explicit_callable_instantiation(
      decl, [this](const clang::FunctionDecl *fd) {
        emit_explicit_instantiation(fd);
      });
  return true;
}

bool SymbolVisitor::VisitClassTemplateDecl(clang::ClassTemplateDecl *decl) {
  for_each_explicit_callable_instantiation(
      decl, [this](const clang::FunctionDecl *fd) {
        emit_explicit_instantiation(fd);
      });
  return true;
}

} // namespace cidx::ast
