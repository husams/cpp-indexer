// Bounded, routed root-event contract for the whole-TU declaration visitors.
//
// The four routed root passes (symbols, declaration edges, function
// definitions, namespace uses) each run their own RecursiveASTVisitor over the
// translation-unit root. Every one of those walks re-derives the same
// information: which decl the traversal is on, in which routed file it lives,
// and how namespace scopes nest around it.
//
// RoutedRootEventBuffer records that shared information ONCE, in deterministic
// RecursiveASTVisitor encounter order, so the same non-recursive handlers can
// be driven either by their standalone RAV adapter or by replaying this buffer.
// It is a pure in-memory relay:
//
//   - it is valid only while the recording ASTContext lives (events hold bare
//     clang::Decl/TypeLoc/NestedNameSpecifierLoc handles);
//   - it owns no database identifiers and no serialized facts — the routed
//     file id it carries is the caller's own routing answer, resolved once;
//   - it is bounded by the declared pass budget and fails with a named
//     PassBudgetExceeded diagnostic instead of growing without limit.
//
// Routing exclusion matches S-071 exactly: an event is retained only when the
// FileRouter resolves its own expansion file, so system headers, unowned
// files, and already-indexed headers never enter the buffer. The one exception
// is a class/function template declaration whose explicit callable
// instantiations are anchored at a routed point of instantiation: ownership
// there follows the instantiation statement, not the template's file, so such
// a declaration is retained even when the template itself is unrouted.
//
// This header defines the contract only. Registering a fused pass that
// consumes it is deliberately out of scope: the production plan still runs
// four whole-TU root traversals.
#pragma once

#include "clang/AST/NestedNameSpecifier.h"
#include "clang/AST/TypeLoc.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace clang {
class ASTContext;
class Decl;
} // namespace clang

namespace cidx::ast {

class DeclarationEdgeVisitor;
class FunctionDefinitionVisitor;
class NamespaceUseVisitor;
class RoutedRootEventRecorder;
class SymbolVisitor;
struct PassMetrics;

// One recorded RecursiveASTVisitor callback. The kinds are exactly the union
// of the callbacks the four routed root visitors consume; the ordering of the
// enumerators carries no meaning, the recorded sequence does.
enum class RoutedRootEventKind : std::uint8_t {
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
  visit_using_directive_decl,
  visit_nested_name_specifier,
  visit_type_loc,
};

struct RoutedRootEvent {
  RoutedRootEventKind kind{};
  clang::Decl *decl = nullptr;
  std::optional<clang::NestedNameSpecifierLoc> nested_name;
  std::optional<clang::TypeLoc> type_loc;
  // The routed file this event's own expansion location belongs to, resolved
  // once when the event was recorded. It is empty only for a template
  // declaration retained solely because it owns a routed explicit
  // instantiation.
  std::optional<std::int64_t> routed_file_id;
  bool owns_routed_instantiation = false;
};

class RoutedRootEventBuffer {
public:
  // The same routing port the four routed root visitors already take from the
  // engine: a normalized path maps to its routed file id, or to nothing when
  // the file is a system header, unowned, or already indexed.
  using FileRouter =
      std::function<std::optional<std::int64_t>(const std::string &)>;

  // The budget dimension named in the PassBudgetExceeded diagnostic when the
  // event buffer itself would overflow.
  static constexpr std::string_view kEventBudgetDimension =
      "routed_root_events";

  // max_events bounds the buffer; pass_id names the owning pass in every
  // budget diagnostic. metrics, when given, receives one visited construct per
  // traversed declaration and therefore enforces the declared
  // visited-construct budget as well. Throws std::invalid_argument when the
  // router is empty, the pass id is empty, or the event bound is zero.
  RoutedRootEventBuffer(clang::ASTContext &context, FileRouter router,
                        std::size_t max_events, std::string pass_id,
                        PassMetrics *metrics = nullptr);

  // Records the whole subtree under root in RAV encounter order. Throws
  // PassBudgetExceeded when either budget dimension is exhausted; the buffer
  // is then left cleared so no partially recorded stream can be replayed.
  void collect(clang::Decl *root);

  void replay_symbols(SymbolVisitor &visitor) const;
  void replay_declarations(DeclarationEdgeVisitor &visitor) const;
  void replay_definitions(FunctionDefinitionVisitor &visitor) const;
  void replay_namespaces(NamespaceUseVisitor &visitor) const;

  [[nodiscard]] auto events() const -> const std::vector<RoutedRootEvent> & {
    return events_;
  }
  [[nodiscard]] auto size() const -> std::size_t { return events_.size(); }
  [[nodiscard]] auto declaration_count() const -> std::size_t {
    return declaration_count_;
  }
  // Declarations the router excluded, i.e. traversed but never recorded.
  [[nodiscard]] auto excluded_declaration_count() const -> std::size_t {
    return excluded_declaration_count_;
  }
  // The routed files the recorded events touch, ascending and deduplicated.
  [[nodiscard]] auto routed_file_ids() const -> std::vector<std::int64_t>;

private:
  friend class RoutedRootEventRecorder;

  void record(const RoutedRootEvent &event);
  void note_declaration(bool retained);

  clang::ASTContext &context_;
  FileRouter router_;
  std::size_t max_events_;
  std::string pass_id_;
  PassMetrics *metrics_;
  std::size_t declaration_count_ = 0;
  std::size_t excluded_declaration_count_ = 0;
  std::vector<RoutedRootEvent> events_;
};

} // namespace cidx::ast
