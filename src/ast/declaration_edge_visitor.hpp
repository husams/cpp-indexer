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

#include "ast/instance_minter.hpp"
#include "ast/mint_builder.hpp"
#include "ast/template_argument_encoder.hpp"

#include "clang/AST/RecursiveASTVisitor.h"

#include <optional>
#include <string>
#include <vector>

namespace clang {
class ASTContext;
class CXXBaseSpecifier;
class Expr;
class SourceManager;
class SourceRange;
class TypeSourceInfo;
class VarDecl;
class TypedefNameDecl;
} // namespace clang

namespace cidx::ast {

class EdgeSink;

class DeclarationEdgeVisitor : public clang::RecursiveASTVisitor<DeclarationEdgeVisitor> {
public:
  DeclarationEdgeVisitor(clang::ASTContext &context, EdgeSink &sink,
              std::string target_file, int64_t file_id);

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
  bool VisitVarDecl(clang::VarDecl *decl);           // type uses + static defs
  bool VisitTypedefNameDecl(clang::TypedefNameDecl *decl); // alias uses/mint

private:
  // The decl-level walk prunes at function bodies and only covers cursors of
  // the target file (for_file_cursors_p).
  bool in_walk(const clang::Decl *decl) const;

  void emit_base_specifier(const clang::NamedDecl *derived,
                           const std::string &derived_usr,
                           const clang::CXXBaseSpecifier &base);
  int64_t inherits_src_id(const clang::NamedDecl *derived,
                          const std::string &derived_usr);
  void emit_crtp_instantiates(const clang::CXXRecordDecl *base_rec,
                              const std::string &base_usr, int64_t dst_id);
  void emit_static_member_definition(const clang::VarDecl *decl,
                                     int64_t symbol_id);
  std::optional<std::string> static_var_init_text(clang::SourceRange range) const;
  void emit_static_init_def_edges(int64_t def_id, const clang::Expr *init);
  std::vector<const clang::NamedDecl *>
  friend_targets(const clang::TypeSourceInfo *tsi) const;
  void emit_template_params(const clang::TemplateDecl *tmpl,
                            int64_t owner_id);
  void emit_signature_uses(const clang::FunctionDecl *fn);

  clang::ASTContext &context_;
  clang::SourceManager &source_manager_;
  EdgeSink &sink_;
  MintBuilder mint_;
  TemplateArgumentEncoder targ_encoder_;
  InstanceMinter minter_;
  std::string target_file_;
  int64_t file_id_;
};

} // namespace cidx::ast
