// ExtractionPlan execution: runs a VALIDATED plan against one Clang
// ASTContext (an HSE-61 FrontendSession's AST) and emits extension facts
// through an ExtensionFactSink (extension_facts.hpp).
//
// execute_plan() re-validates the plan itself (defense in depth: a plan can
// never be executed without having passed validate(), regardless of what the
// caller already checked) and NEVER runs Clang against a plan with
// validation errors.
#pragma once

#include "extract/extension_facts.hpp"
#include "extract/plan_ir.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace cidx {
struct TranslationUnitDescriptor;
namespace ast {
struct FrontendSession;
} // namespace ast
} // namespace cidx

namespace clang {
class ASTContext;
class Preprocessor;
} // namespace clang

namespace cidx::extract {

class PlanNotValidated final : public std::runtime_error {
public:
  explicit PlanNotValidated(const std::string &message);
};

// Caller-supplied identity for the pinned workspace/TU this execution runs
// against (typically WorkspaceSnapshot::identity and
// TranslationUnitDescriptor::semantic_hash, src/workspace/context.hpp).
// Both may be left empty for ad hoc/test execution; execute_plan() always
// additionally folds in a content fingerprint derived from the ASTContext
// and Preprocessor themselves (every file that contributed to the TU,
// every predefined/-D/-U macro via Preprocessor::getPredefines(), target
// triple/ABI, and language standard), so the resulting artifact_identity
// changes on a source/header/config change even when the caller supplies
// no workspace/TU descriptor at all.
struct ExecutionInput {
  std::string workspace_identity;
  std::string tu_identity;
};

struct ExecutionOptions {
  // An absolute safety cap independent of any rule's declared budget, so a
  // defective budget declaration (accepted by validation but still far too
  // large for the process running it) cannot produce an unbounded artifact.
  std::size_t hard_output_cap = 100'000;
};

struct ExecutionDiagnostic {
  std::string rule_id;
  std::string code;
  std::string message;
};

struct RuleExecutionStats {
  std::string rule_id;
  std::size_t visited_nodes = 0;
  std::size_t matches = 0;
  std::size_t emitted = 0;
  bool budget_exhausted = false;
};

struct ExecutionReport {
  std::string plan_hash;
  // Identity of THIS execution -- plan_hash folded with the pinned
  // workspace/TU identity and the ASTContext/Preprocessor content
  // fingerprint (plan_identity.hpp's artifact_identity()). This is what
  // gets stamped onto every ExtensionProvenance and used when publishing
  // the extension artifact (src/extract/artifact.hpp).
  std::string artifact_identity;
  // The pinned identities this report's artifact_identity was actually
  // computed from (copied from ExecutionInput). publish_extension_artifact
  // reads these directly rather than trusting an independently-supplied,
  // potentially mismatched PublicationRequest field.
  std::string workspace_identity;
  std::string tu_identity;
  std::vector<RuleExecutionStats> rule_stats;
  std::vector<ExecutionDiagnostic> diagnostics;

  [[nodiscard]] const RuleExecutionStats *
  find(const std::string &rule_id) const;

  // Publication accepts only reports produced from an HSE-61 translation-unit
  // descriptor through the HSE-63 pass registry. The integrity check also
  // detects any caller mutation of publication-relevant report state after
  // execution.
  [[nodiscard]] bool descriptor_backed() const noexcept {
    return descriptor_backed_;
  }
  [[nodiscard]] bool publication_state_is_intact() const;

private:
  bool descriptor_backed_ = false;
  std::string publication_seal_;

  [[nodiscard]] std::string publication_state_seal() const;
  void refresh_publication_seal();

  friend ExecutionReport
  execute_plan(const ExtractionPlan &plan, clang::ASTContext &context,
               clang::Preprocessor &preprocessor, ExtensionFactSink &sink,
               const ExecutionInput &input, const ExecutionOptions &options);
  friend ExecutionReport
  execute_plan(const ExtractionPlan &plan,
               const cidx::TranslationUnitDescriptor &descriptor,
               cidx::ast::FrontendSession &session, ExtensionFactSink &sink,
               const ExecutionOptions &options);
};

// `preprocessor` is required (not defaulted) so the artifact/content
// fingerprint always has access to the exact macro/predefines state
// (Preprocessor::getPredefines(), which bakes in every -D/-U command-line
// macro) in addition to the ASTContext's file contents and target/language
// configuration -- ASTContext alone cannot see command-line macro state.
[[nodiscard]] ExecutionReport
execute_plan(const ExtractionPlan &plan, clang::ASTContext &context,
             clang::Preprocessor &preprocessor, ExtensionFactSink &sink,
             const ExecutionInput &input = {},
             const ExecutionOptions &options = {});

// Production execution path. It consumes the complete HSE-61 descriptor
// identity and runs as an HSE-63 registered frontend pass. Only reports
// returned by this overload are eligible for publication.
[[nodiscard]] ExecutionReport
execute_plan(const ExtractionPlan &plan,
             const cidx::TranslationUnitDescriptor &descriptor,
             cidx::ast::FrontendSession &session, ExtensionFactSink &sink,
             const ExecutionOptions &options = {});

} // namespace cidx::extract
