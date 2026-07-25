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

namespace clang {
class ASTContext;
}

namespace cidx::extract {

class PlanNotValidated final : public std::runtime_error {
public:
  explicit PlanNotValidated(const std::string &message);
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
  std::size_t matches = 0;
  std::size_t emitted = 0;
  bool budget_exhausted = false;
};

struct ExecutionReport {
  std::string plan_hash;
  std::vector<RuleExecutionStats> rule_stats;
  std::vector<ExecutionDiagnostic> diagnostics;

  [[nodiscard]] const RuleExecutionStats *
  find(const std::string &rule_id) const;
};

[[nodiscard]] ExecutionReport
execute_plan(const ExtractionPlan &plan, clang::ASTContext &context,
             ExtensionFactSink &sink, const ExecutionOptions &options = {});

} // namespace cidx::extract
