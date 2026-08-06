#include "application/services.hpp"

#include "application/tu_fact_cache_service.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "application/analysis_service.hpp"
#include "application/clean_rebuild.hpp"
#include "ast/clang_version.hpp"
#include "ast/index_engine.hpp"
#include "astgraph/astgraph.hpp"
#include "diff/analyze.hpp"
#include "diff/compare.hpp"
#include "diff/report.hpp"
#include "diff/target.hpp"
#include "include_hygiene/analysis.hpp"
#include "include_hygiene/executor.hpp"
#include "include_hygiene/plan.hpp"
#include "index/parallel_index.hpp"
#include "query/cxq.hpp"
#include "query/exec.hpp"
#include "storage/storage.hpp"
#include "util/files.hpp"
#include "util/pathutil.hpp"

namespace cidx::application {
namespace {

protocol::ResultEnvelope base_result(std::string operation,
                                     const ApplicationContext &context) {
  protocol::ResultEnvelope result;
  result.operation = std::move(operation);
  result.identity.fact_sets = {"application"};
  result.identity.freshness = "current";
  result.identity.index = "application-context";
  result.identity.workspace = context.workspace() == nullptr
                                  ? "test-context"
                                  : context.workspace()->snapshot().identity;
  result.producer.backend = "cpp";
  return result;
}

protocol::ResultEnvelope service_result(std::string operation,
                                        const ApplicationContext &context,
                                        std::string service) {
  protocol::ResultEnvelope result = base_result(std::move(operation), context);
  result.result = json_out::Value::obj(
      {{"service", json_out::Value::of(std::move(service))}});
  return result;
}

protocol::ResultEnvelope service_error(std::string operation,
                                       const ApplicationContext &context,
                                       std::string code, std::string message) {
  protocol::ResultEnvelope result = base_result(std::move(operation), context);
  result.status = code == "policy_refuted" ? protocol::Status::Refuted
                                           : protocol::Status::Error;
  result.completeness.state = "unknown";
  result.diagnostics.push_back(
      protocol::Diagnostic{.code = std::move(code),
                           .severity = "error",
                           .message = std::move(message)});
  return result;
}

std::string operation_name(const std::optional<Operation> &operation) {
  if (!operation) {
    return "application";
  }
  if (const CommandMetadata *entry = metadata(*operation); entry != nullptr) {
    return std::string(entry->group) + "." + std::string(entry->name);
  }
  return "application";
}

bool belongs_to_root(const std::string &path, const std::string &root) {
  return path == root || (path.starts_with(root) && path.size() > root.size() &&
                          path[root.size()] == '/');
}

std::vector<std::string> split_configuration(std::string_view value) {
  std::istringstream input{std::string(value)};
  std::vector<std::string> tokens;
  std::string token;
  while (input >> token) {
    tokens.push_back(std::move(token));
  }
  return tokens;
}

void apply_configuration_override(diff::ParseConfig &config,
                                  const std::optional<std::string> &override) {
  if (!override) {
    return;
  }
  config.args = split_configuration(*override);
  config.classes = diff::classify_options(config.args);
  config.config_hash = *override;
}

void log_parse_failure(Logger *logger, const std::string &path,
                       const ast::IndexOneOutcome &outcome) {
  if (logger == nullptr || logger->file_path().empty() ||
      outcome.failed_flags.empty()) {
    return;
  }
  std::string flags;
  for (std::size_t index = 0; index < outcome.failed_flags.size(); ++index) {
    if (index != 0) {
      flags += ' ';
    }
    flags += outcome.failed_flags[index];
  }
  logger->error("cidx.clang",
                path + ": failed parse flags: " + flags +
                    "; clang: " + std::to_string(clang_version_major()));
  std::size_t shown = 0;
  for (const Diagnostic &diagnostic : outcome.diagnostics) {
    if (diagnostic.severity < 3 || shown >= 25) {
      continue;
    }
    ++shown;
    logger->info("cidx.clang", path + ": diag " +
                                   diagnostic.file_path.value_or("") + ":" +
                                   std::to_string(diagnostic.line.value_or(0)) +
                                   ": " + diagnostic.spelling);
  }
}

// S-078: turn a changed dependency into the translation units that have to be
// re-extracted because of it.
//
// Every other question target selection asks is "did *this* file's own bytes
// change?", which is the right question for a source and the wrong one for a
// header. A header is never a target in its own right -- it is indexed as part
// of a translation unit -- so a stale header is deferred, and the units that
// include it answer "unchanged" and are skipped. Without this closure a header
// edit re-extracts nothing at all and the index silently keeps the old facts.
//
// The closure walks `include_edge` backwards from every changed input. That
// table is Layer-0: it is written by every index run regardless of worker
// topology or of whether the optional translation-unit fact cache is enabled,
// it records the destination path even when the destination has no `file` row
// (an unowned or generated intermediate), and it is transitive, so a change to
// a header included by another header reaches the units at the end of the
// chain.
//
// Two properties this deliberately keeps:
//   * it costs nothing when nothing changed. An unchanged run has an empty
//     seed set and issues no query at all;
//   * it cannot invent work on a cold run. Every source is already a target
//     there, so the closure adds nothing.
//
// What it cannot see is a change to an input cidx never recorded a digest for
// -- a system or toolchain header, or an unresolved directive. Those are
// deliberately not content-hashed (see docs/tu-fact-cache.md); the normalized
// toolchain and configuration identities cover their *set*, and the
// translation-unit fact cache refuses a hit for a unit that records one.
//
// Returns the paths of the units to re-extract. Paths rather than records on
// purpose: dispatch order is the legacy apply key that `list_files()` already
// returns, and both cross-translation-unit identity resolution and the bounded
// parallel scheduler's publication order depend on it, so the caller builds
// the target list in that order rather than appending to it.
std::set<std::string>
dependents_of_changed_inputs(Storage &db,
                             const std::vector<std::pair<File, std::string>> &registered,
                             const std::set<std::string> &changed,
                             const std::set<std::string> &already_targeted) {
  std::set<std::string> dependents;
  if (changed.empty()) {
    return dependents;
  }
  std::map<std::int64_t, std::string> path_of_file;
  for (const auto &entry : registered) {
    path_of_file.emplace(entry.first.id, entry.second);
  }

  std::set<std::string> visited = changed;
  std::vector<std::string> pending(changed.begin(), changed.end());
  for (std::size_t index = 0; index < pending.size(); ++index) {
    // Copied, not referenced: `pending` grows inside this loop.
    const std::string node = pending[index];
    for (const IncludeEdge &edge : db.include_edges_to_path(node)) {
      const auto found = path_of_file.find(edge.src_file_id);
      if (found == path_of_file.end()) {
        continue;
      }
      if (visited.insert(found->second).second) {
        pending.push_back(found->second);
      }
    }
  }

  // A forced include (`-include x.hpp`) is a dependency the preprocessor never
  // sees as a directive, so it produces no `include_edge` row. The
  // configuration records it as a generated input, which is where the reverse
  // lookup has to come from.
  //
  // Reaching for it costs a configuration read per registered unit, so it is
  // gated on there being a changed input that is not already a target in its
  // own right. An ordinary one-source edit changes exactly one file, that file
  // is its own target, and this pays nothing.
  const bool forced_include_possible =
      std::ranges::any_of(changed, [&](const std::string &path) {
        return !already_targeted.contains(path);
      });
  for (const auto &entry : registered) {
    if (!forced_include_possible || visited.contains(entry.second) ||
        files::is_header(entry.second)) {
      continue;
    }
    // Resolved through `file_config`, not through `translation_unit`: the
    // index path registers the former and leaves the latter to the workspace
    // lifecycle, so a `translation_unit`-keyed lookup finds nothing here.
    for (const FileConfigApplicability &applicability :
         db.file_configs_for(entry.first.id)) {
      if (applicability.role != "translation_unit") {
        continue;
      }
      const std::optional<TranslationUnitConfig> stored =
          db.translation_unit_config_by_id(applicability.config_id);
      if (!stored) {
        continue;
      }
      const TranslationUnitConfig &config = *stored;
      const bool depends = std::ranges::any_of(
          config.generated_inputs, [&](const std::string &input) {
            // Same resolution the configuration service validates with: a
            // recorded generated input is usually already absolute, and a
            // relative one is relative to the recorded working directory.
            std::string resolved = pathutil::resolve_fs_path(input);
            if (!pathutil::isabs(resolved)) {
              resolved = pathutil::abspath(pathutil::join(
                  config.working_dir.value_or(pathutil::getcwd()), resolved));
            }
            return changed.contains(resolved);
          });
      if (depends) {
        visited.insert(entry.second);
        break;
      }
    }
  }

  for (const std::string &path : visited) {
    if (changed.contains(path) || already_targeted.contains(path) ||
        files::is_header(path)) {
      continue;
    }
    dependents.insert(path);
  }
  return dependents;
}

// The one index lifecycle. Both the in-place path and the clean-rebuild path
// call this with the database they are writing to, so default, --no-graph and
// --defer-transforms behave identically no matter which entry point ran.
// `index_path` is the on-disk location of `db`. Bounded parallel extraction
// needs it because each worker opens its own read-only handle to the same
// database; every other path ignores it.
// `extraction_failure` is the in-phase fault the clean-rebuild qualification
// injects into the translation-unit pipeline itself. It is
// IndexFailurePoint::none on every production path and on the in-place path;
// only the CIDX_CLEAN_REBUILD_FAIL_AT hook can set it.
protocol::ResultEnvelope run_index_pass(
    Storage &db, const std::string &index_path, const IndexRequest &request,
    ApplicationContext &context,
    ast::IndexFailurePoint extraction_failure = ast::IndexFailurePoint::none) {
  std::optional<std::string> source_root;
  if (request.source) {
    const std::optional<Component> component =
        db.get_component_by_name(*request.source);
    if (!component) {
      return service_error("index", context, "unknown_source",
                           "no component named " + *request.source);
    }
    source_root = db.component_abs_base(*component);
  }
  if (request.action == IndexAction::explain) {
    protocol::ResultEnvelope result =
        service_result("index.explain", context, "index");
    result.result = json_out::Value::obj(
        {{"workspace",
          json_out::Value::of(context.workspace() == nullptr
                                  ? std::string{}
                                  : context.workspace()->snapshot().identity)},
         {"index", json_out::Value::of(db.index_identity().freshness)}});
    return result;
  }

  std::vector<std::pair<File, std::string>> targets;
  std::int64_t already = 0;
  std::int64_t deferred = 0;
  json_out::Array file_records;
  const bool rebuild = request.action == IndexAction::rebuild;
  if (rebuild) {
    for (const auto &[file, path] : db.list_files()) {
      if (source_root && !belongs_to_root(path, *source_root)) {
        continue;
      }
      db.set_file_indexed(file.id, false);
      if (!files::is_header(path)) {
        targets.emplace_back(file, path);
      }
    }
  } else if (request.files.empty()) {
    std::vector<std::pair<File, std::string>> registered;
    std::set<std::string> changed;
    for (const auto &[file, path] : db.list_files()) {
      if (source_root && !belongs_to_root(path, *source_root)) {
        continue;
      }
      registered.emplace_back(file, path);
      if (files::index_status(file, path) != files::IndexStatus::kOk) {
        changed.insert(path);
      }
    }

    std::set<std::string> targeted;
    for (const auto &[_, path] : registered) {
      if (changed.contains(path) && !files::is_header(path)) {
        targeted.insert(path);
      }
    }
    // A changed header is not a target in its own right; the units that
    // include it are what has to be re-extracted.
    const std::set<std::string> dependents =
        dependents_of_changed_inputs(db, registered, changed, targeted);
    targeted.insert(dependents.begin(), dependents.end());
    // One ordered pass, so dispatch order stays the legacy apply key
    // regardless of whether a unit was selected by its own change or by a
    // dependency's.
    for (const auto &[file, path] : registered) {
      if (targeted.contains(path)) {
        targets.emplace_back(file, path);
      }
    }

    // Accounting, in `list_files` order so the record set stays deterministic.
    // A unit the closure pulled in is a target, not "already": it is being
    // re-extracted even though its own bytes did not change.
    for (const auto &[file, path] : registered) {
      if (targeted.contains(path)) {
        continue;
      }
      const bool is_changed = changed.contains(path);
      if (is_changed) {
        ++deferred;
      } else {
        ++already;
      }
      file_records.push_back(json_out::Value::obj(
          {{"path", json_out::Value::of(path)},
           {"status", json_out::Value::of(std::string(
                          is_changed ? "deferred" : "already"))}}));
    }
  }

  std::int64_t indexed = 0;
  std::int64_t failed = 0;
  std::int64_t unknown = 0;
  std::int64_t warnings = 0;
  std::int64_t errors = 0;
  std::vector<protocol::Diagnostic> diagnostics;

  if (!request.files.empty() && !rebuild) {
    for (const std::string &file_arg : request.files) {
      const std::string path = files::resolve_file_arg(file_arg, source_root);
      const std::optional<File> file = db.get_file(path);
      if (!file) {
        ++unknown;
        file_records.push_back(json_out::Value::obj(
            {{"path", json_out::Value::of(path)},
             {"status", json_out::Value::of(std::string("unknown"))}}));
        if (diagnostics.size() < context.policy().max_diagnostics) {
          diagnostics.push_back(protocol::Diagnostic{
              .code = "invalid_input",
              .severity = "error",
              .message = "not in index database: " + path});
        }
        continue;
      }
      if (files::index_status(*file, path) == files::IndexStatus::kOk) {
        ++already;
        file_records.push_back(json_out::Value::obj(
            {{"path", json_out::Value::of(path)},
             {"status", json_out::Value::of(std::string("already"))}}));
        continue;
      }
      targets.emplace_back(*file, path);
    }
  }

  bool truncated = false;
  if (context.policy().max_work_items != 0 &&
      targets.size() > context.policy().max_work_items) {
    targets.resize(context.policy().max_work_items);
    truncated = true;
  }

  // One accounting path for both extraction modes. Serial and parallel differ
  // only in who produces the outcome and where it is published; every counter,
  // diagnostic and file record below is derived identically, so `--jobs N`
  // cannot report a run differently from `--jobs 1`.
  const auto publish_progress = [&](std::size_t position,
                                    const std::string &path) {
    context.publish(
        protocol::ProgressEvent{.sequence = static_cast<int64_t>(position),
                                .operation = "index",
                                .message = path,
                                .completed = static_cast<int64_t>(position),
                                .total = static_cast<int64_t>(targets.size())});
  };
  const auto record_outcome = [&](const File &file, const std::string &path,
                                  const ast::IndexOneOutcome &outcome) {
    std::vector<Diagnostic> persisted_diagnostics = outcome.diagnostics;
    if ((outcome.parse_failed || outcome.source_changed) &&
        persisted_diagnostics.empty()) {
      persisted_diagnostics.push_back(Diagnostic{
          .severity = 4,
          .spelling = outcome.error.empty() ? "indexing failed for " + path
                                            : outcome.error,
          .file_path = path});
    }
    if (outcome.parse_failed || outcome.source_changed) {
      db.replace_diagnostics(file.id, persisted_diagnostics);
    }
    for (const Diagnostic &diagnostic : persisted_diagnostics) {
      if (diagnostic.severity >= 3) {
        ++errors;
      } else if (diagnostic.severity == 2) {
        ++warnings;
      }
    }
    if (outcome.parse_failed) {
      log_parse_failure(context.logger(), path, outcome);
    }
    if (outcome.parse_failed || outcome.source_changed) {
      db.set_file_indexed(file.id, false);
      ++failed;
      if (diagnostics.size() < context.policy().max_diagnostics) {
        diagnostics.push_back(protocol::Diagnostic{
            .code = "backend_error",
            .severity = "error",
            .message = outcome.error.empty() ? "indexing failed for " + path
                                             : outcome.error});
      }
      file_records.push_back(json_out::Value::obj(
          {{"path", json_out::Value::of(path)},
           {"status", json_out::Value::of(std::string("failed"))},
           {"error", json_out::Value::of(outcome.error.empty()
                                             ? "indexing failed for " + path
                                             : outcome.error)}}));
    } else {
      ++indexed;
      file_records.push_back(json_out::Value::obj(
          {{"path", json_out::Value::of(path)},
           {"status", json_out::Value::of(std::string("indexed"))},
           {"stored", json_out::Value::of(outcome.stored)},
           {"headers_indexed", json_out::Value::of(outcome.headers.indexed)},
           {"headers_symbols", json_out::Value::of(outcome.headers.symbols)},
           {"headers_already", json_out::Value::of(outcome.headers.already)},
           {"headers_system", json_out::Value::of(outcome.headers.system)},
           {"headers_unowned", json_out::Value::of(outcome.headers.unowned)}}));
    }
  };

  const index::ParallelBudgets budgets{
      .jobs = request.jobs,
      .max_queue_bytes = request.max_queue_bytes,
      .max_queue_items = request.max_queue_items,
      .memory_budget_bytes = request.memory_budget_bytes};
  // The same pure policy the scheduler applies, asked here so a run that plans
  // exactly one worker takes the serial path instead. That is not a silent
  // downgrade of a parallel request: one worker IS the plan, and the serial
  // path computes it with the translation-unit fact cache and without paying
  // for an extraction snapshot.
  const std::size_t planned_workers =
      index::plan_parallel_extraction(budgets, index::probe_host_resources(),
                                      targets.size())
          .workers;
  if (planned_workers > 1 && !index_path.empty()) {
    // Bounded parallel extraction. `targets` is already in the legacy apply
    // order -- list_files orders by (component.path, directory.path,
    // file.name), and an explicit file list keeps the operator's order, which
    // is the order the serial loop would have published in. The scheduler
    // publishes in exactly that order regardless of who finishes first.
    std::vector<index::ParallelIndexTarget> parallel_targets;
    parallel_targets.reserve(targets.size());
    for (const auto &[file, path] : targets) {
      parallel_targets.push_back({.file = file, .path = path});
    }
    std::size_t position = 0;
    const index::ParallelIndexReport report = index::run_parallel_index(
        db, index_path, parallel_targets, request.graph,
        request.no_front_end_reuse, budgets,
        [&context] { return context.cancellation().cancelled(); },
        [&](const index::ParallelIndexTarget &target,
            const ast::IndexOneOutcome &outcome, bool /*published*/) {
          publish_progress(position++, target.path);
          record_outcome(target.file, target.path, outcome);
          return !context.cancellation().cancelled();
        });
    index::record_parallel_index_profile(report);
  } else {
    ast::IndexSession session(db);
    // Same cache orchestration as the CLI: one decision path, one set of
    // counters, one conservative fallback.
    TuFactCacheIndexer indexer(db, session,
                               tu_fact_cache_options_from_environment());
    for (std::size_t position = 0; position < targets.size(); ++position) {
      const auto &[file, path] = targets[position];
      if (context.cancellation().cancelled()) {
        break;
      }
      publish_progress(position, path);
      if (context.cancellation().cancelled()) {
        break;
      }
      const ast::IndexOneOutcome outcome =
          indexer.index_one(file, path, request.graph, extraction_failure,
                            request.no_front_end_reuse);
      record_outcome(file, path, outcome);
    }
  }
  protocol::ResultEnvelope result = service_result("index", context, "index");
  const std::int64_t skipped =
      static_cast<std::int64_t>(targets.size()) - indexed - failed + already;
  const bool cancelled = context.cancellation().cancelled();
  const bool all_current =
      std::ranges::all_of(db.list_files(), [](const auto &entry) {
        return files::index_status(entry.first, entry.second) ==
               files::IndexStatus::kOk;
      });
  const bool incomplete =
      truncated || cancelled || failed != 0 || unknown != 0 || !all_current;
  result.result = json_out::Value::obj(
      {{"indexed", json_out::Value::of(indexed)},
       {"failed", json_out::Value::of(failed)},
       {"skipped", json_out::Value::of(skipped)},
       {"already", json_out::Value::of(already)},
       {"unknown", json_out::Value::of(unknown)},
       {"deferred", json_out::Value::of(deferred)},
       {"warnings", json_out::Value::of(warnings)},
       {"errors", json_out::Value::of(errors)},
       {"files", json_out::Value::arr(std::move(file_records))}});
  if (cancelled) {
    result.status = protocol::Status::Error;
    result.completeness.state = "unknown";
    result.diagnostics.push_back(
        protocol::Diagnostic{.code = "timeout",
                             .severity = "error",
                             .message = "operation was cancelled"});
  } else if (failed != 0 || unknown != 0) {
    result.status = protocol::Status::Error;
    result.completeness.state = "unknown";
    result.diagnostics = std::move(diagnostics);
    if (result.diagnostics.empty()) {
      result.diagnostics.push_back(protocol::Diagnostic{
          .code = failed != 0 ? "backend_error" : "invalid_input",
          .severity = "error",
          .message = "index operation failed"});
    }
  } else if (incomplete) {
    result.status = protocol::Status::Partial;
    result.completeness.state = "partial";
    if (truncated) {
      result.diagnostics.push_back(
          protocol::Diagnostic{.code = "truncated_budget",
                               .severity = "warning",
                               .message = "index work budget reached before "
                                          "all pending files were processed"});
    }
  }
  result.completeness.truncated = truncated;
  if (truncated) {
    result.completeness.budget =
        static_cast<int64_t>(context.policy().max_work_items);
  }
  if (!incomplete) {
    db.stamp_index_identity();
  }
  if (!request.graph) {
    db.mark_transform_pipeline_pending("graph extraction disabled");
  } else if (request.defer_transforms) {
    db.mark_transform_pipeline_pending("derived publication pending");
  } else if (incomplete) {
    db.mark_transform_pipeline_pending("index has pending or selected files");
  } else {
    const TransformReport transforms = db.run_transform_pipeline();
    if (transforms.failed) {
      result.status = protocol::Status::Error;
      result.completeness.state = "unknown";
      result.diagnostics.push_back(protocol::Diagnostic{
          .code = "backend_error",
          .severity = "error",
          .message = "derived transform publication failed",
          .next_action = std::nullopt});
    } else {
      db.stamp_graph_resolved();
    }
  }
  const IndexIdentity identity = db.index_identity();
  result.identity.index = identity.freshness;
  if (result.status == protocol::Status::Error) {
    result.identity.freshness = "unverifiable";
  } else if (identity.freshness == "stale" &&
             result.status == protocol::Status::Complete) {
    result.status = protocol::Status::Unknown;
    result.completeness.state = "unknown";
    result.completeness.stale = true;
    result.completeness.truncated = false;
    result.identity.freshness = "stale";
    result.diagnostics.clear();
    result.diagnostics.push_back(protocol::Diagnostic{
        .code = "stale_input",
        .severity = "error",
        .message = "pending or changed files remain in the selected index"});
  } else if (identity.freshness == "stale") {
    // Preserve partial work and its evidence when a pending row keeps the
    // selected index stale; the stale identity is exposed in identity.index.
    result.identity.freshness = "unverifiable";
  } else {
    result.identity.freshness = identity.freshness;
  }
  return result;
}

// Removes the candidate database unless it has been published or explicitly
// released. A clean rebuild must never leave a half-built database behind.
class CandidateGuard {
public:
  // The path is stored as a filesystem::path so the destructor's removal is
  // itself noexcept: cleanup runs on the failure path and must never raise a
  // second failure over the one being reported.
  explicit CandidateGuard(const std::string &path) : path_(path) {}
  ~CandidateGuard() noexcept {
    if (!armed_) {
      return;
    }
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }
  CandidateGuard(const CandidateGuard &) = delete;
  CandidateGuard &operator=(const CandidateGuard &) = delete;
  CandidateGuard(CandidateGuard &&) = delete;
  CandidateGuard &operator=(CandidateGuard &&) = delete;

  void release() noexcept { armed_ = false; }

private:
  std::filesystem::path path_;
  bool armed_ = true;
};

json_out::Value clean_rebuild_report(const std::string &candidate_path,
                                     const CleanRebuildVerification &checks,
                                     bool published,
                                     const std::string &detail) {
  return json_out::Value::obj(
      {{"published", json_out::Value::of(published)},
       {"candidate", json_out::Value::of(candidate_path)},
       {"schema_version",
        json_out::Value::of(static_cast<int64_t>(checks.schema_version))},
       {"integrity_ok", json_out::Value::of(checks.integrity_ok)},
       {"foreign_keys_ok", json_out::Value::of(checks.foreign_keys_ok)},
       {"catalog_ok", json_out::Value::of(checks.catalog_ok)},
       {"complete", json_out::Value::of(checks.complete)},
       {"files_pending", json_out::Value::of(checks.files_pending)},
       {"catalog_digest", json_out::Value::of(checks.catalog.digest)},
       {"semantic_digest", json_out::Value::of(checks.semantic_digest)},
       {"detail", json_out::Value::of(detail)}});
}

// Clean rebuild: build a private candidate, verify it, publish it atomically.
// The serving database is only ever read, and only replaced by one rename of a
// fully verified file.
protocol::ResultEnvelope run_clean_rebuild(const std::string &index_path,
                                           const IndexRequest &request,
                                           ApplicationContext &context) {
  if (index_path.empty()) {
    return service_error("index", context, "invalid_input",
                         "clean rebuild requires an index database path");
  }
  if (!request.files.empty()) {
    return service_error("index", context, "invalid_input",
                         "clean rebuild rebuilds the whole index; it does not "
                         "accept file arguments");
  }
  if (request.source) {
    return service_error("index", context, "invalid_input",
                         "clean rebuild rebuilds the whole index; it does not "
                         "accept --source");
  }

  // Whether the database in service can be the source of a rebuild at all:
  // schema identity and whole-file containment, both checked read-only before
  // anything is built. Refusing here costs nothing; discovering it after a full
  // rebuild would either misread the catalog or strand a sidecar.
  if (const std::string refusal = clean_rebuild_source_refusal(index_path);
      !refusal.empty()) {
    return service_error("index", context, "invalid_input",
                         "clean rebuild refused: " + refusal);
  }

  const CleanRebuildFailurePoint failure =
      clean_rebuild_failure_point_from_environment();
  const auto injected = [&](CleanRebuildFailurePoint point) {
    return failure == point;
  };
  // The in-phase faults are delivered by the serial translation-unit pipeline's
  // injector, which the bounded parallel scheduler does not carry. Rather than
  // accept the flag and silently not inject it, the combination is refused.
  const ast::IndexFailurePoint extraction_failure = [&] {
    switch (failure) {
    case CleanRebuildFailurePoint::during_extraction:
      return ast::IndexFailurePoint::adapter;
    case CleanRebuildFailurePoint::during_writer_commit:
      return ast::IndexFailurePoint::commit;
    default:
      return ast::IndexFailurePoint::none;
    }
  }();
  if (extraction_failure != ast::IndexFailurePoint::none && request.jobs > 1) {
    return service_error(
        "index", context, "invalid_input",
        "clean rebuild in-phase failure injection (" +
            std::string(clean_rebuild_failure_point_name(failure)) +
            ") is delivered by the serial extraction path; use --jobs 1");
  }
  const auto injection_error = [&](CleanRebuildFailurePoint point) {
    return service_error(
        "index", context, "backend_error",
        "clean rebuild aborted: injected failure at " +
            std::string(clean_rebuild_failure_point_name(point)));
  };

  CleanRebuildInputs inputs;
  storage::DatabaseCatalogIdentity expected;
  const bool had_serving_database = [&] {
    std::error_code ec;
    const bool exists = std::filesystem::exists(index_path, ec);
    return exists && !ec;
  }();
  try {
    inputs = capture_clean_rebuild_inputs(index_path);
    if (had_serving_database) {
      // From the capture, not re-read from the serving file: when the serving
      // database was older, the catalog was read from the migrated copy and
      // re-reading the original would compare against a pre-migration shape.
      expected = inputs.catalog;
    }
  } catch (const std::exception &error) {
    return service_error("index", context, "backend_error",
                         std::string("clean rebuild cannot read the index "
                                     "database: ") +
                             error.what());
  }
  if (injected(CleanRebuildFailurePoint::after_inputs_captured)) {
    return injection_error(CleanRebuildFailurePoint::after_inputs_captured);
  }

  const std::string candidate_path = clean_rebuild_candidate_path(index_path);
  {
    // A candidate left by an earlier crashed run is not evidence about this
    // one; it is removed before the new candidate is created.
    std::error_code ec;
    std::filesystem::remove(candidate_path, ec);
  }
  CandidateGuard guard(candidate_path);

  protocol::ResultEnvelope pass;
  try {
    Storage candidate(candidate_path);
    if (injected(CleanRebuildFailurePoint::after_candidate_created)) {
      return injection_error(CleanRebuildFailurePoint::after_candidate_created);
    }
    replay_clean_rebuild_inputs(candidate, inputs);
    if (!had_serving_database) {
      // Nothing was in service, so the replayed catalog is the only reference
      // the candidate can be held to.
      expected = storage::read_database_catalog_identity(candidate_path);
    }
    if (injected(CleanRebuildFailurePoint::after_inputs_replayed)) {
      return injection_error(CleanRebuildFailurePoint::after_inputs_replayed);
    }
    IndexRequest pass_request = request;
    pass_request.clean = false;
    // Every replayed file starts pending, so the ordinary "index everything
    // pending" lifecycle is exactly what a clean rebuild needs.
    pass_request.action = IndexAction::update;
    pass = run_index_pass(candidate, candidate_path, pass_request, context,
                          extraction_failure);
  } catch (const std::exception &error) {
    return service_error("index", context, "backend_error",
                         std::string("clean rebuild failed: ") + error.what());
  }
  if (injected(CleanRebuildFailurePoint::after_candidate_indexed)) {
    return injection_error(CleanRebuildFailurePoint::after_candidate_indexed);
  }

  if (pass.status != protocol::Status::Complete) {
    pass.result = json_out::Value::obj(
        {{"index", pass.result},
         {"clean_rebuild",
          clean_rebuild_report(candidate_path, CleanRebuildVerification{},
                               false,
                               "candidate was not published: the rebuild did "
                               "not complete")}});
    if (pass.status == protocol::Status::Complete) {
      pass.status = protocol::Status::Error;
    }
    return pass;
  }

  CleanRebuildVerification checks;
  try {
    checks = verify_clean_rebuild_candidate(candidate_path, expected, true);
  } catch (const std::exception &error) {
    return service_error("index", context, "backend_error",
                         std::string("clean rebuild verification failed: ") +
                             error.what());
  }
  if (!checks.passed()) {
    protocol::ResultEnvelope result = service_result("index", context, "index");
    result.status = protocol::Status::Error;
    result.completeness.state = "unknown";
    result.identity.freshness = "unverifiable";
    result.result = json_out::Value::obj(
        {{"index", pass.result},
         {"clean_rebuild", clean_rebuild_report(candidate_path, checks, false,
                                                checks.failure_reason())}});
    result.diagnostics.push_back(
        protocol::Diagnostic{.code = "backend_error",
                             .severity = "error",
                             .message = "clean rebuild refused publication: " +
                                        checks.failure_reason()});
    return result;
  }
  if (injected(CleanRebuildFailurePoint::after_verification)) {
    return injection_error(CleanRebuildFailurePoint::after_verification);
  }

  const std::string refusal = publish_clean_rebuild_candidate(
      candidate_path, index_path, inputs.source_digest, failure);
  if (!refusal.empty()) {
    protocol::ResultEnvelope result = service_result("index", context, "index");
    result.status = protocol::Status::Error;
    result.completeness.state = "unknown";
    result.identity.freshness = "unverifiable";
    result.result = json_out::Value::obj(
        {{"index", pass.result},
         {"clean_rebuild",
          clean_rebuild_report(candidate_path, checks, false, refusal)}});
    result.diagnostics.push_back(protocol::Diagnostic{
        .code = "backend_error",
        .severity = "error",
        .message = "clean rebuild could not publish: " + refusal});
    return result;
  }
  // Published: the candidate path no longer exists, so the guard must not fire.
  guard.release();
  pass.result = json_out::Value::obj(
      {{"index", pass.result},
       {"clean_rebuild",
        clean_rebuild_report(candidate_path, checks, true, "")}});
  return pass;
}

} // namespace

protocol::ResultEnvelope
StorageApplicationOperations::execute(const IndexRequest &request,
                                      ApplicationContext &context) {
  if (!request.graph && request.defer_transforms) {
    return service_error("index", context, "invalid_input",
                         kIndexTransformFlagConflict);
  }
  if (request.action == IndexAction::status &&
      context.read_ports().schema != nullptr) {
    const Stats stats = context.read_ports().schema->stats();
    protocol::ResultEnvelope result =
        service_result("index.status", context, "index");
    result.result =
        json_out::Value::obj({{"files", json_out::Value::of(stats.files)},
                              {"symbols", json_out::Value::of(stats.symbols)},
                              {"edges", json_out::Value::of(stats.edges)}});
    return result;
  }
  // S-074: bounded parallel extraction is refused, not silently downgraded --
  // an operator who asked for it must never be told the run succeeded when it
  // ran serially. Workers open their own read-only handle to the database
  // being written, so a database with no on-disk path (an in-memory index)
  // cannot run the mode at all.
  //
  // Checked here, at the one entry point, rather than inside run_index_pass:
  // both the in-place and clean-rebuild paths funnel through this, and a clean
  // rebuild creates and replays a candidate database BEFORE it reaches the
  // index pass. Checking downstream would do all of that work and then refuse.
  const std::string index_path =
      index_path_.empty() && context.workspace() != nullptr
          ? context.workspace()->index_path()
          : index_path_;
  if (request.jobs > 1 && index_path.empty()) {
    return service_error("index", context, "unsupported",
                         kIndexParallelNeedsFileIndex);
  }
  if (request.clean) {
    return run_clean_rebuild(index_path_, request, context);
  }
  return run_index_pass(storage_, index_path, request, context);
}

protocol::ResultEnvelope
StorageApplicationOperations::execute(const AnalysisRequest &request,
                                      ApplicationContext &context) {
  try {
    protocol::ResultEnvelope result =
        service_result("analysis", context, "souffle-analysis");
    const std::string path =
        index_path_.empty() && context.workspace() != nullptr
            ? context.workspace()->index_path()
            : index_path_;
    result.result = cli::run_analysis_application(request, path).result;
    return result;
  } catch (const std::exception &error) {
    return service_error("analysis", context, "backend_error", error.what());
  }
}

protocol::ResultEnvelope
StorageApplicationOperations::execute(const AstInspectionRequest &request,
                                      ApplicationContext &context) {
  if (request.action != AstInspectionAction::dump) {
    return service_error(
        "ast", context, "invalid_input",
        "AST locals and conditions inspection is not implemented yet");
  }
  const std::optional<File> file =
      storage_.get_file(files::resolve_file_arg(request.source));
  if (!file) {
    return service_error("ast", context, "unknown_source",
                         "source is not registered in the selected index");
  }
  if (!file->compile_options) {
    return service_error("ast", context, "backend_error",
                         "source has no translation-unit configuration");
  }

  std::error_code ec;
  const std::filesystem::path temp_dir =
      std::filesystem::temp_directory_path(ec) / "cidx-application-ast";
  if (ec) {
    return service_error("ast", context, "backend_error",
                         "cannot resolve temporary AST directory");
  }
  std::filesystem::create_directories(temp_dir, ec);
  if (ec) {
    return service_error("ast", context, "backend_error",
                         "cannot create temporary AST directory");
  }
  const std::filesystem::path output =
      temp_dir / (std::to_string(file->id) + ".db");
  try {
    const astgraph::DumpStats stats = astgraph::dump_tu(
        files::resolve_file_arg(request.source), *file->compile_options,
        file->driver, output.string(), astgraph::Options{});
    std::filesystem::remove(output, ec);
    std::filesystem::remove(temp_dir, ec);
    protocol::ResultEnvelope result =
        service_result("ast", context, "astgraph");
    result.result = json_out::Value::obj(
        {{"action", json_out::Value::of(static_cast<int>(request.action))},
         {"source", json_out::Value::of(request.source)},
         {"cursor_nodes", json_out::Value::of(stats.cursor_nodes)},
         {"type_nodes", json_out::Value::of(stats.type_nodes)},
         {"edges", json_out::Value::of(stats.edges)},
         {"symbols", json_out::Value::of(stats.symbols)},
         {"files", json_out::Value::of(stats.files)}});
    return result;
  } catch (const std::exception &error) {
    std::filesystem::remove(output, ec);
    std::filesystem::remove(temp_dir, ec);
    return service_error("ast", context, "backend_error", error.what());
  }
}

protocol::ResultEnvelope
StorageApplicationOperations::execute(const DiffRequest &request,
                                      ApplicationContext &context) {
  try {
    if (request.scope == DiffScope::index) {
      const std::string left =
          request.left_index_identity.value_or(request.left);
      const std::string right =
          request.right_index_identity.value_or(request.right);
      protocol::ResultEnvelope result =
          service_result("diff.index", context, "diff");
      result.result =
          json_out::Value::obj({{"left", json_out::Value::of(left)},
                                {"right", json_out::Value::of(right)},
                                {"equal", json_out::Value::of(left == right)}});
      return result;
    }
    if (request.scope == DiffScope::source) {
      const std::string left =
          request.left_source_revision.value_or(request.left);
      const std::string right =
          request.right_source_revision.value_or(request.right);
      protocol::ResultEnvelope result =
          service_result("diff.source", context, "diff");
      result.result =
          json_out::Value::obj({{"left", json_out::Value::of(left)},
                                {"right", json_out::Value::of(right)},
                                {"equal", json_out::Value::of(left == right)}});
      return result;
    }

    const std::string left_db = request.left_index.value_or("");
    if (left_db.empty()) {
      return service_error("diff", context, "invalid_input",
                           "file and symbol diffs require an index path");
    }
    diff::ParseConfig left_config =
        diff::resolve_parse_config(diff::SideSpec{.side = "left",
                                                  .file = request.left,
                                                  .db = left_db,
                                                  .tu = std::nullopt});
    diff::ParseConfig right_config = diff::resolve_parse_config(
        diff::SideSpec{.side = "right",
                       .file = request.right,
                       .db = request.right_index.value_or(left_db),
                       .tu = std::nullopt});
    apply_configuration_override(left_config, request.left_configuration);
    apply_configuration_override(right_config, request.right_configuration);
    const diff::ConfigDelta delta =
        diff::config_delta(left_config, right_config);
    if (request.scope == DiffScope::configuration) {
      protocol::ResultEnvelope result =
          service_result("diff.configuration", context, "diff");
      result.result = json_out::Value::obj(
          {{"identical", json_out::Value::of(delta.identical)},
           {"includes_changed", json_out::Value::of(delta.includes_changed)},
           {"options_reordered",
            json_out::Value::of(delta.options_reordered)}});
      return result;
    }

    std::optional<diff::Selector> selector;
    if (request.scope == DiffScope::symbol) {
      if (!request.selector) {
        return service_error("diff.symbol", context, "invalid_input",
                             "symbol diffs require a selector");
      }
      selector = diff::Selector{.raw = *request.selector};
    }
    const diff::SideAnalysis left = diff::analyze_side(left_config, selector);
    const diff::SideAnalysis right = diff::analyze_side(right_config, selector);
    const diff::Comparison comparison = diff::compare_sides(
        left, right, "heuristic", delta,
        request.scope == DiffScope::symbol ? "symbol" : "file");
    std::ostringstream rendered;
    diff::render_report(
        diff::ReportSpec{.scope = request.scope == DiffScope::symbol ? "symbol"
                                                                     : "file",
                         .mode = "both",
                         .match = "heuristic",
                         .json = request.json},
        left, right, delta, comparison, rendered);
    protocol::ResultEnvelope result = service_result("diff", context, "diff");
    result.result = json_out::Value::of(rendered.str());
    return result;
  } catch (const std::exception &error) {
    return service_error("diff", context, "backend_error", error.what());
  }
}

protocol::ResultEnvelope
StorageApplicationOperations::execute(const IncludeRequest &request,
                                      ApplicationContext &context) {
  hygiene::AnalysisOptions options;
  options.scope_paths = request.paths;
  options.want_duplicates = request.duplicates || !request.unused;
  options.want_unused = request.unused || !request.duplicates;
  const hygiene::AnalysisResult analysis = hygiene::analyze(storage_, options);
  protocol::ResultEnvelope result =
      service_result("include", context, "include-hygiene");
  result.result = json_out::Value::obj(
      {{"candidates", json_out::Value::of(static_cast<std::int64_t>(
                          analysis.candidates.size()))},
       {"uncovered", json_out::Value::of(static_cast<std::int64_t>(
                         analysis.uncovered_scope.size()))}});
  if (request.action == IncludeAction::plan) {
    if (context.workspace() == nullptr) {
      return service_error("include.plan", context, "invalid_input",
                           "include plans require a workspace");
    }
    const hygiene::CleanupPlan plan = hygiene::build_plan(
        storage_, analysis, context.workspace()->index_path());
    result.result = json_out::Value::obj(
        {{"candidates", json_out::Value::of(static_cast<std::int64_t>(
                            analysis.candidates.size()))},
         {"uncovered", json_out::Value::of(static_cast<std::int64_t>(
                           analysis.uncovered_scope.size()))},
         {"plan", json_out::Value::of(hygiene::serialize(plan))}});
  } else if (request.action == IncludeAction::apply) {
    if (!request.plan) {
      return service_error("include.apply", context, "invalid_input",
                           "include apply requires a plan path");
    }
    std::ifstream input(*request.plan);
    if (!input) {
      return service_error("include.apply", context, "backend_error",
                           "cannot read include plan");
    }
    const hygiene::CleanupPlan plan = hygiene::deserialize(
        std::string((std::istreambuf_iterator<char>(input)),
                    std::istreambuf_iterator<char>()));
    const hygiene::ExecuteResult execution =
        hygiene::execute(storage_, plan,
                         hygiene::ExecuteOptions{.dry_run = request.dry_run,
                                                 .only = request.only});
    result.result = json_out::Value::obj(
        {{"ok", json_out::Value::of(execution.ok)},
         {"removed", json_out::Value::of(execution.removed)},
         {"skipped", json_out::Value::of(execution.skipped)}});
    if (!execution.ok) {
      result.status = protocol::Status::Refuted;
      result.completeness.state = "unknown";
    }
  }
  return result;
}

protocol::ResultEnvelope
DefaultApplicationServices::index(const IndexRequest &request,
                                  ApplicationContext &context) const {
  return context.operations().index == nullptr
             ? service_error("index", context, "backend_error",
                             "index operation is not installed")
             : context.operations().index->execute(request, context);
}

protocol::ResultEnvelope
DefaultApplicationServices::query(const QueryRequest &request,
                                  ApplicationContext &context) const {
  if (context.read_ports().query == nullptr) {
    return service_error("query", context, "backend_error",
                         "query read port is not installed");
  }
  try {
    const query::Plan plan = query::parse_cxq(request.expression);
    query::Executor executor(*context.read_ports().query);
    protocol::ResultEnvelope result = base_result("query", context);
    result.result =
        request.explain
            ? executor.explain(plan)
            : executor.run(plan, std::nullopt, request.max_results).to_json();
    return result;
  } catch (const std::exception &error) {
    return service_error("query", context, "invalid_input", error.what());
  }
}

protocol::ResultEnvelope
DefaultApplicationServices::analysis(const AnalysisRequest &request,
                                     ApplicationContext &context) const {
  if (request.action == AnalysisAction::list) {
    try {
      protocol::ResultEnvelope result =
          service_result("analysis.list", context, "souffle-analysis");
      result.result = cli::run_analysis_application(request, {}).result;
      return result;
    } catch (const std::exception &error) {
      return service_error("analysis.list", context, "backend_error",
                           error.what());
    }
  }
  return context.operations().analysis == nullptr
             ? service_error("analysis", context, "backend_error",
                             "analysis operation is not installed")
             : context.operations().analysis->execute(request, context);
}

protocol::ResultEnvelope
DefaultApplicationServices::workspace(const WorkspaceRequest &request,
                                      ApplicationContext &context) const {
  (void)request;
  return service_error("workspace", context, "invalid_input",
                       "workspace operation is not implemented");
}

protocol::ResultEnvelope
DefaultApplicationServices::ast(const AstInspectionRequest &request,
                                ApplicationContext &context) const {
  return context.operations().ast == nullptr
             ? service_error("ast", context, "backend_error",
                             "AST operation is not installed")
             : context.operations().ast->execute(request, context);
}

protocol::ResultEnvelope
DefaultApplicationServices::diff(const DiffRequest &request,
                                 ApplicationContext &context) const {
  return context.operations().diff == nullptr
             ? service_error("diff", context, "backend_error",
                             "diff operation is not installed")
             : context.operations().diff->execute(request, context);
}

protocol::ResultEnvelope
DefaultApplicationServices::include(const IncludeRequest &request,
                                    ApplicationContext &context) const {
  return context.operations().include == nullptr
             ? service_error("include", context, "backend_error",
                             "include operation is not installed")
             : context.operations().include->execute(request, context);
}

protocol::ResultEnvelope
DefaultApplicationServices::refactor(const RefactoringRequest &request,
                                     ApplicationContext &context) const {
  (void)request;
  return service_error("refactor", context, "invalid_input",
                       "refactoring operation is not implemented");
}

protocol::ResultEnvelope
DefaultApplicationServices::proof(const ProofRequest &request,
                                  ApplicationContext &context) const {
  (void)request;
  return service_error("proof", context, "invalid_input",
                       "proof operation is not implemented");
}

protocol::ResultEnvelope
ApplicationService::failure(const std::optional<Operation> &operation,
                            std::string code, std::string message) {
  protocol::ResultEnvelope result;
  result.operation = operation_name(operation);
  result.status = code == "policy_refuted" ? protocol::Status::Refuted
                                           : protocol::Status::Error;
  result.completeness.state = "unknown";
  result.diagnostics.push_back(
      protocol::Diagnostic{.code = std::move(code),
                           .severity = "error",
                           .message = std::move(message)});
  return result;
}

protocol::ResultEnvelope
ApplicationService::execute(const CommandRequest &request,
                            ApplicationContext &context) const {
  const std::optional<Operation> operation = operation_of(request);
  if (!operation) {
    return failure(std::nullopt, "invalid_input",
                   "request contains an unknown action or scope");
  }
  const CommandMetadata *entry = metadata(*operation);
  if (entry == nullptr) {
    return failure(operation, "backend_error", "operation is not registered");
  }
  if (context.cancellation().cancelled()) {
    return failure(operation, "timeout", "operation was cancelled");
  }
  if (context.policy().access == AccessMode::read_only &&
      entry->mutability == Mutability::mutating) {
    return failure(
        operation, "policy_refuted",
        "read-only application context rejects a mutating operation");
  }
  if (const auto *include = std::get_if<IncludeRequest>(&request);
      include != nullptr && include->action == IncludeAction::plan &&
      include->output &&
      !context.permits(capability_bit(Capability::artifacts))) {
    return failure(operation, "policy_refuted",
                   "include plan output requires the artifacts capability");
  }
  if (!context.permits(entry->required_capabilities) ||
      (entry->mutability == Mutability::mutating &&
       !context.policy().allow_schema_migration &&
       *operation == Operation::workspace_refresh)) {
    return failure(operation, "policy_refuted",
                   "application policy does not grant this capability");
  }

  return std::visit(
      [this, &context](const auto &typed) -> protocol::ResultEnvelope {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, IndexRequest>) {
          return services_.index(typed, context);
        } else if constexpr (std::is_same_v<T, QueryRequest>) {
          return services_.query(typed, context);
        } else if constexpr (std::is_same_v<T, AnalysisRequest>) {
          return services_.analysis(typed, context);
        } else if constexpr (std::is_same_v<T, WorkspaceRequest>) {
          return services_.workspace(typed, context);
        } else if constexpr (std::is_same_v<T, AstInspectionRequest>) {
          return services_.ast(typed, context);
        } else if constexpr (std::is_same_v<T, DiffRequest>) {
          return services_.diff(typed, context);
        } else if constexpr (std::is_same_v<T, IncludeRequest>) {
          return services_.include(typed, context);
        } else if constexpr (std::is_same_v<T, RefactoringRequest>) {
          return services_.refactor(typed, context);
        } else {
          return services_.proof(typed, context);
        }
      },
      request);
}

} // namespace cidx::application
