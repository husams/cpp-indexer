#include "ast/pass_registry.hpp"

#include <algorithm>
#include <stdexcept>

namespace cidx::ast {

namespace {

template <typename T>
void append_keys(std::string &out, const std::vector<T> &values) {
  for (const T &value : values) {
    out += std::to_string(static_cast<std::uint8_t>(value));
    out.push_back(',');
  }
}

void append_keys(std::string &out, const std::vector<std::string> &values) {
  for (const std::string &value : values) {
    out += value;
    out.push_back(',');
  }
}

} // namespace

auto ExtractionPassDescriptor::stable_key() const -> std::string {
  std::string key = id + '@' + std::to_string(version) + '|';
  append_keys(key, required_capabilities);
  key.push_back('|');
  append_keys(key, produced_fact_families);
  key += std::to_string(static_cast<std::uint8_t>(scope));
  key += std::to_string(static_cast<std::uint8_t>(traversal));
  return key;
}

void PassMetrics::note_diagnostic(std::string message) {
  ++diagnostics;
  diagnostic_messages.push_back(std::move(message));
}

auto PassExecutionReport::find(const std::string &id) const
    -> const PassExecutionRecord * {
  const auto found = std::ranges::find_if(
      passes, [&id](const PassExecutionRecord &record) -> bool {
        return record.descriptor.id == id;
      });
  return found == passes.end() ? nullptr : &*found;
}

void ExtractionPassRegistry::register_pass(ExtractionPassDescriptor descriptor,
                                           Runner runner) {
  if (descriptor.id.empty() || descriptor.version == 0 || !runner) {
    throw std::invalid_argument("invalid extraction pass registration");
  }
  if (std::ranges::any_of(entries_, [&descriptor](const Entry &entry) -> bool {
        return entry.descriptor.id == descriptor.id;
      })) {
    throw std::invalid_argument("duplicate extraction pass: " + descriptor.id);
  }
  entries_.push_back(
      {.descriptor = std::move(descriptor), .runner = std::move(runner)});
}

auto ExtractionPassRegistry::descriptor(const std::string &id) const
    -> const ExtractionPassDescriptor & {
  const auto found =
      std::ranges::find_if(entries_, [&id](const Entry &entry) -> bool {
        return entry.descriptor.id == id;
      });
  if (found == entries_.end()) {
    throw std::invalid_argument("unknown extraction pass: " + id);
  }
  return found->descriptor;
}

auto ExtractionPassRegistry::descriptors() const
    -> std::vector<ExtractionPassDescriptor> {
  std::vector<ExtractionPassDescriptor> result;
  result.reserve(entries_.size());
  for (const Entry &entry : entries_) {
    result.push_back(entry.descriptor);
  }
  std::ranges::sort(result, {}, &ExtractionPassDescriptor::stable_key);
  return result;
}

auto ExtractionPassRegistry::run(const IndexingPlan &plan) const
    -> PassExecutionReport {
  if (plan.steps().empty()) {
    throw std::invalid_argument("an extraction plan must contain a pass");
  }
  PassExecutionReport report;
  for (const IndexingPlanStep &step : plan.steps()) {
    const auto found =
        std::ranges::find_if(entries_, [&step](const Entry &entry) -> bool {
          return entry.descriptor.id == step.pass_id;
        });
    if (found == entries_.end()) {
      throw std::invalid_argument("plan references unknown extraction pass: " +
                                  step.pass_id);
    }
    for (const std::string &dependency : found->descriptor.dependencies) {
      if (!report.find(dependency)) {
        throw std::invalid_argument("pass dependency is not ordered: " +
                                    found->descriptor.id + " -> " + dependency);
      }
    }
    if (report.find(found->descriptor.id)) {
      throw std::invalid_argument("extraction pass appears twice: " +
                                  found->descriptor.id);
    }

    PassMetrics metrics;
    const auto started = std::chrono::steady_clock::now();
    PassExecutionContext context{.metrics = metrics};
    found->runner(context);
    metrics.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    const PassBudget &budget = found->descriptor.budget;
    if ((budget.max_visited_constructs != 0 &&
         metrics.visited_constructs > budget.max_visited_constructs) ||
        (budget.max_emitted_facts != 0 &&
         metrics.emitted_facts > budget.max_emitted_facts) ||
        (budget.max_diagnostics != 0 &&
         metrics.diagnostics > budget.max_diagnostics)) {
      metrics.budget_exhausted = true;
      metrics.note_diagnostic("deterministic pass budget exceeded");
    }
    report.passes.push_back(
        {.descriptor = found->descriptor, .metrics = std::move(metrics)});
  }
  return report;
}

} // namespace cidx::ast
