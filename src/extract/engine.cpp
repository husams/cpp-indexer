#include "extract/engine.hpp"

#include "extract/matcher_catalog.hpp"
#include "extract/plan_identity.hpp"
#include "extract/validator.hpp"

#include "ast/location.hpp"
#include "ast/usr.hpp"
#include "util/hashing.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/Dynamic/Diagnostics.h"
#include "clang/ASTMatchers/Dynamic/Parser.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Specifiers.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

namespace cidx::extract {

PlanNotValidated::PlanNotValidated(const std::string &message)
    : std::runtime_error(message) {}

const RuleExecutionStats *
ExecutionReport::find(const std::string &rule_id) const {
  for (const auto &stats : rule_stats) {
    if (stats.rule_id == rule_id) {
      return &stats;
    }
  }
  return nullptr;
}

namespace {

// Every binding resolves to (at most) one of these, per Binding::domain.
struct ResolvedBinding {
  const clang::Decl *decl = nullptr;
  const clang::Expr *expr = nullptr;
  std::optional<clang::QualType> type;
  clang::SourceLocation loc;
  bool present = false;
};

// A flat if-chain reads better than a nested ternary for the same
// decl-else-expr-else-fallback location choice used throughout this file.
clang::SourceLocation location_of(const ResolvedBinding &bound) {
  if (bound.decl != nullptr) {
    return bound.decl->getLocation();
  }
  if (bound.expr != nullptr) {
    return bound.expr->getExprLoc();
  }
  return bound.loc;
}

ExtensionEvidence evidence_for(clang::ASTContext &context,
                               clang::SourceLocation loc) {
  ast::ExpansionLoc expansion = ast::expansion_loc(context, loc);
  return ExtensionEvidence{
      .file = expansion.file, .line = expansion.line, .col = expansion.col};
}

// A fingerprint of the COMPLETE parsed input this rule ran against: every
// file that contributed to the TU (main file, included/generated headers --
// not just the main file), every predefined/-D/-U macro via
// Preprocessor::getPredefines() (Clang bakes every command-line macro into
// this buffer as an implicit "<built-in>" text), target triple/ABI, and
// language standard. Folded into plan_identity.hpp's artifact_identity() so
// a header, generated-input, or -D/-U change is always visible in the
// artifact identity, independent of whether the caller also supplies a
// full HSE-61 workspace/TU descriptor (ExecutionInput may leave both of
// those empty).
std::string tu_content_fingerprint(clang::ASTContext &context,
                                   clang::Preprocessor &preprocessor) {
  clang::SourceManager &source_manager = context.getSourceManager();
  std::vector<std::pair<std::string, std::string>> files;
  for (auto it = source_manager.fileinfo_begin();
       it != source_manager.fileinfo_end(); ++it) {
    const clang::FileEntryRef file_entry = it->first;
    if (auto buffer = source_manager.getMemoryBufferForFileOrNone(file_entry)) {
      files.emplace_back(file_entry.getName().str(),
                         cidx::sha256_hex(buffer->getBuffer().str()));
    }
  }
  std::ranges::sort(files, [](const auto &lhs, const auto &rhs) {
    return lhs.first < rhs.first;
  });

  std::ostringstream combined;
  for (const auto &[path, content_hash] : files) {
    combined << path << '\x1f' << content_hash << '\x1e';
  }
  combined << preprocessor.getPredefines() << '\x1e';
  combined << context.getTargetInfo().getTriple().str() << '\x1f'
           << context.getTargetInfo().getABI().str() << '\x1e';
  combined << static_cast<int>(context.getLangOpts().LangStd);
  return cidx::sha256_hex(combined.str());
}

// Counts AST nodes reachable from the translation unit, stopping the moment
// the budget is exceeded. Deliberately NOT a clang::RecursiveASTVisitor
// subclass: every RecursiveASTVisitor customization point (Traverse*,
// Visit*, shouldVisit*) shares a name with a base-class method by design
// (CRTP static polymorphism), which trips clang-tidy's
// bugprone-derived-method-shadowing-base-method unconditionally -- this
// class instead uses its own uniquely-named recursive walk() overloads over
// Decl/Stmt's own public child-iteration APIs, so no base-class method name
// is ever shadowed.
class NodeBudgetCounter final {
public:
  explicit NodeBudgetCounter(std::int64_t budget) : budget_(budget) {}

  void walk(const clang::Decl *decl) {
    if (decl == nullptr || exhausted_) {
      return;
    }
    if (++visited_ > budget_) {
      exhausted_ = true;
      return;
    }
    if (const auto *decl_context = llvm::dyn_cast<clang::DeclContext>(decl)) {
      for (const clang::Decl *child : decl_context->decls()) {
        walk(child);
        if (exhausted_) {
          return;
        }
      }
    }
    if (const auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      if (function->doesThisDeclarationHaveABody()) {
        walk(function->getBody());
      }
    } else if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl)) {
      walk(var->getInit());
    } else if (const auto *field = llvm::dyn_cast<clang::FieldDecl>(decl)) {
      walk(field->getInClassInitializer());
    }
  }

  void walk(const clang::Stmt *stmt) {
    if (stmt == nullptr || exhausted_) {
      return;
    }
    if (++visited_ > budget_) {
      exhausted_ = true;
      return;
    }
    for (const clang::Stmt *child : stmt->children()) {
      walk(child);
      if (exhausted_) {
        return;
      }
    }
    // A lambda's closure body belongs to its implicit call-operator
    // FunctionDecl, not to the LambdaExpr's own Stmt::children().
    if (const auto *lambda = llvm::dyn_cast<clang::LambdaExpr>(stmt)) {
      walk(lambda->getBody());
    }
  }

  [[nodiscard]] bool exhausted() const { return exhausted_; }
  [[nodiscard]] std::int64_t visited() const { return visited_; }

private:
  std::int64_t budget_;
  std::int64_t visited_ = 0;
  bool exhausted_ = false;
};

// Combinators whose Clang implementation repeatedly re-traverses a subtree
// for every candidate node during matchAST (unlike a plain narrowing
// predicate such as hasName(), which is O(1) per node). A rule's ESTIMATED
// matcher-evaluation work is visited_nodes * (1 + combinator_count): using
// one of these against a large TU must fit a proportionally smaller
// effective budget, closing the explosive-rule hole that a one-time AST
// size check alone cannot (max_matches only stops fact emission, never
// Clang's own internal traversal).
const std::set<std::string> &traversal_work_combinators() {
  static const std::set<std::string> combinators = {"hasDescendant",
                                                    "hasAncestor"};
  return combinators;
}

// Default single-primitive identity for a binding: canonical USR for a
// declaration, source anchor otherwise. Used by owner_position/composed,
// which reference OTHER bindings by name rather than declaring their own
// recipe.
std::optional<std::string> default_identity(clang::ASTContext &context,
                                            const ResolvedBinding &bound) {
  if (bound.decl != nullptr) {
    std::string usr = ast::usr_for_decl(bound.decl);
    if (!usr.empty()) {
      return "usr:" + usr;
    }
  }
  const clang::SourceLocation loc = location_of(bound);
  if (loc.isInvalid()) {
    return std::nullopt;
  }
  ExtensionEvidence anchor = evidence_for(context, loc);
  if (anchor.file.empty()) {
    return std::nullopt;
  }
  return "anchor:" + anchor.file + ":" + std::to_string(anchor.line) + ":" +
         std::to_string(anchor.col);
}

std::optional<std::string>
compute_identity(clang::ASTContext &context, const IdentityRecipe &identity,
                 const std::string &owning_binding,
                 const std::map<std::string, ResolvedBinding> &bound) {
  auto it = bound.find(owning_binding);
  if (it == bound.end() || !it->second.present) {
    return std::nullopt;
  }
  const ResolvedBinding &self = it->second;
  switch (identity.kind) {
  case IdentityKind::usr: {
    if (self.decl == nullptr) {
      return std::nullopt;
    }
    std::string usr = ast::usr_for_decl(self.decl);
    return usr.empty() ? std::nullopt : std::make_optional("usr:" + usr);
  }
  case IdentityKind::source_anchor: {
    const clang::SourceLocation loc = location_of(self);
    if (loc.isInvalid()) {
      return std::nullopt;
    }
    ExtensionEvidence anchor = evidence_for(context, loc);
    if (anchor.file.empty()) {
      return std::nullopt;
    }
    return "anchor:" + anchor.file + ":" + std::to_string(anchor.line) + ":" +
           std::to_string(anchor.col);
  }
  case IdentityKind::type_key: {
    clang::QualType type;
    if (self.type) {
      type = *self.type;
    } else if (const auto *value_decl =
                   llvm::dyn_cast_or_null<clang::ValueDecl>(self.decl)) {
      type = value_decl->getType();
    } else if (self.expr != nullptr) {
      type = self.expr->getType();
    }
    if (type.isNull()) {
      return std::nullopt;
    }
    return "type:" +
           type.getCanonicalType().getAsString(context.getPrintingPolicy());
  }
  case IdentityKind::owner_position: {
    auto owner_it = bound.find(identity.components[0]);
    if (owner_it == bound.end() || !owner_it->second.present) {
      return std::nullopt;
    }
    auto owner_identity = default_identity(context, owner_it->second);
    if (!owner_identity) {
      return std::nullopt;
    }
    return "owner:" + *owner_identity + "#" + identity.components[1];
  }
  case IdentityKind::composed: {
    std::ostringstream joined;
    for (const auto &component : identity.components) {
      auto component_it = bound.find(component);
      if (component_it == bound.end() || !component_it->second.present) {
        return std::nullopt;
      }
      auto component_identity = default_identity(context, component_it->second);
      if (!component_identity) {
        return std::nullopt;
      }
      joined << *component_identity << '|';
    }
    return "composed:sha256:" + cidx::sha256_hex(joined.str());
  }
  }
  return std::nullopt;
}

std::optional<std::string> read_property(const ResolvedBinding &bound,
                                         const std::string &property) {
  if (property == "spelling") {
    if (const auto *named =
            llvm::dyn_cast_or_null<clang::NamedDecl>(bound.decl)) {
      return named->getNameAsString();
    }
    return std::nullopt;
  }
  if (property == "qualified_name") {
    if (const auto *named =
            llvm::dyn_cast_or_null<clang::NamedDecl>(bound.decl)) {
      return named->getQualifiedNameAsString();
    }
    return std::nullopt;
  }
  if (property == "is_pure") {
    if (const auto *method =
            llvm::dyn_cast_or_null<clang::CXXMethodDecl>(bound.decl)) {
      return method->isPureVirtual() ? "true" : "false";
    }
    return std::nullopt;
  }
  if (property == "is_static") {
    if (const auto *func =
            llvm::dyn_cast_or_null<clang::FunctionDecl>(bound.decl)) {
      return func->isStatic() ? "true" : "false";
    }
    if (const auto *var = llvm::dyn_cast_or_null<clang::VarDecl>(bound.decl)) {
      return var->isStaticDataMember() ||
                     var->getStorageClass() == clang::SC_Static
                 ? "true"
                 : "false";
    }
    return std::nullopt;
  }
  if (property == "is_virtual") {
    if (const auto *method =
            llvm::dyn_cast_or_null<clang::CXXMethodDecl>(bound.decl)) {
      return method->isVirtual() ? "true" : "false";
    }
    return std::nullopt;
  }
  if (property == "access_spelling") {
    if (bound.decl != nullptr) {
      switch (bound.decl->getAccess()) {
      case clang::AS_public:
        return "public";
      case clang::AS_protected:
        return "protected";
      case clang::AS_private:
        return "private";
      case clang::AS_none:
        return "none";
      }
    }
    return std::nullopt;
  }
  if (property == "storage_class") {
    if (const auto *var = llvm::dyn_cast_or_null<clang::VarDecl>(bound.decl)) {
      return std::string(clang::VarDecl::getStorageClassSpecifierString(
          var->getStorageClass()));
    }
    return std::nullopt;
  }
  if (property == "type_spelling" || property == "canonical_type_spelling") {
    if (bound.expr != nullptr) {
      return bound.expr->getType().getAsString();
    }
    if (const auto *value_decl =
            llvm::dyn_cast_or_null<clang::ValueDecl>(bound.decl)) {
      return value_decl->getType().getAsString();
    }
    return std::nullopt;
  }
  if (property == "value_kind") {
    if (bound.expr != nullptr) {
      switch (bound.expr->getValueKind()) {
      case clang::VK_PRValue:
        return "prvalue";
      case clang::VK_LValue:
        return "lvalue";
      case clang::VK_XValue:
        return "xvalue";
      }
    }
    return std::nullopt;
  }
  return std::nullopt;
}

class RuleMatchCallback final
    : public clang::ast_matchers::MatchFinder::MatchCallback {
public:
  RuleMatchCallback(const ExtractionRule &rule, const std::string &plan_hash,
                    const std::string &artifact_identity,
                    ExtensionFactSink &sink, RuleExecutionStats &stats,
                    std::vector<ExecutionDiagnostic> &diagnostics,
                    std::size_t &total_emitted, std::size_t hard_output_cap)
      : rule_(rule), plan_hash_(plan_hash),
        artifact_identity_(artifact_identity), sink_(sink), stats_(stats),
        diagnostics_(diagnostics), total_emitted_(total_emitted),
        hard_output_cap_(hard_output_cap) {}

  [[nodiscard]] std::optional<clang::TraversalKind>
  getCheckTraversalKind() const override {
    return rule_.traversal == TraversalMode::as_is
               ? clang::TK_AsIs
               : clang::TK_IgnoreUnlessSpelledInSource;
  }

  void
  run(const clang::ast_matchers::MatchFinder::MatchResult &result) override {
    std::map<std::string, ResolvedBinding> bound;
    for (const auto &binding : rule_.bindings) {
      ResolvedBinding resolved;
      switch (binding.domain) {
      case EndpointDomain::declaration:
        resolved.decl = result.Nodes.getNodeAs<clang::Decl>(binding.name);
        resolved.present = resolved.decl != nullptr;
        break;
      case EndpointDomain::expression:
        resolved.expr = result.Nodes.getNodeAs<clang::Expr>(binding.name);
        resolved.present = resolved.expr != nullptr;
        break;
      case EndpointDomain::type: {
        const auto *type =
            result.Nodes.getNodeAs<clang::QualType>(binding.name);
        if (type != nullptr) {
          resolved.type = *type;
          resolved.present = true;
        }
        break;
      }
      case EndpointDomain::custom_node:
        diagnostics_.push_back(ExecutionDiagnostic{
            .rule_id = rule_.id,
            .code = "unsupported_domain",
            .message = "custom_node bindings are not resolved by this engine "
                       "version: " +
                       binding.name});
        break;
      }
      bound.emplace(binding.name, resolved);
    }

    // Scope routing: main_file restricts matches to the TU's designated
    // main file; translation_unit imposes no filter (matches anywhere in
    // the TU, headers included). workspace scope is rejected outright by
    // validate_structure() -- this single-ASTContext engine has no
    // cross-TU execution model, so it is refused before Clang runs rather
    // than silently treated as translation_unit.
    if (rule_.scope == PlanScope::main_file) {
      clang::SourceLocation probe;
      for (const auto &binding : rule_.bindings) {
        auto it = bound.find(binding.name);
        if (it != bound.end() && it->second.present) {
          probe = location_of(it->second);
          if (probe.isValid()) {
            break;
          }
        }
      }
      if (probe.isValid() &&
          !result.SourceManager->isInMainFile(
              result.SourceManager->getExpansionLoc(probe))) {
        return; // out of scope: not a match for this rule.
      }
    }

    ++stats_.matches;
    if (std::cmp_greater(stats_.matches, rule_.budget.max_matches)) {
      stats_.budget_exhausted = true;
      return;
    }
    if (total_emitted_ >= hard_output_cap_ ||
        std::cmp_greater_equal(stats_.emitted,
                               rule_.budget.max_emitted_facts)) {
      stats_.budget_exhausted = true;
      return;
    }

    ExtensionProvenance provenance{.plan_hash = plan_hash_,
                                   .artifact_identity = artifact_identity_,
                                   .rule_id = rule_.id,
                                   .producer_package = rule_.producer_package,
                                   .producer_version = rule_.producer_version,
                                   .completeness = rule_.completeness};

    for (const auto &emit : rule_.emits) {
      if (total_emitted_ >= hard_output_cap_ ||
          std::cmp_greater_equal(stats_.emitted,
                                 rule_.budget.max_emitted_facts)) {
        stats_.budget_exhausted = true;
        return;
      }
      if (emit.node) {
        auto identity = compute_identity(*result.Context, emit.node->identity,
                                         emit.node->binding, bound);
        if (!identity) {
          diagnostics_.push_back(ExecutionDiagnostic{
              .rule_id = rule_.id,
              .code = "identity_unresolved",
              .message = "could not compute identity for binding: " +
                         emit.node->binding});
          continue;
        }
        const auto &self = bound.at(emit.node->binding);
        sink_.emit(ExtensionNodeFact{
            .provenance = provenance,
            .namespace_name = emit.node->namespace_name,
            .node_kind = emit.node->node_kind,
            .identity = *identity,
            .evidence = evidence_for(*result.Context, location_of(self))});
        ++stats_.emitted;
        ++total_emitted_;
      } else if (emit.relation) {
        // A relation's endpoints are not minted through an explicit
        // IdentityRecipe (only EmitNode declares one); each endpoint takes
        // the best available safe primitive for its own binding (USR,
        // falling back to a source anchor).
        const auto &from_bound = bound.at(emit.relation->from_binding);
        const auto &to_bound = bound.at(emit.relation->to_binding);
        auto from = default_identity(*result.Context, from_bound);
        auto to = default_identity(*result.Context, to_bound);
        if (!from || !to) {
          diagnostics_.push_back(ExecutionDiagnostic{
              .rule_id = rule_.id,
              .code = "identity_unresolved",
              .message = "could not compute identity for relation endpoints"});
          continue;
        }
        sink_.emit(ExtensionRelationFact{
            .provenance = provenance,
            .namespace_name = emit.relation->namespace_name,
            .relation_kind = emit.relation->relation_kind,
            .from_identity = *from,
            .to_identity = *to,
            .evidence =
                evidence_for(*result.Context, location_of(from_bound))});
        ++stats_.emitted;
        ++total_emitted_;
      } else if (emit.attribute) {
        const auto &self = bound.at(emit.attribute->binding);
        auto value = read_property(self, emit.attribute->ast_property);
        auto identity = default_identity(*result.Context, self);
        if (!value || !identity) {
          diagnostics_.push_back(ExecutionDiagnostic{
              .rule_id = rule_.id,
              .code = "attribute_unresolved",
              .message = "could not read property '" +
                         emit.attribute->ast_property +
                         "' for binding: " + emit.attribute->binding});
          continue;
        }
        sink_.emit(ExtensionAttributeFact{
            .provenance = provenance,
            .namespace_name = emit.attribute->namespace_name,
            .attribute_name = emit.attribute->attribute_name,
            .identity = *identity,
            .value = *value,
            .evidence = evidence_for(*result.Context, location_of(self))});
        ++stats_.emitted;
        ++total_emitted_;
      } else if (emit.unknown) {
        const auto &self = bound.at(emit.unknown->binding);
        auto identity = default_identity(*result.Context, self);
        sink_.emit(ExtensionUnknownFact{
            .provenance = provenance,
            .namespace_name = emit.unknown->namespace_name,
            .reason_code = emit.unknown->reason_code,
            .identity = identity.value_or("unresolved"),
            .evidence = evidence_for(*result.Context, location_of(self))});
        ++stats_.emitted;
        ++total_emitted_;
      }
    }
  }

private:
  const ExtractionRule &rule_;
  const std::string &plan_hash_;
  const std::string &artifact_identity_;
  ExtensionFactSink &sink_;
  RuleExecutionStats &stats_;
  std::vector<ExecutionDiagnostic> &diagnostics_;
  std::size_t &total_emitted_;
  std::size_t hard_output_cap_;
};

} // namespace

ExecutionReport
execute_plan(const ExtractionPlan &plan, clang::ASTContext &context,
             clang::Preprocessor &preprocessor, ExtensionFactSink &sink,
             const ExecutionInput &input, const ExecutionOptions &options) {
  ValidationResult validation = validate(plan);
  if (!validation.ok()) {
    std::ostringstream message;
    message << "ExtractionPlan failed validation and cannot be executed:";
    for (const auto &error : validation.errors) {
      message << " [" << error.rule_id << "] " << to_string(error.code) << ": "
              << error.message << ";";
    }
    throw PlanNotValidated(message.str());
  }

  ExecutionReport report;
  report.plan_hash = plan_hash(plan);
  report.workspace_identity = input.workspace_identity;
  report.tu_identity = input.tu_identity;
  report.artifact_identity = artifact_identity(
      plan,
      ExecutionIdentityInput{.workspace_identity = input.workspace_identity,
                             .tu_identity = input.tu_identity,
                             .tu_content_fingerprint = tu_content_fingerprint(
                                 context, preprocessor)});

  std::size_t total_emitted = 0;

  for (const auto &rule : plan.rules) {
    RuleExecutionStats stats{.rule_id = rule.id,
                             .matches = 0,
                             .emitted = 0,
                             .budget_exhausted = false};

    // Enforce the visited-node budget BEFORE constructing or running the
    // matcher at all: an oversized/explosive TU never reaches Clang's
    // matcher execution for this rule. The estimated WORK the matcher
    // itself will do also scales with how many repeated-subtree-traversal
    // combinators (hasDescendant/hasAncestor) the rule uses, since Clang's
    // MatchFinder has no public API to interrupt matchAST mid-evaluation --
    // a rule using such a combinator must fit a proportionally smaller
    // effective share of the same declared budget.
    NodeBudgetCounter counter(rule.budget.max_visited_nodes);
    counter.walk(context.getTranslationUnitDecl());
    const std::int64_t combinators = count_matcher_occurrences(
        rule.matcher_expression, traversal_work_combinators());
    const bool estimated_work_exceeded =
        combinators > 0 &&
        counter.visited() > rule.budget.max_visited_nodes / (combinators + 1);
    if (counter.exhausted() || estimated_work_exceeded) {
      stats.budget_exhausted = true;
      report.diagnostics.push_back(ExecutionDiagnostic{
          .rule_id = rule.id,
          .code = "visited_node_budget_exceeded",
          .message =
              counter.exhausted()
                  ? "translation unit exceeds max_visited_nodes=" +
                        std::to_string(rule.budget.max_visited_nodes) +
                        " before any match could be attempted"
                  : "estimated matcher-evaluation work (" +
                        std::to_string(counter.visited()) +
                        " visited nodes "
                        "x " +
                        std::to_string(combinators + 1) + " for " +
                        std::to_string(combinators) +
                        " repeated-subtree-traversal combinator(s)) exceeds "
                        "max_visited_nodes=" +
                        std::to_string(rule.budget.max_visited_nodes)});
      report.rule_stats.push_back(stats);
      continue;
    }

    clang::ast_matchers::MatchFinder finder;
    RuleMatchCallback callback(rule, report.plan_hash, report.artifact_identity,
                               sink, stats, report.diagnostics, total_emitted,
                               options.hard_output_cap);

    llvm::StringRef code(rule.matcher_expression);
    clang::ast_matchers::dynamic::Diagnostics diagnostics;
    auto matcher = clang::ast_matchers::dynamic::Parser::parseMatcherExpression(
        code, &diagnostics);
    if (!matcher) {
      // Unreachable in practice (validate() above already parsed every rule
      // successfully), but never execute a rule whose matcher cannot be
      // reconstructed.
      report.diagnostics.push_back(ExecutionDiagnostic{
          .rule_id = rule.id,
          .code = "unknown_matcher",
          .message = "matcher failed to reconstruct at execution time: " +
                     diagnostics.toStringFull()});
      report.rule_stats.push_back(stats);
      continue;
    }
    if (!finder.addDynamicMatcher(*matcher, &callback)) {
      report.diagnostics.push_back(ExecutionDiagnostic{
          .rule_id = rule.id,
          .code = "unsupported_domain",
          .message =
              "matcher's bound node kind is not supported by this ASTContext"});
      report.rule_stats.push_back(stats);
      continue;
    }
    finder.matchAST(context);
    report.rule_stats.push_back(stats);
  }
  return report;
}

} // namespace cidx::extract
