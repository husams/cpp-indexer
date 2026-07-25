#include "ast/namespace_use_visitor.hpp"

#include "ast/fact_emitters.hpp"
#include "ast/location.hpp"
#include "ast/pass_registry.hpp"
#include "ast/usr.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/NestedNameSpecifier.h"
#include "llvm/Config/llvm-config.h"

namespace cidx::ast {

namespace {

// _SCOPE_KINDS: decls that establish an enclosing edge source.
bool is_scope_decl(const clang::Decl *d) {
  return llvm::isa<clang::FunctionDecl>(d) ||
         llvm::isa<clang::FunctionTemplateDecl>(d) ||
         llvm::isa<clang::TagDecl>(d) ||
         llvm::isa<clang::ClassTemplateDecl>(d) ||
         llvm::isa<clang::NamespaceDecl>(d) || llvm::isa<clang::VarDecl>(d) ||
         llvm::isa<clang::FieldDecl>(d) || llvm::isa<clang::TypedefNameDecl>(d);
}

} // namespace

NamespaceUseVisitor::NamespaceUseVisitor(clang::ASTContext &context,
                                         NamespacePassPorts &ports,
                                         std::string target_file,
                                         int64_t file_id)
    : context_(context), ports_(ports), target_file_(std::move(target_file)),
      file_id_(file_id) {}

NamespaceUseVisitor::NamespaceUseVisitor(clang::ASTContext &context,
                                         NamespacePassPorts &ports,
                                         std::string target_file,
                                         int64_t file_id,
                                         PassMetrics *metrics)
    : context_(context), ports_(ports), target_file_(std::move(target_file)),
      file_id_(file_id), metrics_(metrics) {}

bool NamespaceUseVisitor::VisitDecl(clang::Decl * /*decl*/) {
  if (metrics_ != nullptr) {
    metrics_->note_visited();
  }
  return true;
}

bool NamespaceUseVisitor::in_target_file(const clang::Decl *decl) const {
  return expansion_loc(context_, decl->getLocation()).file == target_file_;
}

// The indexed symbol id a scope-establishing decl contributes to the
// enclosing-source stack (nullopt when not a scope, unnamed, or unindexed).
std::optional<int64_t>
NamespaceUseVisitor::scope_symbol_id(const clang::Decl *decl) const {
  if (!is_scope_decl(decl)) {
    return std::nullopt;
  }
  const auto *nd = llvm::dyn_cast<clang::NamedDecl>(decl);
  if (nd == nullptr) {
    return std::nullopt;
  }
  const std::string usr = usr_for_decl(nd);
  if (usr.empty()) {
    return std::nullopt;
  }
  return ports_.lookup_symbol_id(
      usr, expansion_loc(context_, nd->getLocation()).file);
}

bool NamespaceUseVisitor::TraverseDecl(clang::Decl *decl) {
  if (decl == nullptr) {
    return true;
  }
  const std::optional<int64_t> scope_id = scope_symbol_id(decl);
  if (scope_id) {
    scope_stack_.push_back(*scope_id);
  }
  const bool result = RecursiveASTVisitor::TraverseDecl(decl);
  if (scope_id) {
    scope_stack_.pop_back();
  }
  return result;
}

void NamespaceUseVisitor::emit_ns_use(const clang::NamedDecl *ns_decl,
                                      clang::SourceLocation loc) {
  if (scope_stack_.empty()) {
    return; // no enclosing indexed symbol (-1 root)
  }
  const ExpansionLoc eloc = expansion_loc(context_, loc);
  if (eloc.file != target_file_) {
    return;
  }
  const std::string usr = usr_for_decl(ns_decl);
  if (usr.empty()) {
    return;
  }
  const auto ns_id = ports_.lookup_symbol_id(
      usr, expansion_loc(context_, ns_decl->getLocation()).file);
  if (!ns_id || *ns_id == scope_stack_.back()) {
    return;
  }
  if (!seen_.insert({scope_stack_.back(), *ns_id, eloc.line, eloc.col})
           .second) {
    return; // same NAMESPACE_REF site reached twice via RAV
  }
  EdgeRecord e;
  e.src_id = scope_stack_.back();
  e.dst_id = *ns_id;
  e.kind = 7; // uses
  const int64_t edge_id = ports_.add_edge(e);
  if (eloc.line != 0) {
    EdgeSiteRecord site;
    site.edge_id = edge_id;
    site.file_id = file_id_;
    site.line = eloc.line;
    site.col = eloc.col;
    site.conditional = 0;
  ports_.add_edge_site(site);
  }
}

bool NamespaceUseVisitor::TraverseNestedNameSpecifierLoc(
    clang::NestedNameSpecifierLoc nns) {
  // Walk namespace qualifier levels. LLVM 22 chains through
  // NamespaceAndPrefixLoc off a value NNS; LLVM 21 chains through getPrefix()
  // off a pointer NNS with getAsNamespace()/getAsNamespaceAlias().
  for (clang::NestedNameSpecifierLoc level = nns; level;) {
#if LLVM_VERSION_MAJOR >= 22
    if (level.getNestedNameSpecifier().getKind() !=
        clang::NestedNameSpecifier::Kind::Namespace) {
      break;
    }
    const clang::NamespaceAndPrefixLoc np = level.getAsNamespaceAndPrefix();
    if (np.Namespace != nullptr) {
      if (const auto *alias =
              llvm::dyn_cast<clang::NamespaceAliasDecl>(np.Namespace)) {
        emit_ns_use(alias->getNamespace(), level.getLocalBeginLoc());
      } else if (const auto *ns =
                     llvm::dyn_cast<clang::NamespaceDecl>(np.Namespace)) {
        emit_ns_use(ns, level.getLocalBeginLoc());
      }
    }
    level = np.Prefix;
#else
    const clang::NestedNameSpecifier *spec = level.getNestedNameSpecifier();
    if (spec == nullptr)
      break;
    if (const clang::NamespaceDecl *ns = spec->getAsNamespace())
      emit_ns_use(ns, level.getLocalBeginLoc());
    else if (const clang::NamespaceAliasDecl *alias =
                 spec->getAsNamespaceAlias())
      emit_ns_use(alias->getNamespace(), level.getLocalBeginLoc());
    level = level.getPrefix();
#endif
  }
  return RecursiveASTVisitor::TraverseNestedNameSpecifierLoc(nns);
}

#if LLVM_VERSION_MAJOR >= 22
bool NamespaceUseVisitor::VisitTypeLoc(clang::TypeLoc tl) {
  // LLVM 22 folds elaboration into tag/typedef TypeLocs, so their qualifier
  // never reaches TraverseNestedNameSpecifierLoc — visit it explicitly. On
  // LLVM 21 the qualifier arrives via ElaboratedTypeLoc through the normal
  // RAV traversal, so this shim is unnecessary (and TagTypeLoc has no
  // getQualifierLoc).
  clang::NestedNameSpecifierLoc qual;
  if (auto tt = tl.getAs<clang::TagTypeLoc>()) {
    qual = tt.getQualifierLoc();
  } else if (auto td = tl.getAs<clang::TypedefTypeLoc>()) {
    qual = td.getQualifierLoc();
  }
  if (qual) {
    TraverseNestedNameSpecifierLoc(qual);
  }
  return true;
}
#else
bool NamespaceUseVisitor::VisitTypeLoc(clang::TypeLoc /*tl*/) { return true; }
#endif

bool NamespaceUseVisitor::VisitUsingDirectiveDecl(
    clang::UsingDirectiveDecl *decl) {
  if (in_target_file(decl)) {
    emit_ns_use(decl->getNominatedNamespaceAsWritten(),
                decl->getIdentLocation());
  }
  return true;
}

} // namespace cidx::ast
