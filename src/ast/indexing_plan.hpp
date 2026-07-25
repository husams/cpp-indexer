// Explicit ordering for the load-bearing translation-unit extraction plan.
#pragma once

#include <algorithm>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cidx::ast {

struct IndexingPlanStep {
  std::string pass_id;
};

class IndexingPlan {
public:
  IndexingPlan() = default;
  explicit IndexingPlan(std::vector<IndexingPlanStep> steps)
      : steps_(std::move(steps)) {}

  void add(std::string pass_id) {
    steps_.push_back({.pass_id = std::move(pass_id)});
  }

  void insert_before(const std::string &anchor, std::string pass_id) {
    const auto found = std::ranges::find_if(
        steps_, [&anchor](const IndexingPlanStep &step) -> bool {
          return step.pass_id == anchor;
        });
    if (found == steps_.end()) {
      throw std::invalid_argument("plan anchor is not registered: " + anchor);
    }
    steps_.insert(found, {.pass_id = std::move(pass_id)});
  }

  [[nodiscard]] auto steps() const -> const std::vector<IndexingPlanStep> & {
    return steps_;
  }

  [[nodiscard]] auto contains(const std::string &pass_id) const -> bool;

private:
  std::vector<IndexingPlanStep> steps_;
};

} // namespace cidx::ast
