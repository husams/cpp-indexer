// Typed application requests shared by CLI, agent, SDK, and future IDE
// adapters. These types deliberately do not depend on CLI11, streams, or
// command spelling.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cidx::application {

enum class IndexAction : std::uint8_t { update, rebuild, status, explain };

struct IndexRequest {
  IndexAction action = IndexAction::update;
  std::vector<std::string> files;
  std::optional<std::string> source;
  bool graph = true;
  bool autoderive_labels = true;
  bool no_front_end_reuse = false;
  bool json = false;
  std::optional<std::string> index;
  std::optional<std::string> profile_json;
  std::optional<std::string> profile_sqlite_configuration;
};

enum class QueryOutput : std::uint8_t { human, json };

struct QueryRequest {
  std::string expression;
  QueryOutput output = QueryOutput::human;
  bool explain = false;
  std::optional<std::string> index;
  // An optional caller-owned result budget. The executor reports exhaustion
  // explicitly; it never treats a capped prefix as complete.
  std::optional<std::int64_t> max_results;
};

enum class AnalysisAction : std::uint8_t { list, execute, export_facts };

struct AnalysisRequest {
  AnalysisAction action = AnalysisAction::execute;
  std::optional<std::string> rule;
  std::optional<std::string> rules_file;
  std::optional<std::string> export_directory;
  std::optional<std::string> index;
  int jobs = 1;
};

enum class WorkspaceAction : std::uint8_t { list, show, select, refresh };

struct WorkspaceRequest {
  WorkspaceAction action = WorkspaceAction::show;
  std::optional<std::string> workspace;
  std::optional<std::string> repository;
  std::optional<std::string> revision;
  std::optional<std::string> index;
};

enum class AstInspectionAction : std::uint8_t { dump, locals, conditions };

struct AstInspectionRequest {
  AstInspectionAction action = AstInspectionAction::dump;
  std::string source;
  std::optional<std::string> index;
  bool json = false;
};

enum class DiffScope : std::uint8_t {
  file,
  symbol,
  source,
  configuration,
  index
};

struct DiffRequest {
  DiffScope scope = DiffScope::file;
  std::string left;
  std::string right;
  std::optional<std::string> left_index;
  std::optional<std::string> right_index;
  std::optional<std::string> selector;
  std::optional<std::string> left_source_revision;
  std::optional<std::string> right_source_revision;
  std::optional<std::string> left_configuration;
  std::optional<std::string> right_configuration;
  std::optional<std::string> left_index_identity;
  std::optional<std::string> right_index_identity;
  bool json = false;
};

enum class IncludeAction : std::uint8_t { graph, check, plan, apply };

struct IncludeRequest {
  IncludeAction action = IncludeAction::check;
  std::vector<std::string> paths;
  std::optional<std::string> files_from;
  bool reverse = false;
  bool transitive = false;
  bool cycles = false;
  bool include_system = false;
  bool duplicates = false;
  bool unused = false;
  bool json = false;
  std::optional<std::string> output;
  std::optional<std::string> plan;
  std::vector<std::string> only;
  bool dry_run = false;
  std::optional<std::string> index;
};

enum class RefactoringAction : std::uint8_t { check, plan, apply };

struct RefactoringRequest {
  RefactoringAction action = RefactoringAction::check;
  std::vector<std::string> paths;
  std::optional<std::string> plan;
  std::vector<std::string> only;
  bool dry_run = false;
  std::optional<std::string> index;
};

enum class ProofAction : std::uint8_t { prepare, execute, status, explain };

struct ProofRequest {
  ProofAction action = ProofAction::status;
  std::optional<std::string> target;
  std::optional<std::string> policy;
  std::optional<std::string> budget;
  std::optional<std::string> index;
};

using CommandRequest =
    std::variant<IndexRequest, QueryRequest, AnalysisRequest, WorkspaceRequest,
                 AstInspectionRequest, DiffRequest, IncludeRequest,
                 RefactoringRequest, ProofRequest>;

} // namespace cidx::application
