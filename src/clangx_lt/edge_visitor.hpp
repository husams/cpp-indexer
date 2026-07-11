// Declaration-level edge visitor.
//
// RecursiveASTVisitor reproducing the libclang edge pass's DECLARATION walk
// (ast_edges.cpp index_edges_notxn B1) for one target file:
//
//   contains(3)    lexical namespace -> any indexed child;
//                  record/class-template -> nested type
//   inherits(2)    derived -> base (+ access, virtual), from base specifiers
//   instantiates(5) CRTP: base specialization instance -> primary template
//   field_of(8)    field -> owning record
//   method_of(9)   method/ctor/dtor -> owning record (incl. member fn-templates)
//   overrides(6)   method -> each directly overridden method (minted)
//   friend(17)     record -> befriended record (lookup-only)
//   specializes(4)/instantiates(5) + template_arg rows for class-template
//                  full specializations / explicit instantiations
//   template_param rows for class/function templates
//
// DEFERRED to the body/uses phase: signature-level uses(7) (emit_type_use),
// template-instance minting, static-member init definitions, body descent
// (calls/uses/constructs), namespace-uses.
//
// Emission goes through EdgeSink only; no storage or I/O here.
#pragma once

#include "clangx_lt/mint_builder.hpp"
#include "clangx_lt/template_arg_resolver.hpp"

#include "clang/AST/RecursiveASTVisitor.h"

#include <string>

namespace clang {
class ASTContext;
class SourceManager;
} // namespace clang

namespace cidx::lt {

class EdgeSink;

class EdgeVisitor : public clang::RecursiveASTVisitor<EdgeVisitor> {
public:
  EdgeVisitor(clang::ASTContext &context, EdgeSink &sink,
              std::string target_file);

  bool VisitNamedDecl(clang::NamedDecl *decl);           // contains
  bool TraverseTypedefDecl(clang::TypedefDecl *decl);    // libclang dup quirk
  bool VisitCXXRecordDecl(clang::CXXRecordDecl *decl);   // inherits (+CRTP)
  bool VisitFieldDecl(clang::FieldDecl *decl);           // field_of
  bool VisitCXXMethodDecl(clang::CXXMethodDecl *decl);   // method_of+overrides
  bool VisitFriendDecl(clang::FriendDecl *decl);         // friend
  bool VisitClassTemplateDecl(clang::ClassTemplateDecl *decl);
  bool VisitFunctionTemplateDecl(clang::FunctionTemplateDecl *decl);
  bool VisitClassTemplateSpecializationDecl(
      clang::ClassTemplateSpecializationDecl *decl); // specializes/instantiates

private:
  // The decl-level walk prunes at function bodies and only covers cursors of
  // the target file (for_file_cursors_p).
  bool in_walk(const clang::Decl *decl) const;

  void emit_template_params(const clang::TemplateDecl *tmpl,
                            int64_t owner_id);

  clang::ASTContext &context_;
  clang::SourceManager &source_manager_;
  EdgeSink &sink_;
  MintBuilder mint_;
  TemplateArgResolver arg_resolver_;
  std::string target_file_;
};

} // namespace cidx::lt
