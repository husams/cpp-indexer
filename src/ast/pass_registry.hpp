// Registry and execution contract for composable extraction passes.
#pragma once

#include "ast/fact_records.hpp"
#include "ast/indexing_plan.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cidx::ast {

enum class FrontendCapability : std::uint8_t {
  ast,
  preprocessor,
  cfg,
  templates,
};

enum class PassScope : std::uint8_t {
  translation_unit,
  main_file,
  owned_header,
};

enum class TraversalMode : std::uint8_t {
  declaration,
  body,
  preprocessing,
  lifecycle,
};

struct PassBudget {
  std::size_t max_visited_constructs = 0;
  std::size_t max_emitted_facts = 0;
  std::size_t max_diagnostics = 0;
};

struct ExtractionPassDescriptor {
  std::string id;
  std::uint32_t version = 1;
  std::vector<FrontendCapability> required_capabilities;
  std::vector<std::string> produced_fact_families;
  std::vector<std::uint32_t> catalog_versions;
  std::vector<std::string> dependencies;
  PassScope scope = PassScope::translation_unit;
  TraversalMode traversal = TraversalMode::lifecycle;
  FactCompleteness completeness = FactCompleteness::complete;
  FactTrust trust = FactTrust::trusted;
  PassBudget budget;

  [[nodiscard]] auto stable_key() const -> std::string;
};

struct PassMetrics {
  std::size_t visited_constructs = 0;
  std::size_t emitted_facts = 0;
  std::size_t unknown_constructs = 0;
  std::size_t duplicates = 0;
  std::size_t diagnostics = 0;
  std::chrono::microseconds elapsed{};
  bool budget_exhausted = false;
  std::vector<std::string> diagnostic_messages;

  void note_visited(std::size_t count = 1) { visited_constructs += count; }
  void note_emitted(std::size_t count = 1) { emitted_facts += count; }
  void note_unknown(std::size_t count = 1) { unknown_constructs += count; }
  void note_duplicate(std::size_t count = 1) { duplicates += count; }
  void note_diagnostic(std::string message);
};

struct PassExecutionContext {
  PassMetrics &metrics;
};

struct PassExecutionRecord {
  ExtractionPassDescriptor descriptor;
  PassMetrics metrics;
};

struct PassExecutionReport {
  std::vector<PassExecutionRecord> passes;

  [[nodiscard]] auto find(const std::string &id) const
      -> const PassExecutionRecord *;
};

class ExtractionPassRegistry {
public:
  using Runner = std::function<void(PassExecutionContext &)>;

  void register_pass(ExtractionPassDescriptor descriptor, Runner runner);

  [[nodiscard]] auto descriptor(const std::string &id) const
      -> const ExtractionPassDescriptor &;
  [[nodiscard]] auto descriptors() const
      -> std::vector<ExtractionPassDescriptor>;
  [[nodiscard]] auto run(const IndexingPlan &plan) const -> PassExecutionReport;

private:
  struct Entry {
    ExtractionPassDescriptor descriptor;
    Runner runner;
  };
  std::vector<Entry> entries_;
};

} // namespace cidx::ast
