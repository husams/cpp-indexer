#include "ast/routed_root_events.hpp"

#include "ast/declaration_edge_visitor.hpp"
#include "ast/function_definition_visitor.hpp"
#include "ast/instantiation_edges.hpp"
#include "ast/location.hpp"
#include "ast/namespace_use_visitor.hpp"
#include "ast/pass_registry.hpp"
#include "ast/symbol_visitor.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace cidx::ast {

// The RecursiveASTVisitor adapter that fills a RoutedRootEventBuffer. It lives
// in the implementation so the contract header stays a plain data relay and
// the CRTP callback surface is not part of the public API.
class RoutedRootEventRecorder final
    : public clang::RecursiveASTVisitor<RoutedRootEventRecorder> {
public:
  RoutedRootEventRecorder(RoutedRootEventBuffer &buffer,
                          clang::ASTContext &context,
                          const RoutedRootEventBuffer::FileRouter &router)
      : buffer_(buffer), source_manager_(context.getSourceManager()),
        context_(context), router_(router) {}

  bool TraverseDecl(clang::Decl *decl) {
    if (decl == nullptr) {
      return true;
    }
    const Routing routing = routing_for(decl);
    buffer_.note_declaration(routing.retained);
    routings_.push_back(routing);
    if (routing.retained) {
      emit(RoutedRootEventKind::enter_decl, decl);
    }
    const bool result = RecursiveASTVisitor::TraverseDecl(decl);
    if (routing.retained) {
      emit(RoutedRootEventKind::leave_decl, decl);
    }
    routings_.pop_back();
    return result;
  }

  bool TraverseNestedNameSpecifierLoc(clang::NestedNameSpecifierLoc nns) {
    if (nns) {
      if (const std::optional<std::int64_t> routed =
              route(nns.getLocalBeginLoc())) {
        buffer_.record(
            {.kind = RoutedRootEventKind::visit_nested_name_specifier,
             .nested_name = nns,
             .routed_file_id = routed});
      }
    }
    return RecursiveASTVisitor::TraverseNestedNameSpecifierLoc(nns);
  }

  bool VisitTypeLoc(clang::TypeLoc type_loc) {
    if (const std::optional<std::int64_t> routed =
            route(type_loc.getBeginLoc())) {
      buffer_.record({.kind = RoutedRootEventKind::visit_type_loc,
                      .type_loc = type_loc,
                      .routed_file_id = routed});
    }
    return true;
  }

  bool VisitDecl(clang::Decl *decl) {
    emit(RoutedRootEventKind::visit_decl, decl);
    return true;
  }
  bool VisitNamedDecl(clang::NamedDecl *decl) {
    emit(RoutedRootEventKind::visit_named_decl, decl);
    return true;
  }
  bool VisitCXXRecordDecl(clang::CXXRecordDecl *decl) {
    emit(RoutedRootEventKind::visit_cxx_record_decl, decl);
    return true;
  }
  bool VisitFieldDecl(clang::FieldDecl *decl) {
    emit(RoutedRootEventKind::visit_field_decl, decl);
    return true;
  }
  bool VisitCXXMethodDecl(clang::CXXMethodDecl *decl) {
    emit(RoutedRootEventKind::visit_cxx_method_decl, decl);
    return true;
  }
  bool VisitFriendDecl(clang::FriendDecl *decl) {
    emit(RoutedRootEventKind::visit_friend_decl, decl);
    return true;
  }
  bool VisitClassTemplateDecl(clang::ClassTemplateDecl *decl) {
    emit(RoutedRootEventKind::visit_class_template_decl, decl);
    return true;
  }
  bool VisitFunctionTemplateDecl(clang::FunctionTemplateDecl *decl) {
    emit(RoutedRootEventKind::visit_function_template_decl, decl);
    return true;
  }
  bool VisitClassTemplateSpecializationDecl(
      clang::ClassTemplateSpecializationDecl *decl) {
    emit(RoutedRootEventKind::visit_class_template_specialization_decl, decl);
    return true;
  }
  bool VisitClassTemplatePartialSpecializationDecl(
      clang::ClassTemplatePartialSpecializationDecl *decl) {
    emit(RoutedRootEventKind::visit_class_template_partial_specialization_decl,
         decl);
    return true;
  }
  bool VisitFunctionDecl(clang::FunctionDecl *decl) {
    emit(RoutedRootEventKind::visit_function_decl, decl);
    return true;
  }
  bool VisitVarDecl(clang::VarDecl *decl) {
    emit(RoutedRootEventKind::visit_var_decl, decl);
    return true;
  }
  bool VisitTypedefNameDecl(clang::TypedefNameDecl *decl) {
    emit(RoutedRootEventKind::visit_typedef_name_decl, decl);
    return true;
  }
  bool VisitUsingDirectiveDecl(clang::UsingDirectiveDecl *decl) {
    emit(RoutedRootEventKind::visit_using_directive_decl, decl);
    return true;
  }

private:
  struct Routing {
    std::optional<std::int64_t> file_id;
    bool owns_instantiation = false;
    bool retained = false;
  };

  // The routed file of an expansion location, or nothing when the router
  // excludes it. System headers are rejected before the router so an excluded
  // path never costs a normalization.
  auto route(clang::SourceLocation loc) -> std::optional<std::int64_t> {
    if (loc.isInvalid()) {
      return std::nullopt;
    }
    const clang::SourceLocation expansion =
        source_manager_.getExpansionLoc(loc);
    if (expansion.isInvalid() || source_manager_.isInSystemHeader(expansion)) {
      return std::nullopt;
    }
    const clang::FileID file = source_manager_.getFileID(expansion);
    if (const auto cached = routed_files_.find(file.getHashValue());
        cached != routed_files_.end()) {
      return cached->second;
    }
    const std::string path = expansion_loc(context_, loc).file;
    const std::optional<std::int64_t> routed =
        path.empty() ? std::nullopt : router_(path);
    routed_files_.emplace(file.getHashValue(), routed);
    return routed;
  }

  // Explicit callable instantiations never appear as lexical decls; they are
  // owned by their point of instantiation, so a template declaration in an
  // unrouted file still has to be replayed when it carries one.
  auto owns_routed_instantiation(clang::Decl *decl) -> bool {
    bool owns = false;
    const auto probe = [this, &owns](const clang::FunctionDecl *instantiation) {
      if (owns) {
        return;
      }
      owns = route(instantiation->getPointOfInstantiation()).has_value();
    };
    if (const auto *fn = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
      for_each_explicit_callable_instantiation(fn, probe);
    } else if (const auto *rec =
                   llvm::dyn_cast<clang::ClassTemplateDecl>(decl)) {
      for_each_explicit_callable_instantiation(rec, probe);
    }
    return owns;
  }

  auto routing_for(clang::Decl *decl) -> Routing {
    Routing routing;
    routing.file_id = route(decl->getLocation());
    routing.owns_instantiation =
        !routing.file_id.has_value() && owns_routed_instantiation(decl);
    routing.retained =
        routing.file_id.has_value() || routing.owns_instantiation;
    return routing;
  }

  void emit(RoutedRootEventKind kind, clang::Decl *decl) {
    if (routings_.empty() || !routings_.back().retained) {
      return;
    }
    buffer_.record(
        {.kind = kind,
         .decl = decl,
         .routed_file_id = routings_.back().file_id,
         .owns_routed_instantiation = routings_.back().owns_instantiation});
  }

  RoutedRootEventBuffer &buffer_;
  clang::SourceManager &source_manager_;
  clang::ASTContext &context_;
  const RoutedRootEventBuffer::FileRouter &router_;
  std::vector<Routing> routings_;
  // A translation unit reaches the same file through very many declarations;
  // the routing answer depends only on the FileID, never on the position.
  std::unordered_map<unsigned, std::optional<std::int64_t>> routed_files_;
};

RoutedRootEventBuffer::RoutedRootEventBuffer(clang::ASTContext &context,
                                             FileRouter router,
                                             std::size_t max_events,
                                             std::string pass_id,
                                             PassMetrics *metrics)
    : context_(context), router_(std::move(router)), max_events_(max_events),
      pass_id_(std::move(pass_id)), metrics_(metrics) {
  if (!router_) {
    throw std::invalid_argument("routed root events require a file router");
  }
  if (pass_id_.empty()) {
    throw std::invalid_argument("routed root events require a pass id");
  }
  if (max_events_ == 0) {
    throw std::invalid_argument("routed root events require an event bound");
  }
}

void RoutedRootEventBuffer::record(const RoutedRootEvent &event) {
  if (events_.size() >= max_events_) {
    throw PassBudgetExceeded(pass_id_, std::string(kEventBudgetDimension));
  }
  events_.push_back(event);
}

void RoutedRootEventBuffer::note_declaration(bool retained) {
  ++declaration_count_;
  if (!retained) {
    ++excluded_declaration_count_;
  }
  if (metrics_ != nullptr) {
    metrics_->note_visited();
  }
}

void RoutedRootEventBuffer::collect(clang::Decl *root) {
  events_.clear();
  declaration_count_ = 0;
  excluded_declaration_count_ = 0;
  if (root == nullptr) {
    return;
  }
  RoutedRootEventRecorder recorder(*this, context_, router_);
  try {
    recorder.TraverseDecl(root);
  } catch (...) {
    // A partially recorded stream is not a replayable traversal; drop it so a
    // caller that swallows the diagnostic cannot publish half a translation
    // unit's facts.
    events_.clear();
    throw;
  }
}

auto RoutedRootEventBuffer::routed_file_ids() const
    -> std::vector<std::int64_t> {
  std::vector<std::int64_t> ids;
  for (const RoutedRootEvent &event : events_) {
    if (event.routed_file_id) {
      ids.push_back(*event.routed_file_id);
    }
  }
  std::ranges::sort(ids);
  const auto duplicates = std::ranges::unique(ids);
  ids.erase(duplicates.begin(), duplicates.end());
  return ids;
}

void RoutedRootEventBuffer::replay_symbols(SymbolVisitor &visitor) const {
  for (const RoutedRootEvent &event : events_) {
    switch (event.kind) {
    case RoutedRootEventKind::visit_decl:
      visitor.VisitDecl(event.decl);
      break;
    case RoutedRootEventKind::visit_named_decl:
      visitor.VisitNamedDecl(llvm::cast<clang::NamedDecl>(event.decl));
      break;
    case RoutedRootEventKind::visit_function_template_decl:
      visitor.VisitFunctionTemplateDecl(
          llvm::cast<clang::FunctionTemplateDecl>(event.decl));
      break;
    case RoutedRootEventKind::visit_class_template_decl:
      visitor.VisitClassTemplateDecl(
          llvm::cast<clang::ClassTemplateDecl>(event.decl));
      break;
    default:
      break;
    }
  }
}

void RoutedRootEventBuffer::replay_declarations(
    DeclarationEdgeVisitor &visitor) const {
  for (const RoutedRootEvent &event : events_) {
    switch (event.kind) {
    case RoutedRootEventKind::visit_decl:
      visitor.VisitDecl(event.decl);
      break;
    case RoutedRootEventKind::visit_named_decl:
      visitor.VisitNamedDecl(llvm::cast<clang::NamedDecl>(event.decl));
      break;
    case RoutedRootEventKind::visit_cxx_record_decl:
      visitor.VisitCXXRecordDecl(llvm::cast<clang::CXXRecordDecl>(event.decl));
      break;
    case RoutedRootEventKind::visit_field_decl:
      visitor.VisitFieldDecl(llvm::cast<clang::FieldDecl>(event.decl));
      break;
    case RoutedRootEventKind::visit_cxx_method_decl:
      visitor.VisitCXXMethodDecl(llvm::cast<clang::CXXMethodDecl>(event.decl));
      break;
    case RoutedRootEventKind::visit_friend_decl:
      visitor.VisitFriendDecl(llvm::cast<clang::FriendDecl>(event.decl));
      break;
    case RoutedRootEventKind::visit_class_template_decl:
      visitor.VisitClassTemplateDecl(
          llvm::cast<clang::ClassTemplateDecl>(event.decl));
      break;
    case RoutedRootEventKind::visit_function_template_decl:
      visitor.VisitFunctionTemplateDecl(
          llvm::cast<clang::FunctionTemplateDecl>(event.decl));
      break;
    case RoutedRootEventKind::visit_class_template_specialization_decl:
      visitor.VisitClassTemplateSpecializationDecl(
          llvm::cast<clang::ClassTemplateSpecializationDecl>(event.decl));
      break;
    case RoutedRootEventKind::visit_class_template_partial_specialization_decl:
      visitor.VisitClassTemplatePartialSpecializationDecl(
          llvm::cast<clang::ClassTemplatePartialSpecializationDecl>(
              event.decl));
      break;
    case RoutedRootEventKind::visit_function_decl:
      visitor.VisitFunctionDecl(llvm::cast<clang::FunctionDecl>(event.decl));
      break;
    case RoutedRootEventKind::visit_var_decl:
      visitor.VisitVarDecl(llvm::cast<clang::VarDecl>(event.decl));
      break;
    case RoutedRootEventKind::visit_typedef_name_decl:
      visitor.VisitTypedefNameDecl(
          llvm::cast<clang::TypedefNameDecl>(event.decl));
      break;
    default:
      break;
    }
  }
}

void RoutedRootEventBuffer::replay_definitions(
    FunctionDefinitionVisitor &visitor) const {
  for (const RoutedRootEvent &event : events_) {
    switch (event.kind) {
    case RoutedRootEventKind::visit_decl:
      visitor.VisitDecl(event.decl);
      break;
    case RoutedRootEventKind::visit_function_decl:
      visitor.VisitFunctionDecl(llvm::cast<clang::FunctionDecl>(event.decl));
      break;
    default:
      break;
    }
  }
}

void RoutedRootEventBuffer::replay_namespaces(
    NamespaceUseVisitor &visitor) const {
  std::vector<std::optional<std::int64_t>> scopes;
  for (const RoutedRootEvent &event : events_) {
    switch (event.kind) {
    case RoutedRootEventKind::enter_decl:
      scopes.push_back(visitor.begin_decl(event.decl));
      break;
    case RoutedRootEventKind::leave_decl:
      if (!scopes.empty()) {
        visitor.end_decl(scopes.back());
        scopes.pop_back();
      }
      break;
    case RoutedRootEventKind::visit_decl:
      visitor.VisitDecl(event.decl);
      break;
    case RoutedRootEventKind::visit_nested_name_specifier:
      // The non-recursive handler: the recorder already recorded whatever the
      // qualifier's own subtree reached, so re-traversing it here would double
      // the stream.
      if (event.nested_name) {
        visitor.visit_nested_name_specifier(*event.nested_name);
      }
      break;
    case RoutedRootEventKind::visit_using_directive_decl:
      visitor.VisitUsingDirectiveDecl(
          llvm::cast<clang::UsingDirectiveDecl>(event.decl));
      break;
    case RoutedRootEventKind::visit_type_loc:
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
