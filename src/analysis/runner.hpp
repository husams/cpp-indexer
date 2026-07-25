// One deterministic analysis request/result contract for every fact provider.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "analysis/facts.hpp"

namespace cidx::analysis {

struct RelationRequirement {
  std::string name;
  int version = 1;
  int catalog_version = 1;
  std::vector<FactColumn> columns;
};

struct AnalysisPackage {
  std::string name;
  std::string version;
  std::string entry_point;
  std::string engine = "souffle";
  std::string program;
  std::string content_hash;
  std::vector<RelationRequirement> required_relations;

  [[nodiscard]] std::string canonical() const;
};

struct AnalysisOptions {
  int jobs = 1;
  std::int64_t step_budget = 0;
  std::int64_t output_budget = 0;

  [[nodiscard]] std::string canonical() const;
};

enum class AnalysisStatus : std::uint8_t { complete, partial, unknown, error };

struct AnalysisDiagnostic {
  std::string code;
  std::string message;
};

struct AnalysisRun {
  std::string run_id;
  std::string input_hash;
  std::string package_hash;
  AnalysisStatus status = AnalysisStatus::complete;
  std::map<std::string, FactRelation> relations;
  std::vector<AnalysisDiagnostic> diagnostics;
  std::vector<std::string> generated_inputs;

  [[nodiscard]] std::string canonical_result() const;
  [[nodiscard]] std::string artifact_hash() const;
};

class AnalysisEngine {
public:
  virtual ~AnalysisEngine() = default;
  [[nodiscard]] virtual AnalysisRun
  execute(const AnalysisPackage &package, const FactSnapshot &snapshot,
          const AnalysisOptions &options) const = 0;
};

class SouffleAnalysisEngine final : public AnalysisEngine {
public:
  [[nodiscard]] AnalysisRun
  execute(const AnalysisPackage &package, const FactSnapshot &snapshot,
          const AnalysisOptions &options) const override;
};

class AstgraphCallgraphEngine final : public AnalysisEngine {
public:
  using CallgraphFunction =
      std::function<std::vector<FactRow>(const std::string &, int)>;

  explicit AstgraphCallgraphEngine(CallgraphFunction callgraph);

  [[nodiscard]] AnalysisRun
  execute(const AnalysisPackage &package, const FactSnapshot &snapshot,
          const AnalysisOptions &options) const override;

private:
  CallgraphFunction callgraph_;
};

class AnalysisRunner {
public:
  explicit AnalysisRunner(std::unique_ptr<AnalysisEngine> engine);

  [[nodiscard]] AnalysisRun run(const AnalysisPackage &package,
                                const FactProvider &provider,
                                const FactRequest &request = {},
                                const AnalysisOptions &options = {}) const;

private:
  std::unique_ptr<AnalysisEngine> engine_;
};

[[nodiscard]] std::string analysis_status_name(AnalysisStatus status);

} // namespace cidx::analysis
