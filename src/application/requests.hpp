// Typed application requests shared by CLI, agent, SDK, and future IDE
// adapters. These types deliberately do not depend on CLI11, streams, or
// command spelling.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cidx::application {

enum class IndexAction : std::uint8_t { update, rebuild, status, explain };

inline constexpr const char *kIndexTransformFlagConflict =
    "--no-graph and --defer-transforms are mutually exclusive";
inline constexpr const char *kIndexCleanRequiresRebuild =
    "--clean is only valid for `index rebuild`";

// S-074. The bounded parallel extraction mechanism -- worker/budget policy,
// the ordered owned-header claim oracle, the bounded reorder buffer and
// legacy-order publication through the single controlled writer -- is complete
// and covered by tests. The mode is still refused end to end because
// EXTRACTION ITSELF STILL READS THE AUTHORITATIVE DATABASE (owned-header file
// rows, per-file configuration applicability, component ownership, portable
// identities) throughout each parse.
//
// cidx ships rollback journaling with FULL synchronous durability
// (storage/sqlite.cpp: `PRAGMA journal_mode = DELETE`), deliberately, and WAL
// is explicitly not enabled without measured atomicity evidence. Under rollback
// journaling a writer needs an EXCLUSIVE lock, which no connection can take
// while another holds a SHARED read lock. Concurrent workers reading the
// database therefore starve the scheduler's controlled writer, which fails the
// publication rather than merely slowing it down.
//
// Closing this needs the story's "workers consume immutable extraction inputs"
// bullet: hoist every extraction-time read into an immutable per-run snapshot
// handed to the worker, so a worker touches no database at all. That is a
// change to the extraction engine's inputs, not to this scheduler, and it is
// not something to fake with a lock retry -- retrying would trade a hard
// failure for an unbounded stall.
inline constexpr const char *kIndexParallelUnavailable =
    "--jobs greater than 1 is not available yet: translation-unit extraction "
    "still reads the index database, which cannot run concurrently with the "
    "controlled writer under rollback journaling. Use --jobs 1.";

struct IndexRequest {
  IndexAction action = IndexAction::update;
  std::vector<std::string> files;
  std::optional<std::string> source;
  bool graph = true;
  bool defer_transforms = false;
  // Opt-in clean rebuild: index into a private candidate database, verify it,
  // and publish it with one atomic rename. Never edits the serving database.
  bool clean = false;
  bool autoderive_labels = true;
  bool no_front_end_reuse = false;
  bool json = false;
  std::optional<std::string> index;
  std::optional<std::string> profile_json;
  std::optional<std::string> profile_sqlite_configuration;
  // Bounded parallel translation-unit extraction (S-074). 0 selects the
  // documented automatic policy; the parsers reject anything that is not a
  // positive integer, so a set value is always usable.
  int jobs = 0;
  std::uint64_t max_queue_bytes = 0;
  std::size_t max_queue_items = 0;
  std::uint64_t memory_budget_bytes = 0;
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
