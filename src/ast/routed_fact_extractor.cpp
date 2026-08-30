#include "ast/routed_fact_extractor.hpp"

#include "ast/declaration_edge_visitor.hpp"
#include "ast/function_definition_visitor.hpp"
#include "ast/instantiation_edges.hpp"
#include "ast/location.hpp"
#include "ast/namespace_use_visitor.hpp"
#include "ast/symbol_visitor.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/Basic/SourceManager.h"

#include <llvm/Support/Casting.h>

#include <utility>

namespace cidx::ast {

RoutedFactExtractor::RoutedFactExtractor(clang::ASTContext &context,
                                         FileRouter router,
                                         SymbolVisitor *symbols,
                                         DeclarationEdgeVisitor *declarations,
                                         FunctionDefinitionVisitor *definitions,
                                         NamespaceUseVisitor *namespaces)
    : context_(context), source_manager_(context.getSourceManager()),
      router_(std::move(router)), symbols_(symbols),
      declarations_(declarations), definitions_(definitions),
      namespaces_(namespaces) {}

bool RoutedFactExtractor::TraverseDecl(clang::Decl *decl) {
  if (decl == nullptr) {
    return true;
  }
  const Routing routing = routing_for(decl);
  routings_.push_back(routing);
  begin_decl(decl);
  try {
    const bool result = RecursiveASTVisitor::TraverseDecl(decl);
    end_decl();
    routings_.pop_back();
    return result;
  } catch (...) {
    end_decl();
    routings_.pop_back();
    throw;
  }
}

bool RoutedFactExtractor::TraverseNestedNameSpecifierLoc(
    clang::NestedNameSpecifierLoc nns) {
  if (nns && namespaces_ != nullptr && route(nns.getLocalBeginLoc())) {
    namespaces_->visit_nested_name_specifier(nns);
  }
  return RecursiveASTVisitor::TraverseNestedNameSpecifierLoc(nns);
}

bool RoutedFactExtractor::VisitDecl(clang::Decl *decl) {
  if (!current_decl_is_retained()) {
    return true;
  }
  if (symbols_ != nullptr) {
    symbols_->VisitDecl(decl);
  }
  if (declarations_ != nullptr) {
    declarations_->VisitDecl(decl);
  }
  if (definitions_ != nullptr) {
    definitions_->VisitDecl(decl);
  }
  if (namespaces_ != nullptr) {
    namespaces_->VisitDecl(decl);
  }
  return true;
}

bool RoutedFactExtractor::VisitNamedDecl(clang::NamedDecl *decl) {
  if (!current_decl_is_retained()) {
    return true;
  }
  if (symbols_ != nullptr) {
    symbols_->VisitNamedDecl(decl);
  }
  if (declarations_ != nullptr) {
    declarations_->VisitNamedDecl(decl);
  }
  return true;
}

bool RoutedFactExtractor::VisitCXXRecordDecl(clang::CXXRecordDecl *decl) {
  if (current_decl_is_retained() && declarations_ != nullptr) {
    declarations_->VisitCXXRecordDecl(decl);
  }
  return true;
}

bool RoutedFactExtractor::VisitFieldDecl(clang::FieldDecl *decl) {
  if (current_decl_is_retained() && declarations_ != nullptr) {
    declarations_->VisitFieldDecl(decl);
  }
  return true;
}

bool RoutedFactExtractor::VisitCXXMethodDecl(clang::CXXMethodDecl *decl) {
  if (current_decl_is_retained() && declarations_ != nullptr) {
    declarations_->VisitCXXMethodDecl(decl);
  }
  return true;
}

bool RoutedFactExtractor::VisitFriendDecl(clang::FriendDecl *decl) {
  if (current_decl_is_retained() && declarations_ != nullptr) {
    declarations_->VisitFriendDecl(decl);
  }
  return true;
}

bool RoutedFactExtractor::VisitClassTemplateDecl(
    clang::ClassTemplateDecl *decl) {
  if (!current_decl_is_retained()) {
    return true;
  }
  if (symbols_ != nullptr) {
    symbols_->VisitClassTemplateDecl(decl);
  }
  if (declarations_ != nullptr) {
    declarations_->VisitClassTemplateDecl(decl);
  }
  return true;
}

bool RoutedFactExtractor::VisitFunctionTemplateDecl(
    clang::FunctionTemplateDecl *decl) {
  if (!current_decl_is_retained()) {
    return true;
  }
  if (symbols_ != nullptr) {
    symbols_->VisitFunctionTemplateDecl(decl);
  }
  if (declarations_ != nullptr) {
    declarations_->VisitFunctionTemplateDecl(decl);
  }
  return true;
}

bool RoutedFactExtractor::VisitClassTemplateSpecializationDecl(
    clang::ClassTemplateSpecializationDecl *decl) {
  if (current_decl_is_retained() && declarations_ != nullptr) {
    declarations_->VisitClassTemplateSpecializationDecl(decl);
  }
  return true;
}

bool RoutedFactExtractor::VisitClassTemplatePartialSpecializationDecl(
    clang::ClassTemplatePartialSpecializationDecl *decl) {
  if (current_decl_is_retained() && declarations_ != nullptr) {
    declarations_->VisitClassTemplatePartialSpecializationDecl(decl);
  }
  return true;
}

bool RoutedFactExtractor::VisitFunctionDecl(clang::FunctionDecl *decl) {
  if (current_decl_is_retained()) {
    if (declarations_ != nullptr) {
      declarations_->VisitFunctionDecl(decl);
    }
    if (definitions_ != nullptr) {
      definitions_->VisitFunctionDecl(decl);
    }
  }
  return true;
}

bool RoutedFactExtractor::VisitVarDecl(clang::VarDecl *decl) {
  if (current_decl_is_retained() && declarations_ != nullptr) {
    declarations_->VisitVarDecl(decl);
  }
  return true;
}

bool RoutedFactExtractor::VisitTypedefNameDecl(clang::TypedefNameDecl *decl) {
  if (current_decl_is_retained() && declarations_ != nullptr) {
    declarations_->VisitTypedefNameDecl(decl);
  }
  return true;
}

bool RoutedFactExtractor::VisitUsingDirectiveDecl(
    clang::UsingDirectiveDecl *decl) {
  if (current_decl_is_retained() && namespaces_ != nullptr) {
    namespaces_->VisitUsingDirectiveDecl(decl);
  }
  return true;
}

bool RoutedFactExtractor::VisitTypeLoc(clang::TypeLoc type_loc) {
  if (namespaces_ != nullptr && route(type_loc.getBeginLoc())) {
    namespaces_->VisitTypeLoc(type_loc);
  }
  return true;
}

auto RoutedFactExtractor::route(clang::SourceLocation location)
    -> std::optional<std::int64_t> {
  if (location.isInvalid()) {
    return std::nullopt;
  }
  const clang::SourceLocation expansion =
      source_manager_.getExpansionLoc(location);
  if (expansion.isInvalid() || source_manager_.isInSystemHeader(expansion)) {
    return std::nullopt;
  }
  const unsigned file = source_manager_.getFileID(expansion).getHashValue();
  if (const auto cached = routed_files_.find(file);
      cached != routed_files_.end()) {
    return cached->second;
  }
  const std::string path = expansion_loc(context_, location).file;
  const std::optional<std::int64_t> routed =
      path.empty() ? std::nullopt : router_(path);
  routed_files_.emplace(file, routed);
  return routed;
}

auto RoutedFactExtractor::owns_routed_instantiation(clang::Decl *decl) -> bool {
  bool owns = false;
  const auto probe = [this, &owns](const clang::FunctionDecl *instantiation) {
    if (!owns) {
      owns = route(instantiation->getPointOfInstantiation()).has_value();
    }
  };
  if (const auto *function =
          llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
    for_each_explicit_callable_instantiation(function, probe);
  } else if (const auto *record =
                 llvm::dyn_cast<clang::ClassTemplateDecl>(decl)) {
    for_each_explicit_callable_instantiation(record, probe);
  }
  return owns;
}

auto RoutedFactExtractor::routing_for(clang::Decl *decl) -> Routing {
  Routing routing;
  routing.file_id = route(decl->getLocation());
  routing.owns_instantiation =
      !routing.file_id.has_value() && owns_routed_instantiation(decl);
  routing.retained = routing.file_id.has_value() || routing.owns_instantiation;
  return routing;
}

auto RoutedFactExtractor::current_decl_is_retained() const -> bool {
  return !routings_.empty() && routings_.back().retained;
}

void RoutedFactExtractor::begin_decl(clang::Decl *decl) {
  if (current_decl_is_retained() && namespaces_ != nullptr) {
    routings_.back().scope_id = namespaces_->begin_decl(decl);
  }
}

void RoutedFactExtractor::end_decl() {
  if (current_decl_is_retained() && namespaces_ != nullptr) {
    namespaces_->end_decl(routings_.back().scope_id);
  }
}

} // namespace cidx::ast
