#include "index/parallel_index.hpp"

#include "ast/fact_batch_artifact.hpp"
#include "index/header_claims.hpp"
#include "profile/index_profile.hpp"
#include "storage/fact_batch_writer.hpp"
#include "storage/storage.hpp"

#include <atomic>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace cidx::index {
namespace {

// The immutable extraction input every worker reads.
//
// Workers must not read the authoritative database. Not because the reads are
// wrong -- they are run-invariant lookups of file rows, component ownership and
// configuration identity -- but because holding a SQLite read lock on the
// database the scheduler is publishing into cannot work under rollback
// journaling: a reader's SHARED lock and the writer's EXCLUSIVE lock exclude
// each other, so a worker that reads while the writer commits fails the
// publication outright.
//
// So the run takes ONE consistent snapshot through the SQLite backup API
// before any worker starts, and every worker reads that instead. It is
// immutable for the run's duration, which makes each worker's view of the
// database independent of how far publication has progressed -- the same
// property the deferred symbol-identity resolution gives the fact stream.
//
// Nothing that must observe this run's own publications reads through it:
// owned-header ownership is decided by the scheduler's claim oracle, and
// cross-translation-unit symbol identity is resolved by the controlled writer
// against the live database.
class ExtractionSnapshot {
public:
  explicit ExtractionSnapshot(cidx::Storage &db, const std::string &index_path)
      : path_(index_path + ".extract-snapshot"), file_(path_),
        journal_(path_ + "-journal") {
    std::error_code discard;
    std::filesystem::remove(path_, discard);
    // Through the persistence service's named backup API, not the raw
    // connection: raw SQLite access belongs to the persistence module.
    db.backup_to(path_);
  }

  ExtractionSnapshot(const ExtractionSnapshot &) = delete;
  auto operator=(const ExtractionSnapshot &) -> ExtractionSnapshot & = delete;
  ExtractionSnapshot(ExtractionSnapshot &&) = delete;
  auto operator=(ExtractionSnapshot &&) -> ExtractionSnapshot & = delete;

  // Both paths are built in the constructor so cleanup allocates nothing and
  // cannot throw out of a destructor.
  ~ExtractionSnapshot() noexcept {
    std::error_code discard;
    std::filesystem::remove(file_, discard);
    std::filesystem::remove(journal_, discard);
  }

  [[nodiscard]] auto path() const -> const std::string & { return path_; }

private:
  std::string path_;
  // Held as paths, not strings, so cleanup constructs nothing and therefore
  // allocates nothing on the destructor path.
  std::filesystem::path file_;
  std::filesystem::path journal_;
};

// A worker's isolated extraction context. Each one owns its own database
// handle, its own IndexSession (and therefore its own Toolchain, descriptor and
// configuration caches, and front-end reuse state), so nothing in the
// extraction path is shared mutable state between workers.
//
// The handle is READ-ONLY on purpose. Extraction is supposed to be free of
// database side effects since the FactBatch and controlled-writer work landed;
// opening read-only turns that from a claim into an enforced invariant, and any
// regression surfaces as a loud SQLITE_READONLY rather than as a silent race
// with the scheduler's writer.
class ExtractionWorker {
public:
  explicit ExtractionWorker(const std::string &index_path)
      : db_(std::make_unique<cidx::Storage>(
            index_path, cidx::Storage::OpenMode::read_only)),
        session_(std::make_unique<ast::IndexSession>(*db_)) {}

  [[nodiscard]] auto database() -> cidx::Storage & { return *db_; }
  [[nodiscard]] auto session() -> ast::IndexSession & { return *session_; }

private:
  std::unique_ptr<cidx::Storage> db_;
  std::unique_ptr<ast::IndexSession> session_;
};

// Reorder pressure is based on the immutable bytes that would cross the
// worker-to-writer boundary, not a record-count estimate. The artifact is
// canonical, so this measurement is deterministic across worker counts.
[[nodiscard]] auto batch_bytes(ast::IndexOneOutcome &outcome) -> std::uint64_t {
  if (!outcome.publication || outcome.publication->artifact) {
    return 0;
  }
  auto artifact = std::make_shared<const ast::FactBatchArtifact>(
      ast::encode_fact_batch_artifact(
          outcome.publication->batch,
          {.metadata = {},
           .spill_threshold_bytes =
               outcome.publication->fact_limits.spill_threshold_bytes,
           .spill_directory = outcome.publication->fact_limits.spill_directory,
           .max_artifact_bytes =
               outcome.publication->fact_limits.max_total_bytes}));
  outcome.publication->artifact = artifact;
  outcome.publication->payload = artifact;
  return artifact->byte_size();
}

} // namespace

auto run_parallel_index(cidx::Storage &db, const std::string &index_path,
                        const std::vector<ParallelIndexTarget> &targets,
                        bool graph_enabled, bool no_front_end_reuse,
                        const ParallelBudgets &budgets,
                        const std::function<bool()> &cancelled,
                        const ParallelIndexObserver &observer,
                        const ParallelExtractionBarrier &extracted)
    -> ParallelIndexReport {
  ParallelIndexReport report;
  report.plan =
      plan_parallel_extraction(budgets, probe_host_resources(), targets.size());
  if (targets.empty()) {
    return report;
  }

  const ExtractionSnapshot snapshot(db, index_path);
  SequencedHeaderClaimOracle oracle;
  // Incremented from worker threads, so they cannot be plain members of the
  // report until the pool has joined.
  std::atomic<std::size_t> retries{0};
  std::atomic<std::size_t> retry_exhausted{0};
  // One isolated context per worker, created lazily on the worker's first
  // dispatch so a plan that never uses its last worker never pays for it.
  std::vector<std::unique_ptr<ExtractionWorker>> workers(report.plan.workers);

  const auto extract =
      [&](std::size_t worker_index,
          std::size_t rank) -> ParallelResult<ast::IndexOneOutcome> {
    if (!workers[worker_index]) {
      workers[worker_index] =
          std::make_unique<ExtractionWorker>(snapshot.path());
    }
    ExtractionWorker &worker = *workers[worker_index];
    const ParallelIndexTarget &target = targets[rank];
    const ast::ExtractionControl control{
        .failure = ast::IndexFailurePoint::none,
        .no_front_end_reuse = no_front_end_reuse,
        .publish = false,
        .claims = &oracle,
        .rank = rank};
    ast::IndexOneOutcome outcome =
        ast::run_index_one(worker.database(), worker.session(), target.file,
                           target.path, graph_enabled, control);
    // Bounded retry for the one transient failure: the source changed while it
    // was being parsed. The re-extraction re-captures the snapshot, and the
    // oracle re-grants this rank its own owned headers, so a retry cannot lose
    // headers to itself or double-count them.
    for (std::size_t attempt = 0;
         outcome.source_changed && attempt < kDefaultSourceChangeRetries &&
         !cancelled();
         ++attempt) {
      retries.fetch_add(1, std::memory_order_relaxed);
      outcome =
          ast::run_index_one(worker.database(), worker.session(), target.file,
                             target.path, graph_enabled, control);
    }
    if (outcome.source_changed) {
      retry_exhausted.fetch_add(1, std::memory_order_relaxed);
    }
    if (extracted) {
      extracted(rank);
    }
    const std::uint64_t bytes = batch_bytes(outcome);
    return ParallelResult<ast::IndexOneOutcome>{
        .payload = std::move(outcome), .bytes = bytes, .error = std::nullopt};
  };

  const auto publication_context = [](const ast::IndexOneOutcome &outcome) {
    return cidx::storage::FactBatchPublicationContext{
        .route_plan = outcome.publication->route_plan,
        .translation_unit = outcome.publication->translation_unit,
        .expected_generation = outcome.publication->expected_generation,
        .source_is_current =
            [](const std::string &candidate,
               const ast::PlannedSourceSnapshot &snapshot) {
              return ast::SourceSnapshot{.mtime = snapshot.mtime,
                                         .md5 = snapshot.md5}
                  .matches(candidate);
            },
        .configuration_id = outcome.publication->configuration_id,
        .configuration = outcome.publication->configuration,
        .failure = cidx::storage::FactBatchWriterFailurePoint::none};
  };

  const auto publish =
      [&](std::span<RankedParallelResult<ast::IndexOneOutcome>> window)
      -> ParallelWindowPublication {
    std::vector<cidx::storage::FactBatchWriterWindowItem> writer_items;
    std::vector<std::size_t> writer_positions;
    writer_items.reserve(window.size());
    writer_positions.reserve(window.size());
    for (std::size_t position = 0; position < window.size(); ++position) {
      auto &[rank, result] = window[position];
      ast::IndexOneOutcome &outcome = result.payload;
      if (result.error) {
        outcome.parse_failed = true;
        if (outcome.error.empty()) {
          outcome.error =
              targets[rank].path + ": extraction failed: " + *result.error;
        }
      }
      if (!outcome.parse_failed && !outcome.source_changed &&
          outcome.publication) {
        writer_positions.push_back(position);
        const auto *payload_artifact =
            std::get_if<std::shared_ptr<const ast::FactBatchArtifact>>(
                &outcome.publication->payload);
        const bool artifact_backed = payload_artifact != nullptr &&
                                     *payload_artifact != nullptr &&
                                     (*payload_artifact)->spilled();
        writer_items.push_back(
            {.batch = artifact_backed ? nullptr : &outcome.publication->batch,
             .artifact = outcome.publication->artifact,
             .context = publication_context(outcome),
             .approximate_bytes = result.bytes});
      } else if (!outcome.parse_failed && !outcome.source_changed) {
        outcome.parse_failed = true;
        if (outcome.error.empty()) {
          outcome.error = "FactBatch extraction produced no publication batch";
        }
      }
    }

    cidx::storage::FactBatchWriterWindowResult applied;
    if (!writer_items.empty()) {
      cidx::storage::FactBatchWriter writer(db);
      try {
        const auto mode =
            writer_items.size() == window.size()
                ? cidx::storage::FactBatchWriterWindowMode::speculative
                : cidx::storage::FactBatchWriterWindowMode::replay_only;
        applied = writer.apply_window(writer_items, cancelled, mode);
      } catch (const std::exception &error) {
        applied.results.resize(writer_items.size());
        for (auto &result : applied.results) {
          result.error =
              std::string("FactBatch publication failed: ") + error.what();
        }
      }
      ast::record_writer_profile(applied.report, profile::active());
      if (profile::active()) {
        profile::add_counter("fact_batch_writer.windows_started",
                             applied.report.windows_started);
        profile::add_counter("fact_batch_writer.windows_committed",
                             applied.report.windows_committed);
        profile::add_counter("fact_batch_writer.windows_rolled_back",
                             applied.report.windows_rolled_back);
        profile::add_counter("fact_batch_writer.window_items",
                             applied.report.window_items);
        profile::add_counter("fact_batch_writer.window_bytes",
                             applied.report.window_bytes);
        profile::add_counter("fact_batch_writer.translation_units_replayed",
                             applied.report.translation_units_replayed);
      }
    }

    std::vector<const cidx::storage::FactBatchWriterResult *> by_position(
        window.size(), nullptr);
    for (std::size_t index = 0;
         index < writer_positions.size() && index < applied.results.size();
         ++index) {
      by_position[writer_positions[index]] = &applied.results[index];
    }

    bool keep_going = true;
    std::size_t items_published = 0;
    for (std::size_t position = 0; position < window.size(); ++position) {
      const std::size_t rank = window[position].rank;
      const ParallelIndexTarget &target = targets[rank];
      ast::IndexOneOutcome &outcome = window[position].result.payload;
      bool published = false;
      if (const auto *result = by_position[position]; result != nullptr) {
        if (result->ok()) {
          published = true;
        } else {
          oracle.revoke_grants(rank);
          outcome.parse_failed = true;
          outcome.error =
              result->error.value_or("FactBatch publication failed");
          outcome.dependency_facts = {};
        }
      }
      if (published) {
        ++report.published;
      } else {
        ++report.failed;
      }
      const bool item_continues = observer(target, outcome, published);
      if (keep_going && item_continues) {
        ++items_published;
      } else {
        keep_going = false;
      }
    }
    ParallelRunStatus stop_status = ParallelRunStatus::none;
    if (!keep_going) {
      stop_status = cancelled() ? ParallelRunStatus::cancelled
                                : ParallelRunStatus::publication_stopped;
    }
    return {.items_published = items_published, .stop_status = stop_status};
  };

  report.status = run_parallel_extraction_windowed<ast::IndexOneOutcome>(
      report.plan, targets.size(), report.plan.max_queue_items, extract,
      publish, cancelled,
      [&oracle](std::size_t rank) { oracle.release_unclaimed(rank); },
      report.run);
  oracle.cancel();
  report.claims = oracle.metrics();
  report.retries = retries.load(std::memory_order_relaxed);
  report.retry_exhausted = retry_exhausted.load(std::memory_order_relaxed);
  return report;
}

void record_parallel_index_profile(const ParallelIndexReport &report) {
  if (!profile::active()) {
    return;
  }
  profile::add_counter("parallel.workers", report.plan.workers);
  profile::add_counter("parallel.max_queue_items", report.plan.max_queue_items);
  profile::add_counter("parallel.max_queue_bytes", report.plan.max_queue_bytes);
  profile::add_counter("parallel.memory_budget_bytes",
                       report.plan.memory_budget_bytes);
  profile::add_counter("parallel.reserved_bytes_per_worker",
                       report.plan.reserved_bytes_per_worker);
  profile::add_counter("parallel.items_dispatched",
                       report.run.items_dispatched);
  profile::add_counter("parallel.items_published", report.run.items_published);
  profile::add_counter("parallel.peak_reorder_items",
                       report.run.peak_reorder_items);
  profile::add_counter("parallel.peak_reorder_bytes",
                       report.run.peak_reorder_bytes);
  profile::add_counter("parallel.peak_reserved_bytes",
                       report.run.peak_reserved_bytes);
  profile::add_counter("parallel.publication_windows",
                       report.run.publication_windows);
  profile::add_counter("parallel.peak_publish_window_items",
                       report.run.peak_publish_window_items);
  profile::add_counter("parallel.peak_publish_window_bytes",
                       report.run.peak_publish_window_bytes);
  profile::add_counter("parallel.header_claim_candidates",
                       report.claims.candidates);
  profile::add_counter("parallel.header_claim_granted", report.claims.granted);
  profile::add_counter("parallel.header_claim_denied_already_indexed",
                       report.claims.denied_already_indexed);
  profile::add_counter("parallel.header_claim_denied_in_flight_owner",
                       report.claims.denied_in_flight_owner);
  profile::add_counter("parallel.header_claim_regranted_on_retry",
                       report.claims.regranted_on_retry);
  profile::add_counter("parallel.header_claim_revoked_after_publish_failure",
                       report.claims.revoked_after_publish_failure);
  profile::add_counter("parallel.source_change_retries", report.retries);
  profile::add_counter("parallel.retry_exhausted", report.retry_exhausted);
  profile::add_counter("parallel.peak_rss_bytes",
                       profile::process_peak_rss_bytes());
  // Published translation units per wall second: the one figure that compares
  // directly across worker counts, since every other counter scales with the
  // pool rather than with delivered work.
  if (report.run.wall_seconds > 0.0) {
    profile::add_timing("parallel.publication_throughput_units_per_second",
                        static_cast<double>(report.run.items_published) /
                            report.run.wall_seconds);
  }
  profile::add_timing("parallel.wall", report.run.wall_seconds);
  profile::add_timing("parallel.worker_active",
                      report.run.total_worker_active_seconds);
  profile::add_timing("parallel.worker_idle",
                      report.run.total_worker_idle_seconds);
  profile::add_timing("parallel.backpressure",
                      report.run.total_backpressure_seconds);
  profile::add_timing("parallel.publish_wait",
                      report.run.total_publish_wait_seconds);
  profile::add_timing("parallel.header_claim_gate_wait",
                      report.claims.total_gate_wait_seconds);
  profile::add_timing("parallel.header_claim_gate_wait_max",
                      report.claims.max_gate_wait_seconds);
}

} // namespace cidx::index
