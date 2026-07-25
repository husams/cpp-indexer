#include "extract/engine.hpp"

#include "extract/plan_identity.hpp"
#include "extract/validator.hpp"

#include "ast/location.hpp"
#include "ast/usr.hpp"
#include "util/hashing.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/Dynamic/Diagnostics.h"
#include "clang/ASTMatchers/Dynamic/Parser.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Specifiers.h"
#include "clang/Basic/TargetInfo.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"

#include <map>
#include <optional>
#include <sstream>
#include <utility>

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

// A fingerprint of the actual Clang input this rule ran against: the main
// file's source content plus target/language configuration. Folded into
// plan_identity.hpp's artifact_identity() so a source or configuration
// change is always visible in the artifact identity, independent of
// whether the caller also supplies a full HSE-61 workspace/TU descriptor
// (ExecutionInput may leave both of those empty).
std::string tu_content_fingerprint(clang::ASTContext &context) {
  const clang::SourceManager &source_manager = context.getSourceManager();
  const clang::FileID main_id = source_manager.getMainFileID();
  llvm::StringRef buffer;
  if (main_id.isValid()) {
    bool invalid = false;
    buffer = source_manager.getBufferData(main_id, &invalid);
    if (invalid) {
      buffer = llvm::StringRef();
    }
  }
  const std::string target = context.getTargetInfo().getTriple().str();
  const auto lang_std = static_cast<int>(context.getLangOpts().LangStd);
  return cidx::sha256_hex(std::string(buffer)) + "|" + target + "|" +
         std::to_string(lang_std);
}

// Counts AST nodes reachable from the translation unit, stopping the moment
// the budget is exceeded (RecursiveASTVisitor's bool-return convention is an
// interruption signal, not just an observation) -- run BEFORE any matcher is
// constructed so an oversized input never reaches Clang's matcher execution
// at all (HSE-64: "malformed or explosive rules fail without partial
// artifact publication").
class NodeBudgetCounter final
    : public clang::RecursiveASTVisitor<NodeBudgetCounter> {
public:
  explicit NodeBudgetCounter(std::int64_t budget) : budget_(budget) {}

  [[nodiscard]] static bool shouldVisitTemplateInstantiations() { return true; }
  [[nodiscard]] static bool shouldVisitImplicitCode() { return true; }

  bool TraverseDecl(clang::Decl *decl) {
    if (decl == nullptr) {
      return true;
    }
    if (++visited_ > budget_) {
      exhausted_ = true;
      return false;
    }
    return clang::RecursiveASTVisitor<NodeBudgetCounter>::TraverseDecl(decl);
  }

  bool TraverseStmt(clang::Stmt *stmt) {
    if (stmt == nullptr) {
      return true;
    }
    if (++visited_ > budget_) {
      exhausted_ = true;
      return false;
    }
    return clang::RecursiveASTVisitor<NodeBudgetCounter>::TraverseStmt(stmt);
  }

  [[nodiscard]] bool exhausted() const { return exhausted_; }
  [[nodiscard]] std::int64_t visited() const { return visited_; }

private:
  std::int64_t budget_;
  std::int64_t visited_ = 0;
  bool exhausted_ = false;
};

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
                    std::size_t remaining_output_cap)
      : rule_(rule), plan_hash_(plan_hash),
        artifact_identity_(artifact_identity), sink_(sink), stats_(stats),
        diagnostics_(diagnostics), remaining_output_cap_(remaining_output_cap) {
  }

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
    if (stats_.emitted >= remaining_output_cap_ ||
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
      if (stats_.emitted >= remaining_output_cap_ ||
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
  std::size_t remaining_output_cap_;
};

} // namespace

ExecutionReport execute_plan(const ExtractionPlan &plan,
                             clang::ASTContext &context,
                             ExtensionFactSink &sink,
                             const ExecutionInput &input,
                             const ExecutionOptions &options) {
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
  report.artifact_identity = artifact_identity(
      plan, ExecutionIdentityInput{
                .workspace_identity = input.workspace_identity,
                .tu_identity = input.tu_identity,
                .tu_content_fingerprint = tu_content_fingerprint(context)});

  for (const auto &rule : plan.rules) {
    RuleExecutionStats stats{.rule_id = rule.id,
                             .matches = 0,
                             .emitted = 0,
                             .budget_exhausted = false};

    // Enforce the visited-node budget BEFORE constructing or running the
    // matcher at all: an oversized/explosive TU never reaches Clang's
    // matcher execution for this rule.
    NodeBudgetCounter counter(rule.budget.max_visited_nodes);
    counter.TraverseDecl(context.getTranslationUnitDecl());
    if (counter.exhausted()) {
      stats.budget_exhausted = true;
      report.diagnostics.push_back(ExecutionDiagnostic{
          .rule_id = rule.id,
          .code = "visited_node_budget_exceeded",
          .message = "translation unit exceeds max_visited_nodes=" +
                     std::to_string(rule.budget.max_visited_nodes) +
                     " before any match could be attempted"});
      report.rule_stats.push_back(stats);
      continue;
    }

    clang::ast_matchers::MatchFinder finder;
    RuleMatchCallback callback(rule, report.plan_hash, report.artifact_identity,
                               sink, stats, report.diagnostics,
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
