#include "ast/indexing_plan.hpp"

#include <algorithm>

namespace cidx::ast {

auto IndexingPlan::contains(const std::string &pass_id) const -> bool {
  return std::ranges::any_of(steps_,
                             [&pass_id](const IndexingPlanStep &step) -> bool {
                               return step.pass_id == pass_id;
                             });
}

} // namespace cidx::ast
