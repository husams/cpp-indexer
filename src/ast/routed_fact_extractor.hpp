// Single rooted declaration traversal with direct fact-emitter dispatch.
#pragma once

#include "clang/AST/NestedNameSpecifier.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/TypeLoc.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace clang {
class ASTContext;
class ClassTemplateDecl;
class ClassTemplatePartialSpecializationDecl;
class ClassTemplateSpecializationDecl;
class CXXMethodDecl;
class CXXRecordDecl;
class Decl;
class FieldDecl;
class FriendDecl;
class FunctionDecl;
class FunctionTemplateDecl;
class NamedDecl;
class TypedefNameDecl;
class UsingDirectiveDecl;
class VarDecl;
class SourceManager;
} // namespace clang

namespace cidx::ast {

class DeclarationEdgeVisitor;
class FunctionDefinitionVisitor;
class NamespaceUseVisitor;
class SymbolVisitor;

class RoutedFactExtractor final
    : public clang::RecursiveASTVisitor<RoutedFactExtractor> {
public:
  using FileRouter =
      std::function<std::optional<std::int64_t>(const std::string &)>;

  RoutedFactExtractor(clang::ASTContext &context, FileRouter router,
                      SymbolVisitor *symbols,
                      DeclarationEdgeVisitor *declarations,
                      FunctionDefinitionVisitor *definitions,
                      NamespaceUseVisitor *namespaces);

  bool TraverseDecl(clang::Decl *decl);
  bool TraverseNestedNameSpecifierLoc(clang::NestedNameSpecifierLoc nns);
  bool VisitDecl(clang::Decl *decl);
  bool VisitNamedDecl(clang::NamedDecl *decl);
  bool VisitCXXRecordDecl(clang::CXXRecordDecl *decl);
  bool VisitFieldDecl(clang::FieldDecl *decl);
  bool VisitCXXMethodDecl(clang::CXXMethodDecl *decl);
  bool VisitFriendDecl(clang::FriendDecl *decl);
  bool VisitClassTemplateDecl(clang::ClassTemplateDecl *decl);
  bool VisitFunctionTemplateDecl(clang::FunctionTemplateDecl *decl);
  bool VisitClassTemplateSpecializationDecl(
      clang::ClassTemplateSpecializationDecl *decl);
  bool VisitClassTemplatePartialSpecializationDecl(
      clang::ClassTemplatePartialSpecializationDecl *decl);
  bool VisitFunctionDecl(clang::FunctionDecl *decl);
  bool VisitVarDecl(clang::VarDecl *decl);
  bool VisitTypedefNameDecl(clang::TypedefNameDecl *decl);
  bool VisitUsingDirectiveDecl(clang::UsingDirectiveDecl *decl);
  bool VisitTypeLoc(clang::TypeLoc type_loc);

private:
  struct Routing {
    std::optional<std::int64_t> file_id;
    bool owns_instantiation = false;
    bool retained = false;
    std::optional<std::int64_t> scope_id;
  };

  auto route(clang::SourceLocation location) -> std::optional<std::int64_t>;
  auto owns_routed_instantiation(clang::Decl *decl) -> bool;
  auto routing_for(clang::Decl *decl) -> Routing;
  [[nodiscard]] auto current_decl_is_retained() const -> bool;
  void begin_decl(clang::Decl *decl);
  void end_decl();

  clang::ASTContext &context_;
  clang::SourceManager &source_manager_;
  FileRouter router_;
  SymbolVisitor *symbols_ = nullptr;
  DeclarationEdgeVisitor *declarations_ = nullptr;
  FunctionDefinitionVisitor *definitions_ = nullptr;
  NamespaceUseVisitor *namespaces_ = nullptr;
  std::vector<Routing> routings_;
  std::unordered_map<unsigned, std::optional<std::int64_t>> routed_files_;
};

} // namespace cidx::ast
