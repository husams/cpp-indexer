// Typed application requests shared by CLI, agent, SDK, and future IDE
// adapters.  These types deliberately do not depend on CLI11, streams, or
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
};

enum class QueryOutput : std::uint8_t { human, json };

struct QueryRequest {
  std::string expression;
  QueryOutput output = QueryOutput::human;
  bool explain = false;
  std::optional<std::string> index;
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

enum class AstInspectionAction : std::uint8_t { dump, locals, conditions };

struct AstInspectionRequest {
  AstInspectionAction action = AstInspectionAction::dump;
  std::string source;
  std::optional<std::string> index;
  bool json = false;
};

enum class DiffScope : std::uint8_t { file, symbol };

struct DiffRequest {
  DiffScope scope = DiffScope::file;
  std::string left;
  std::string right;
  std::optional<std::string> left_index;
  std::optional<std::string> right_index;
  std::optional<std::string> selector;
  bool json = false;
};

using CommandRequest = std::variant<IndexRequest, QueryRequest, AnalysisRequest,
                                    AstInspectionRequest, DiffRequest>;

} // namespace cidx::application
