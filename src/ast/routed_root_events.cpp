#include "ast/routed_root_events.hpp"

#include "ast/declaration_edge_visitor.hpp"
#include "ast/function_definition_visitor.hpp"
#include "ast/namespace_use_visitor.hpp"
#include "ast/pass_registry.hpp"

#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/NestedNameSpecifier.h"
#include "clang/AST/TypeLoc.h"

namespace cidx::ast {

RoutedRootEventBuffer::RoutedRootEventBuffer(std::size_t max_events,
                                             PassMetrics *metrics)
    : max_events_(max_events), metrics_(metrics) {
  events_.reserve(1024);
}

bool RoutedRootEventBuffer::collect(clang::Decl *root) {
  events_.clear();
  declaration_count_ = 0;
  if (root == nullptr) {
    return true;
  }
  return TraverseDecl(root);
}

void RoutedRootEventBuffer::record(Event event) {
  if (events_.size() >= max_events_) {
    throw PassBudgetExceeded("graph.headers", "routed_root_events");
  }
  events_.push_back(event);
}

bool RoutedRootEventBuffer::TraverseDecl(clang::Decl *decl) {
  if (decl == nullptr) {
    return true;
  }
  if (metrics_ != nullptr) {
    metrics_->note_visited();
  }
  ++declaration_count_;
  record({.kind = Kind::enter_decl, .decl = decl});
  const bool result = RecursiveASTVisitor::TraverseDecl(decl);
  record({.kind = Kind::leave_decl, .decl = decl});
  return result;
}

bool RoutedRootEventBuffer::TraverseNestedNameSpecifierLoc(
    clang::NestedNameSpecifierLoc nns) {
  record({.kind = Kind::visit_nested_name_specifier, .nested_name = nns});
  return RecursiveASTVisitor::TraverseNestedNameSpecifierLoc(nns);
}

bool RoutedRootEventBuffer::VisitDecl(clang::Decl *decl) {
  record({.kind = Kind::visit_decl, .decl = decl});
  return true;
}

bool RoutedRootEventBuffer::VisitNamedDecl(clang::NamedDecl *decl) {
  record({.kind = Kind::visit_named_decl, .decl = decl});
  return true;
}

bool RoutedRootEventBuffer::VisitCXXRecordDecl(clang::CXXRecordDecl *decl) {
  record({.kind = Kind::visit_cxx_record_decl, .decl = decl});
  return true;
}

bool RoutedRootEventBuffer::VisitFieldDecl(clang::FieldDecl *decl) {
  record({.kind = Kind::visit_field_decl, .decl = decl});
  return true;
}

bool RoutedRootEventBuffer::VisitCXXMethodDecl(clang::CXXMethodDecl *decl) {
  record({.kind = Kind::visit_cxx_method_decl, .decl = decl});
  return true;
}

bool RoutedRootEventBuffer::VisitFriendDecl(clang::FriendDecl *decl) {
  record({.kind = Kind::visit_friend_decl, .decl = decl});
  return true;
}

bool RoutedRootEventBuffer::VisitClassTemplateDecl(
    clang::ClassTemplateDecl *decl) {
  record({.kind = Kind::visit_class_template_decl, .decl = decl});
  return true;
}

bool RoutedRootEventBuffer::VisitFunctionTemplateDecl(
    clang::FunctionTemplateDecl *decl) {
  record({.kind = Kind::visit_function_template_decl, .decl = decl});
  return true;
}

bool RoutedRootEventBuffer::VisitClassTemplateSpecializationDecl(
    clang::ClassTemplateSpecializationDecl *decl) {
  record(
      {.kind = Kind::visit_class_template_specialization_decl, .decl = decl});
  return true;
}

bool RoutedRootEventBuffer::VisitClassTemplatePartialSpecializationDecl(
    clang::ClassTemplatePartialSpecializationDecl *decl) {
  record({.kind = Kind::visit_class_template_partial_specialization_decl,
          .decl = decl});
  return true;
}

bool RoutedRootEventBuffer::VisitFunctionDecl(clang::FunctionDecl *decl) {
  record({.kind = Kind::visit_function_decl, .decl = decl});
  return true;
}

bool RoutedRootEventBuffer::VisitVarDecl(clang::VarDecl *decl) {
  record({.kind = Kind::visit_var_decl, .decl = decl});
  return true;
}

bool RoutedRootEventBuffer::VisitTypedefNameDecl(clang::TypedefNameDecl *decl) {
  record({.kind = Kind::visit_typedef_name_decl, .decl = decl});
  return true;
}

bool RoutedRootEventBuffer::VisitUsingDirectiveDecl(
    clang::UsingDirectiveDecl *decl) {
  record({.kind = Kind::visit_using_directive_decl, .decl = decl});
  return true;
}

bool RoutedRootEventBuffer::VisitTypeLoc(clang::TypeLoc type_loc) {
  record({.kind = Kind::visit_type_loc, .type_loc = type_loc});
  return true;
}

void RoutedRootEventBuffer::replay_declarations(
    DeclarationEdgeVisitor &visitor) const {
  for (const Event &event : events_) {
    switch (event.kind) {
    case Kind::visit_decl:
      visitor.VisitDecl(event.decl);
      break;
    case Kind::visit_named_decl:
      visitor.VisitNamedDecl(static_cast<clang::NamedDecl *>(event.decl));
      break;
    case Kind::visit_cxx_record_decl:
      visitor.VisitCXXRecordDecl(
          static_cast<clang::CXXRecordDecl *>(event.decl));
      break;
    case Kind::visit_field_decl:
      visitor.VisitFieldDecl(static_cast<clang::FieldDecl *>(event.decl));
      break;
    case Kind::visit_cxx_method_decl:
      visitor.VisitCXXMethodDecl(
          static_cast<clang::CXXMethodDecl *>(event.decl));
      break;
    case Kind::visit_friend_decl:
      visitor.VisitFriendDecl(static_cast<clang::FriendDecl *>(event.decl));
      break;
    case Kind::visit_class_template_decl:
      visitor.VisitClassTemplateDecl(
          static_cast<clang::ClassTemplateDecl *>(event.decl));
      break;
    case Kind::visit_function_template_decl:
      visitor.VisitFunctionTemplateDecl(
          static_cast<clang::FunctionTemplateDecl *>(event.decl));
      break;
    case Kind::visit_class_template_specialization_decl:
      visitor.VisitClassTemplateSpecializationDecl(
          static_cast<clang::ClassTemplateSpecializationDecl *>(event.decl));
      break;
    case Kind::visit_class_template_partial_specialization_decl:
      visitor.VisitClassTemplatePartialSpecializationDecl(
          static_cast<clang::ClassTemplatePartialSpecializationDecl *>(
              event.decl));
      break;
    case Kind::visit_function_decl:
      visitor.VisitFunctionDecl(static_cast<clang::FunctionDecl *>(event.decl));
      break;
    case Kind::visit_var_decl:
      visitor.VisitVarDecl(static_cast<clang::VarDecl *>(event.decl));
      break;
    case Kind::visit_typedef_name_decl:
      visitor.VisitTypedefNameDecl(
          static_cast<clang::TypedefNameDecl *>(event.decl));
      break;
    default:
      break;
    }
  }
}

void RoutedRootEventBuffer::replay_definitions(
    FunctionDefinitionVisitor &visitor) const {
  for (const Event &event : events_) {
    if (event.kind == Kind::visit_decl) {
      visitor.VisitDecl(event.decl);
    } else if (event.kind == Kind::visit_function_decl) {
      visitor.VisitFunctionDecl(static_cast<clang::FunctionDecl *>(event.decl));
    }
  }
}

void RoutedRootEventBuffer::replay_namespaces(
    NamespaceUseVisitor &visitor) const {
  std::vector<std::optional<int64_t>> scopes;
  scopes.reserve(declaration_count_);
  for (const Event &event : events_) {
    switch (event.kind) {
    case Kind::enter_decl:
      scopes.push_back(visitor.begin_decl(event.decl));
      break;
    case Kind::leave_decl:
      if (!scopes.empty()) {
        visitor.end_decl(scopes.back());
        scopes.pop_back();
      }
      break;
    case Kind::visit_decl:
      visitor.VisitDecl(event.decl);
      break;
    case Kind::visit_nested_name_specifier:
      if (event.nested_name) {
        visitor.TraverseNestedNameSpecifierLoc(*event.nested_name);
      }
      break;
    case Kind::visit_using_directive_decl:
      visitor.VisitUsingDirectiveDecl(
          static_cast<clang::UsingDirectiveDecl *>(event.decl));
      break;
    case Kind::visit_type_loc:
      if (event.type_loc) {
        visitor.VisitTypeLoc(*event.type_loc);
      }
      break;
    default:
      break;
    }
  }
}

} // namespace cidx::ast
