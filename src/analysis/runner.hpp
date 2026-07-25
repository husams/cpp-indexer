// One deterministic analysis request/result contract for every fact provider.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "analysis/facts.hpp"
#include "storage/artifacts.hpp"

namespace cidx {
class Storage;
}

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
  std::string prelude;
  bool include_catalog_prelude = true;
  std::string content_hash;
  std::vector<RelationRequirement> required_relations;
  std::vector<RelationRequirement> output_relations;

  [[nodiscard]] std::string canonical() const;
};

struct AnalysisOptions {
  int jobs = 1;
  std::int64_t step_budget = 0;
  std::int64_t time_budget_ms = 600'000;
  std::int64_t output_budget = 0;
  std::optional<std::filesystem::path> artifact_root;
  std::int64_t capture_budget = 1'048'576;

  [[nodiscard]] std::string canonical() const;
};

enum class AnalysisStatus : std::uint8_t { complete, partial, unknown, error };

enum class AnalysisResultClass : std::uint8_t {
  complete,
  partial,
  unknown,
  invalid_package,
  package_incompatible,
  unsupported_relation,
  stale_input,
  missing_tu,
  truncated_input,
  timeout,
  step_budget_exceeded,
  output_budget_exceeded,
  provider_failure,
  engine_failure,
  publication_failure
};

struct AnalysisDiagnostic {
  std::string code;
  std::string message;
};

class AnalysisEngineError : public std::runtime_error {
public:
  AnalysisEngineError(std::string code, const std::string &message);

  [[nodiscard]] const std::string &code() const noexcept { return code_; }

private:
  std::string code_;
};

struct AnalysisPublication {
  std::string logical_id;
  std::string namespace_name;
  std::string content_hash;
  std::string relative_path;
  std::vector<std::string> relations;
};

struct AnalysisRun {
  std::string run_id;
  std::string input_hash;
  std::string package_hash;
  AnalysisStatus status = AnalysisStatus::complete;
  AnalysisResultClass result_class = AnalysisResultClass::complete;
  std::map<std::string, FactRelation> relations;
  std::vector<AnalysisDiagnostic> diagnostics;
  std::vector<std::string> generated_inputs;
  std::string engine_version = "unknown";
  std::string stdout_text;
  std::string stderr_text;
  std::int64_t elapsed_ms = 0;
  std::int64_t peak_bytes = 0;
  std::int64_t step_count = 0;
  std::int64_t output_bytes = 0;
  std::optional<AnalysisPublication> publication;

  [[nodiscard]] std::string canonical_result() const;
  [[nodiscard]] std::string artifact_hash() const;
};

class AnalysisEngine {
public:
  virtual ~AnalysisEngine() = default;
  [[nodiscard]] virtual std::string engine_version() const;
  [[nodiscard]] virtual AnalysisRun
  execute(const AnalysisPackage &package, const FactSnapshot &snapshot,
          const AnalysisOptions &options) const = 0;
};

class SouffleAnalysisEngine final : public AnalysisEngine {
public:
  [[nodiscard]] std::string engine_version() const override;
  [[nodiscard]] AnalysisRun
  execute(const AnalysisPackage &package, const FactSnapshot &snapshot,
          const AnalysisOptions &options) const override;
};

class AstgraphCallgraphEngine final : public AnalysisEngine {
public:
  using CallgraphFunction =
      std::function<std::vector<FactRow>(const std::string &, int)>;

  explicit AstgraphCallgraphEngine(CallgraphFunction callgraph);

  [[nodiscard]] std::string engine_version() const override;

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

enum class ProviderKind : std::uint8_t {
  semantic_index,
  astgraph,
  extension,
  composed
};

struct ProviderDeclaration {
  ProviderKind kind = ProviderKind::semantic_index;
  std::filesystem::path path;
  std::shared_ptr<ProviderDeclaration> left;
  std::shared_ptr<ProviderDeclaration> right;
  std::vector<JoinSpec> joins;
};

struct AnalysisRequest {
  AnalysisPackage package;
  ProviderDeclaration provider;
  FactRequest facts;
  AnalysisOptions options;
};

class AnalysisService {
public:
  using EngineFactory =
      std::function<std::unique_ptr<AnalysisEngine>(const AnalysisPackage &)>;

  explicit AnalysisService(EngineFactory factory = {});

  [[nodiscard]] AnalysisRun run(const AnalysisRequest &request) const;

private:
  [[nodiscard]] std::unique_ptr<FactProvider>
  provider_for(const ProviderDeclaration &declaration) const;

  EngineFactory factory_;
};

class AnalysisPublisher {
public:
  [[nodiscard]] static ArtifactRecord
  publish(Storage &storage, const std::filesystem::path &root,
          std::string_view namespace_name, const AnalysisRun &run,
          std::string_view workspace_identity = "workspace:unknown",
          std::string_view tu_identity = "");
};

[[nodiscard]] std::string analysis_status_name(AnalysisStatus status);
[[nodiscard]] std::string
analysis_result_class_name(AnalysisResultClass result_class);

} // namespace cidx::analysis
