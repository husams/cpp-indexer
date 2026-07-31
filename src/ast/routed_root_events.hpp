#pragma once

#include "clang/AST/RecursiveASTVisitor.h"

#include <cstddef>
#include <cstdint>
#include <optional>
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
class NestedNameSpecifierLoc;
class TypeLoc;
class TypedefNameDecl;
class UsingDirectiveDecl;
class VarDecl;
} // namespace clang

namespace cidx::ast {

class DeclarationEdgeVisitor;
class FunctionDefinitionVisitor;
class NamespaceUseVisitor;
struct PassMetrics;

// A translation-unit-local, AST-backed event stream. It owns no persisted
// facts or database identifiers and must be replayed before the ASTContext is
// destroyed.
class RoutedRootEventBuffer
    : public clang::RecursiveASTVisitor<RoutedRootEventBuffer> {
public:
  enum class Kind : std::uint8_t {
    enter_decl,
    leave_decl,
    visit_decl,
    visit_named_decl,
    visit_cxx_record_decl,
    visit_field_decl,
    visit_cxx_method_decl,
    visit_friend_decl,
    visit_class_template_decl,
    visit_function_template_decl,
    visit_class_template_specialization_decl,
    visit_class_template_partial_specialization_decl,
    visit_function_decl,
    visit_var_decl,
    visit_typedef_name_decl,
    visit_nested_name_specifier,
    visit_using_directive_decl,
    visit_type_loc,
  };

  struct Event {
    Kind kind;
    clang::Decl *decl = nullptr;
    std::optional<clang::NestedNameSpecifierLoc> nested_name;
    std::optional<clang::TypeLoc> type_loc;
  };

  explicit RoutedRootEventBuffer(std::size_t max_events,
                                 PassMetrics *metrics = nullptr);

  bool collect(clang::Decl *root);

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

  void replay_declarations(DeclarationEdgeVisitor &visitor) const;
  void replay_definitions(FunctionDefinitionVisitor &visitor) const;
  void replay_namespaces(NamespaceUseVisitor &visitor) const;

  [[nodiscard]] auto size() const -> std::size_t { return events_.size(); }
  [[nodiscard]] auto declaration_count() const -> std::size_t {
    return declaration_count_;
  }

private:
  void record(Event event);

  std::size_t max_events_;
  PassMetrics *metrics_;
  std::size_t declaration_count_ = 0;
  std::vector<Event> events_;
};

} // namespace cidx::ast
