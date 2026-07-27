#include "extract/engine.hpp"

#include "extract/matcher_catalog.hpp"
#include "extract/matcher_root_binding.hpp"
#include "extract/plan_identity.hpp"
#include "extract/validator.hpp"

#include "ast/location.hpp"
#include "ast/pass_registry.hpp"
#include "ast/usr.hpp"
#include "util/hashing.hpp"
#include "workspace/context.hpp"

#include "catalogs/generated_catalog.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/Dynamic/Diagnostics.h"
#include "clang/ASTMatchers/Dynamic/Parser.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Specifiers.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/Version.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/HeaderSearchOptions.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"

#include <algorithm>
#include <limits>
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

std::string ExecutionReport::publication_state_seal() const {
  std::ostringstream state;
  const auto append = [&state](const std::string &value) {
    state << value.size() << ':' << value << ';';
  };
  append(plan_hash);
  append(artifact_identity);
  append(workspace_identity);
  append(tu_identity);
  state << (descriptor_backed_ ? '1' : '0') << ';' << rule_stats.size() << ';';
  for (const RuleExecutionStats &stats : rule_stats) {
    append(stats.rule_id);
    state << stats.visited_nodes << ';' << stats.matches << ';' << stats.emitted
          << ';' << (stats.budget_exhausted ? '1' : '0') << ';';
  }
  state << diagnostics.size() << ';';
  for (const ExecutionDiagnostic &diagnostic : diagnostics) {
    append(diagnostic.rule_id);
    append(diagnostic.code);
    append(diagnostic.message);
  }
  return cidx::sha256_hex(state.str());
}

void ExecutionReport::refresh_publication_seal() {
  publication_seal_ = publication_state_seal();
}

bool ExecutionReport::publication_state_is_intact() const {
  return !publication_seal_.empty() &&
         publication_seal_ == publication_state_seal();
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

// Bump whenever a change to this file (or matcher_catalog.hpp) alters what
// facts a plan can produce from IDENTICAL Clang input -- e.g. a change to
// NodeBudgetCounter's traversal, the matcher-work estimate, scope routing, or
// identity computation. Folded into the fingerprint so two runs of the same
// plan against the same source on two different cidx builds are never
// conflated as "the same artifact" purely because the plan and source
// happened not to change.
constexpr std::string_view kExtractionEngineVersion = "cidx-extract-engine/2";

// Serializes EVERY clang::LangOptions flag/value/enum this build of Clang
// knows about, keyed by its own field name, via the same LangOptions.def
// X-macro Clang's own option-marshalling code is generated from -- rather
// than naming a fixed subset (the previous fingerprint only ever looked at
// LangStd). A frontend flag such as `-fno-elide-constructors` is baked into
// LangOptions::ElideConstructors well before HandleTranslationUnit() runs;
// two invocations that differ in ANY LangOptions.def-listed flag therefore
// always disagree here, without this file needing to know each flag's name
// in advance or be updated when Clang adds a new one. LANGOPT/VALUE_LANGOPT
// fields are plain public bitfield members of clang::LangOptionsBase;
// ENUM_LANGOPT fields are protected there and only reachable through the
// public get##Name() accessor clang::LangOptions itself generates from the
// same .def file (clang/Basic/LangOptions.h) -- so this needs two macro
// bodies, not one.
std::string lang_options_fingerprint(const clang::LangOptions &lang_opts) {
  std::ostringstream combined;
#define LANGOPT(Name, Bits, Default, Compatibility, Description)               \
  combined << #Name << '=' << static_cast<unsigned long long>(lang_opts.Name)  \
           << ';';
#define VALUE_LANGOPT(Name, Bits, Default, Compatibility, Description)         \
  LANGOPT(Name, Bits, Default, Compatibility, Description)
#define ENUM_LANGOPT(Name, Type, Bits, Default, Compatibility, Description)    \
  combined << #Name << '='                                                     \
           << static_cast<unsigned long long>(lang_opts.get##Name()) << ';';
#include "clang/Basic/LangOptions.def"
#undef ENUM_LANGOPT
#undef VALUE_LANGOPT
#undef LANGOPT
  return combined.str();
}

// A fingerprint of the COMPLETE parsed input AND tool configuration this rule
// ran against: every file that contributed to the TU (main file,
// included/generated headers -- not just the main file), every
// predefined/-D/-U macro via Preprocessor::getPredefines() (Clang bakes every
// command-line macro into this buffer as an implicit "<built-in>" text),
// target triple/ABI, language standard, the exact header-search-path
// configuration (-I/-isystem/-iquote entries, sysroot, resource dir), and the
// CIDX extraction engine/catalog version. The header-search configuration is
// included even though it may have NO observable effect on which files get
// resolved for a GIVEN source file: two compiler invocations that differ only
// in an inert -I path are still two distinct configurations, and a
// reproducibility contract that only fingerprinted resolved-file content
// would silently conflate them. Folded into plan_identity.hpp's
// artifact_identity() so a header, generated-input, -D/-U, search-path, or
// engine-version change is always visible in the artifact identity,
// independent of whether the caller also supplies a full HSE-61 workspace/TU
// descriptor (ExecutionInput may leave both of those empty).
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
  // The COMPLETE frontend/language configuration this TU was compiled
  // under -- not just LangStd -- so a flag such as `-fno-elide-constructors`
  // (which changes clang::LangOptions::ElideConstructors without touching
  // predefines, the target, or LangStd) always changes this fingerprint.
  combined << lang_options_fingerprint(context.getLangOpts()) << '\x1e';

  // Tool/configuration applicability: the exact search-path configuration
  // this invocation was given, independent of whether any of it was actually
  // consulted resolving this particular TU.
  const clang::HeaderSearchOptions &header_search_opts =
      preprocessor.getHeaderSearchInfo().getHeaderSearchOpts();
  combined << header_search_opts.Sysroot << '\x1f'
           << header_search_opts.ResourceDir << '\x1e';
  // Search-path entries alone are not the complete header-search policy:
  // -nostdinc/-nostdinc++ toggle these bits without adding or removing a
  // UserEntry. Keep the ad hoc fingerprint honest even though production
  // publication additionally carries HSE-61's complete descriptor hash.
  combined << header_search_opts.UseBuiltinIncludes << '\x1f'
           << header_search_opts.UseStandardSystemIncludes << '\x1f'
           << header_search_opts.UseStandardCXXIncludes << '\x1f'
           << header_search_opts.UseLibcxx << '\x1e';
  for (const auto &entry : header_search_opts.UserEntries) {
    combined << entry.Path << '\x1f' << static_cast<int>(entry.Group) << '\x1f'
             << (entry.IsFramework != 0U ? '1' : '0')
             << (entry.IgnoreSysRoot != 0U ? '1' : '0') << '\x1e';
  }

  // CIDX engine/catalog applicability: which build of this extraction engine
  // (and its Souffle-adjacent analysis catalog) produced this artifact --
  // plus the exact Clang this engine is linked against (a Clang upgrade can
  // change matcher/AST behavior for identical source and flags).
  combined << kExtractionEngineVersion << '\x1f'
           << cidx::catalog::kCatalogVersion << '\x1f'
           << cidx::catalog::kCatalogHash << '\x1f'
           << clang::getClangFullVersion() << '\x1e';
  return cidx::sha256_hex(combined.str());
}

// Counts AST nodes reachable from the translation unit, stopping the moment
// the budget is exceeded.
//
// This used to be a hand-written enumeration of Decl/Stmt cases, proven a
// superset of clang::ast_matchers::MatchFinder's own traversal by manual
// audit. That audit never converged: round 4 fixed a constructor
// member-initializer-list gap; round 5 closed six more (parameter default
// args, ctor-inits, noexcept(expr), template bodies + NTTP defaults,
// enum-constant initializers, decltype/typeof return types); a subsequent
// review pass then raised FIVE MORE candidates in about an hour of
// cross-referencing clang/AST/RecursiveASTVisitor.h -- a static_assert's
// condition/message, a decltype(...) on a local inside a lambda body, a
// concept's constraint expression, a template's trailing requires-clause,
// and an inline friend function defined in-class. Four confirmed as real
// (see tests/extraction_engine_test.cpp); the lambda-local-decltype
// candidate did NOT reproduce under this rewrite's own mutation testing --
// see the disproved-candidates note below. There are 150+
// DEF_TRAVERSE_DECL/DEF_TRAVERSE_STMT cases in RecursiveASTVisitor.h, and
// C++23 concepts, coroutines, attributes, and GNU extensions are all in
// cidx's indexing scope -- so hand-auditing one node kind at a time was
// never going to converge regardless.
//
// So the traversal is no longer hand-written: TraversalVisitor below
// delegates to an actual clang::RecursiveASTVisitor, the SAME base class
// clang::ast_matchers::MatchFinder's own internal visitor derives from.
// The counted node set is therefore a superset of the matched node set BY
// CONSTRUCTION, not by an audit that can always miss one more
// DEF_TRAVERSE_* case.
//
// NodeBudgetCounter itself does NOT inherit clang::RecursiveASTVisitor:
// every RecursiveASTVisitor customization point (Traverse*, Visit*,
// shouldVisit*) shares a name with a base-class method by design (CRTP
// static polymorphism), which trips clang-tidy's
// bugprone-derived-method-shadowing-base-method unconditionally on the
// inheriting type. The actual subclass is the private nested
// TraversalVisitor type below -- an implementation detail this class owns
// and never exposes -- so the one narrowly-scoped NOLINT inheritance
// requires sits on that private type, not on NodeBudgetCounter's own public,
// caller-facing surface.
//
// TraversalVisitor runs with shouldVisitImplicitCode() and
// shouldVisitTemplateInstantiations() both true. Those are Clang's own
// names for "the more permissive of the two traversal kinds a rule can
// declare" (TK_AsIs vs. TK_IgnoreUnlessSpelledInSource -- see
// RuleMatchCallback::getCheckTraversalKind() below): a budget pre-check
// runs once per rule, before the matcher itself is even constructed, so it
// cannot know in advance whether that rule's matcher only wants
// TK_IgnoreUnlessSpelledInSource. Counting under the more permissive kind
// can only see MORE nodes than either mode, never fewer, so it is a safe
// superset regardless of which kind the matcher that eventually runs
// declares.
//
// This also retires the previously-hand-rolled walk_type() helper entirely:
// RecursiveASTVisitor's own TraverseType/TraverseTypeLoc dispatch already
// walks into a decltype(expr)/typeof(expr) through ANY wrapping (pointer,
// reference, array, qualified, elaborated, nested-name-specifier, template
// argument) as part of visiting the owning Decl's declared type -- which
// closes, as a side effect of no longer hand-rolling the wrapper-following
// logic at all, a gap walk_type() never covered: a decltype nested inside a
// template argument (e.g. `std::array<int, decltype(f())::value>`).
//
// Two shapes were investigated and deliberately left uncovered by any
// special-casing, for different reasons:
//
// - A structured binding's compiler-synthesized get<N>()/std::tuple_size
//   machinery carries only O(1) hidden content per binding, and that
//   machinery already dominates a DecompositionDecl's own node count --
//   exhaustion measurably flips once a TU declares 10+ bindings. No
//   special-casing is needed (or present) for it here; the generic Decl
//   traversal below counts DecompositionDecl/BindingDecl nodes exactly like
//   any other Decl.
//
// - A decltype(...) on a local variable declared inside a LAMBDA body was
//   raised as a fifth candidate gap in the old hand-rolled counter (the
//   hypothesis: a lambda's implicit call-operator FunctionDecl is only
//   reachable through the LambdaExpr itself, never through any enclosing
//   DeclContext::decls(), so the old counter's generic decls()-enumeration
//   would never independently walk a local inside it). This rewrite's own
//   mutation testing against the OLD hand-rolled counter DISPROVED that
//   hypothesis for every shape tried (the lambda assigned to a named local,
//   an immediately-invoked lambda expression, and a lambda passed as a call
//   argument): Clang links a lambda's closure class into the SAME enclosing
//   DeclContext an ordinary local variable's Decl is linked into, so the old
//   counter's generic top-of-walk(Decl*) DeclContext branch already
//   recursed into it, reached the call operator, and from there reached the
//   local's declared type -- exhaustion scaled correctly with hidden
//   content in every shape measured. No regression test is carried for this
//   candidate; the RecursiveASTVisitor-derived counter below still visits
//   this shape soundly (by construction, like everything else), it simply
//   never was a demonstrated gap in the code it replaces.
//
// One documented consequence of this rewrite: visited() is now a superset
// of what the old hand-written counter ever produced, so it can only grow
// relative to prior releases, never shrink. For a rule with a
// hasDescendant/hasAncestor combinator, that means the visited^2 *
// combinators work estimate below (see estimated_work_exceeded) can trip
// max_visited_nodes on a translation unit that previously passed, with no
// source change -- see "max_visited_nodes counts more nodes than earlier
// releases" in docs/extraction-plan.md for the plan-author-facing guidance
// (raise the rule's budget, or narrow the matcher).
class NodeBudgetCounter final {
public:
  explicit NodeBudgetCounter(std::int64_t budget) : visitor_(budget) {}

  void walk(const clang::Decl *decl) {
    // clang::RecursiveASTVisitor's Traverse* API takes non-const pointers
    // by design (it never mutates the AST through them); the only caller
    // of this method only ever holds a non-const ASTContext::
    // getTranslationUnitDecl() pointer in the first place.
    visitor_.TraverseDecl(const_cast<clang::Decl *>(decl));
  }

  [[nodiscard]] bool exhausted() const { return visitor_.exhausted(); }
  [[nodiscard]] std::int64_t visited() const { return visitor_.visited(); }

private:
  // The actual clang::RecursiveASTVisitor subclass -- see the class comment
  // above. Private to NodeBudgetCounter and never named outside this file.
  // This type is a private implementation detail never named by any caller
  // outside NodeBudgetCounter -- see the class comment above. CRTP requires
  // shouldVisitImplicitCode/shouldVisitTemplateInstantiations/TraverseDecl
  // below to shadow RecursiveASTVisitor's own methods of the same name by
  // design, which is why each carries its own narrowly-scoped
  // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method) rather
  // than a blanket suppression on the class or file.
  class TraversalVisitor final
      : public clang::RecursiveASTVisitor<TraversalVisitor> {
  public:
    explicit TraversalVisitor(std::int64_t budget) : budget_(budget) {}

    // See the class comment above NodeBudgetCounter: this is the more
    // permissive of the two traversal kinds a rule may declare, so counting
    // under it is always a safe, never-undercounting superset of either.
    // Static (rather than a const instance method, RecursiveASTVisitor's own
    // shape): CRTP dispatch calls these through getDerived(), which resolves
    // a static member function through an instance just as well, and neither
    // reads any TraversalVisitor state.
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
    static bool shouldVisitImplicitCode() { return true; }
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
    static bool shouldVisitTemplateInstantiations() { return true; }

    // Both overrides deliberately take the single-argument form.
    // RecursiveASTVisitor's own TRAVERSE_STMT_BASE dispatch macro detects,
    // via has_same_member_pointer_type (clang/AST/RecursiveASTVisitor.h),
    // whether a derived override omits the trailing DataRecursionQueue
    // parameter, and if so calls this override directly for every node
    // instead of taking the data-recursion fast path that would otherwise
    // bypass it -- so every Decl/Stmt this visitor is asked to traverse
    // passes through bump() exactly once, with no separate opt-out needed.
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
    bool TraverseDecl(clang::Decl *decl) {
      if (decl == nullptr) {
        return true;
      }
      if (!bump()) {
        return false;
      }
      return clang::RecursiveASTVisitor<TraversalVisitor>::TraverseDecl(decl);
    }

    bool TraverseStmt(clang::Stmt *stmt) {
      if (stmt == nullptr) {
        return true;
      }
      if (!bump()) {
        return false;
      }
      return clang::RecursiveASTVisitor<TraversalVisitor>::TraverseStmt(stmt);
    }

    [[nodiscard]] bool exhausted() const { return exhausted_; }
    [[nodiscard]] std::int64_t visited() const { return visited_; }

  private:
    // Returns false, without counting, once already exhausted -- or the
    // moment this call pushes visited_ past budget_, so the caller stops
    // descending into the current subtree immediately rather than finishing
    // it before noticing the budget is gone.
    bool bump() {
      if (exhausted_) {
        return false;
      }
      if (++visited_ > budget_) {
        exhausted_ = true;
        return false;
      }
      return true;
    }

    std::int64_t budget_;
    std::int64_t visited_ = 0;
    bool exhausted_ = false;
  };

  TraversalVisitor visitor_;
};

// Multiplies two non-negative counts, clamping to INT64_MAX instead of
// wrapping on overflow. Used to price the worst-case cost of a traversal-work
// combinator over a large TU without risking a silently-wrapped (and
// possibly negative, budget-defeating) product.
std::int64_t saturating_mul(std::int64_t lhs, std::int64_t rhs) {
  if (lhs == 0 || rhs == 0) {
    return 0;
  }
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  if (lhs > kMax / rhs) {
    return kMax;
  }
  return lhs * rhs;
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
  if (property == "type_spelling") {
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
                    const std::string &scope_root_binding,
                    ExtensionFactSink &sink, RuleExecutionStats &stats,
                    std::vector<ExecutionDiagnostic> &diagnostics,
                    std::size_t &total_emitted, std::size_t hard_output_cap)
      : rule_(rule), plan_hash_(plan_hash),
        artifact_identity_(artifact_identity),
        scope_root_binding_(scope_root_binding), sink_(sink), stats_(stats),
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
    //
    // The probe location is always the OUTERMOST/root matched node's
    // location (scope_root_binding_, computed once per rule from the
    // matcher's actual parse tree -- see matcher_root_binding.hpp), never an
    // arbitrary "first bound in declaration order" binding: a rule's
    // bindings are an unordered set of names, so filtering on declaration
    // order would make scope filtering depend on how the rule happened to
    // list its bindings rather than on what it actually matched.
    // validate_matchers() already rejects a main_file-scoped rule whose root
    // is unbound, so scope_root_binding_ is never empty here.
    if (rule_.scope == PlanScope::main_file) {
      const auto root_it = bound.find(scope_root_binding_);
      clang::SourceLocation probe;
      if (root_it != bound.end() && root_it->second.present) {
        probe = location_of(root_it->second);
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
  const std::string &scope_root_binding_;
  ExtensionFactSink &sink_;
  RuleExecutionStats &stats_;
  std::vector<ExecutionDiagnostic> &diagnostics_;
  std::size_t &total_emitted_;
  std::size_t hard_output_cap_;
};

// Forwards every fact collected for ONE rule's run into the caller's real
// sink, downgrading a `complete`-declared fact's provenance to `partial`
// when `downgrade_to_partial` is set. This exists because a rule's
// truncation status (RuleExecutionStats::budget_exhausted) is only known
// for certain once its ENTIRE matchAST() run finishes -- facts are still
// emitted one match at a time as the AST is walked, before any later match
// in the SAME rule can be known to exhaust the budget. Buffering a rule's
// facts in a local sink and forwarding them here (rather than writing
// straight to the caller's sink from RuleMatchCallback::run()) means every
// fact from a truncated rule is retroactively labelled non-complete,
// instead of only the facts that happened to be emitted after the cutoff.
void forward_rule_facts(ExtensionFactSink &sink,
                        const InMemoryExtensionFactSink &rule_sink,
                        bool downgrade_to_partial) {
  auto downgraded = [&](ExtensionProvenance provenance) {
    if (downgrade_to_partial &&
        provenance.completeness == DeclaredCompleteness::complete) {
      provenance.completeness = DeclaredCompleteness::partial;
    }
    return provenance;
  };
  for (const auto &fact : rule_sink.nodes()) {
    ExtensionNodeFact copy = fact;
    copy.provenance = downgraded(copy.provenance);
    sink.emit(copy);
  }
  for (const auto &fact : rule_sink.relations()) {
    ExtensionRelationFact copy = fact;
    copy.provenance = downgraded(copy.provenance);
    sink.emit(copy);
  }
  for (const auto &fact : rule_sink.attributes()) {
    ExtensionAttributeFact copy = fact;
    copy.provenance = downgraded(copy.provenance);
    sink.emit(copy);
  }
  for (const auto &fact : rule_sink.unknowns()) {
    ExtensionUnknownFact copy = fact;
    copy.provenance = downgraded(copy.provenance);
    sink.emit(copy);
  }
}

void require_executable_plan(const ExtractionPlan &plan) {
  ValidationResult validation = validate(plan);
  if (validation.ok()) {
    return;
  }
  std::ostringstream message;
  message << "ExtractionPlan failed validation and cannot be executed:";
  for (const auto &error : validation.errors) {
    message << " [" << error.rule_id << "] " << to_string(error.code) << ": "
            << error.message << ";";
  }
  throw PlanNotValidated(message.str());
}

std::size_t saturating_add(std::size_t lhs, std::size_t rhs) {
  if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
    return std::numeric_limits<std::size_t>::max();
  }
  return lhs + rhs;
}

} // namespace

ExecutionReport
execute_plan(const ExtractionPlan &plan, clang::ASTContext &context,
             clang::Preprocessor &preprocessor, ExtensionFactSink &sink,
             const ExecutionInput &input, const ExecutionOptions &options) {
  require_executable_plan(plan);

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
                             .visited_nodes = 0,
                             .matches = 0,
                             .emitted = 0,
                             .budget_exhausted = false};

    // Enforce the visited-node budget BEFORE constructing or running the
    // matcher at all: an oversized/explosive TU never reaches Clang's
    // matcher execution for this rule.
    NodeBudgetCounter counter(rule.budget.max_visited_nodes);
    counter.walk(context.getTranslationUnitDecl());
    stats.visited_nodes = static_cast<std::size_t>(counter.visited());
    const std::int64_t combinators = count_matcher_occurrences(
        rule.matcher_expression, traversal_work_combinators());
    // A traversal-work combinator's WORST-CASE cost is quadratic, not
    // linear, in the TU's node count: hasDescendant/hasAncestor re-run a
    // full subtree search for EACH of up to `visited` top-level candidates,
    // and (in the fully degenerate case -- e.g. a chain of nested records)
    // each of those searches can itself cost up to `visited`. A one-time
    // linear estimate ("visited * (combinators + 1)") therefore lets a
    // SINGLE combinator perform unboundedly superlinear work no matter how
    // large max_visited_nodes is declared -- confirmed by the round-4
    // review's repro (100/200/300 nested records at max_visited_nodes =
    // 1,000,000: budget_exhausted stayed false while measured execution
    // time grew 11ms -> 37ms -> 114ms). Clang's MatchFinder::matchAST()
    // offers no hook to interrupt that traversal once started, so this is
    // the only SOUND bound achievable here: charge visited^2 per combinator
    // (saturating, never silently overflowing/wrapping) and refuse to run
    // the matcher at all unless that fits the declared budget. This is
    // deliberately pessimistic (it does not know the actual number of
    // top-level candidates or subtree sizes, only the TU's total node
    // count) -- a correct rejection of an unbounded shape is preferred over
    // an under-priced one that still lets superlinear work through.
    const std::int64_t worst_case_combinator_work =
        combinators > 0 ? saturating_mul(saturating_mul(counter.visited(),
                                                        counter.visited()),
                                         combinators)
                        : 0;
    const bool estimated_work_exceeded =
        combinators > 0 &&
        worst_case_combinator_work > rule.budget.max_visited_nodes;
    if (counter.exhausted() || estimated_work_exceeded) {
      stats.budget_exhausted = true;
      report.diagnostics.push_back(ExecutionDiagnostic{
          .rule_id = rule.id,
          .code = "visited_node_budget_exceeded",
          .message = counter.exhausted()
                         ? "translation unit exceeds max_visited_nodes=" +
                               std::to_string(rule.budget.max_visited_nodes) +
                               " before any match could be attempted"
                         : "worst-case matcher-evaluation work (" +
                               std::to_string(counter.visited()) +
                               " visited nodes squared x " +
                               std::to_string(combinators) +
                               " repeated-subtree-traversal combinator(s) = " +
                               std::to_string(worst_case_combinator_work) +
                               ") exceeds max_visited_nodes=" +
                               std::to_string(rule.budget.max_visited_nodes)});
      report.rule_stats.push_back(stats);
      continue;
    }

    // Computed once per rule (not per match): the outermost/root bind id,
    // used only by main_file scope filtering. validate_matchers() already
    // guarantees this is non-empty whenever rule.scope == main_file.
    const std::string scope_root_binding =
        rule.scope == PlanScope::main_file
            ? root_binding_of(rule.matcher_expression)
            : std::string{};

    // Facts are buffered per-rule rather than written straight to the
    // caller's sink: stats.budget_exhausted is only known for certain once
    // matchAST() below fully returns, so every fact this rule produces must
    // be forwarded (with completeness retroactively downgraded if the rule
    // turned out truncated) in one place after the run finishes -- see
    // forward_rule_facts().
    InMemoryExtensionFactSink rule_sink;
    clang::ast_matchers::MatchFinder finder;
    RuleMatchCallback callback(rule, report.plan_hash, report.artifact_identity,
                               scope_root_binding, rule_sink, stats,
                               report.diagnostics, total_emitted,
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
    forward_rule_facts(sink, rule_sink, stats.budget_exhausted);
    report.rule_stats.push_back(stats);
  }
  report.refresh_publication_seal();
  return report;
}

ExecutionReport execute_plan(const ExtractionPlan &plan,
                             const cidx::TranslationUnitDescriptor &descriptor,
                             cidx::ast::FrontendSession &session,
                             ExtensionFactSink &sink,
                             const ExecutionOptions &options) {
  require_executable_plan(plan);
  if (descriptor.source_identity.empty() ||
      descriptor.workspace_identity.empty() ||
      descriptor.canonical_json.empty() || descriptor.semantic_hash.empty() ||
      cidx::sha256_hex(descriptor.canonical_json) != descriptor.semantic_hash) {
    throw PlanNotValidated(
        "production ExtractionPlan execution requires a complete, internally "
        "consistent HSE-61 TranslationUnitDescriptor");
  }

  std::size_t max_visited = plan.rules.size();
  std::size_t max_emitted = 0;
  std::size_t max_diagnostics = plan.rules.size();
  for (const ExtractionRule &rule : plan.rules) {
    max_visited = saturating_add(
        max_visited, static_cast<std::size_t>(rule.budget.max_visited_nodes));
    max_emitted = saturating_add(
        max_emitted, static_cast<std::size_t>(rule.budget.max_emitted_facts));
    max_diagnostics = saturating_add(
        max_diagnostics, static_cast<std::size_t>(rule.budget.max_matches));
    max_diagnostics =
        saturating_add(max_diagnostics,
                       static_cast<std::size_t>(rule.budget.max_emitted_facts));
  }
  max_emitted = std::min(max_emitted, options.hard_output_cap);

  const bool all_main_file =
      std::ranges::all_of(plan.rules, [](const ExtractionRule &rule) {
        return rule.scope == PlanScope::main_file;
      });
  const bool all_complete =
      std::ranges::all_of(plan.rules, [](const ExtractionRule &rule) {
        return rule.completeness == DeclaredCompleteness::complete;
      });
  std::vector<std::uint32_t> catalog_versions = plan.catalog_versions;
  if (catalog_versions.empty()) {
    catalog_versions.push_back(
        static_cast<std::uint32_t>(cidx::catalog::kCatalogVersion));
  }

  const std::string pass_id = "extract.plan." + plan_hash(plan);
  cidx::ast::ExtractionPassRegistry registry;
  cidx::ast::IndexingPlan indexing_plan;
  std::optional<ExecutionReport> execution_report;
  registry.register_pass(
      cidx::ast::ExtractionPassDescriptor{
          .id = pass_id,
          .version = plan.plan_version,
          .required_capabilities =
              {cidx::ast::FrontendCapability::ast,
               cidx::ast::FrontendCapability::preprocessor},
          .consumed_fact_families = {},
          .produced_fact_families = {"extension_facts", "evidence"},
          .catalog_versions = std::move(catalog_versions),
          .dependencies = {},
          .scope = all_main_file ? cidx::ast::PassScope::main_file
                                 : cidx::ast::PassScope::translation_unit,
          .traversal = cidx::ast::TraversalMode::body,
          .completeness = all_complete ? cidx::ast::FactCompleteness::complete
                                       : cidx::ast::FactCompleteness::partial,
          .trust = cidx::ast::FactTrust::inferred,
          .budget = {.max_visited_constructs = max_visited,
                     .max_emitted_facts = max_emitted,
                     .max_diagnostics = max_diagnostics,
                     .declared = true}},
      [&](cidx::ast::PassExecutionContext &context) {
        execution_report = execute_plan(
            plan, *context.session->ast_context, *context.session->preprocessor,
            sink,
            ExecutionInput{.workspace_identity = descriptor.workspace_identity,
                           .tu_identity = descriptor.semantic_hash},
            options);
        std::size_t visited = 0;
        std::size_t emitted = 0;
        bool budget_exhausted = false;
        for (const RuleExecutionStats &stats : execution_report->rule_stats) {
          visited = saturating_add(visited, stats.visited_nodes);
          emitted = saturating_add(emitted, stats.emitted);
          budget_exhausted = budget_exhausted || stats.budget_exhausted;
        }
        context.metrics.note_visited(visited);
        context.metrics.note_emitted(emitted);
        for (const ExecutionDiagnostic &diagnostic :
             execution_report->diagnostics) {
          context.metrics.note_diagnostic(diagnostic.code + ": " +
                                          diagnostic.message);
        }
        context.metrics.budget_exhausted = budget_exhausted;
      });
  indexing_plan.add(pass_id);
  (void)registry.run(indexing_plan, &session);
  if (!execution_report) {
    throw std::logic_error("registered extraction pass did not execute");
  }
  execution_report->descriptor_backed_ = true;
  execution_report->refresh_publication_seal();
  return std::move(*execution_report);
}

} // namespace cidx::extract
