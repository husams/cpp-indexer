#include "index/parallel_index.hpp"

#include "index/header_claims.hpp"
#include "profile/index_profile.hpp"
#include "storage/fact_batch_writer.hpp"
#include "storage/storage.hpp"

#include <atomic>
#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

namespace cidx::index {
namespace {

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

// Conservative payload accounting for the reorder buffer. The batch itself is
// the dominant term; counting records rather than encoding the artifact keeps
// the budget check off the hot path.
[[nodiscard]] auto approximate_batch_bytes(const ast::IndexOneOutcome &outcome)
    -> std::uint64_t {
  if (!outcome.publication) {
    return 0;
  }
  constexpr std::uint64_t kApproximateBytesPerRecord = 512;
  std::uint64_t records = 0;
  for (const ast::FileFactPartition &partition :
       outcome.publication->batch.partitions()) {
    for (const auto &[family, members] : partition.members) {
      static_cast<void>(family);
      records += members.size();
    }
  }
  return records * kApproximateBytesPerRecord;
}

} // namespace

auto run_parallel_index(cidx::Storage &db, const std::string &index_path,
                        const std::vector<ParallelIndexTarget> &targets,
                        bool graph_enabled, bool no_front_end_reuse,
                        const ParallelBudgets &budgets,
                        const std::function<bool()> &cancelled,
                        const ParallelIndexObserver &observer)
    -> ParallelIndexReport {
  ParallelIndexReport report;
  report.plan =
      plan_parallel_extraction(budgets, probe_host_resources(), targets.size());
  if (targets.empty()) {
    return report;
  }

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
      workers[worker_index] = std::make_unique<ExtractionWorker>(index_path);
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
    const std::uint64_t bytes = approximate_batch_bytes(outcome);
    return ParallelResult<ast::IndexOneOutcome>{
        .payload = std::move(outcome), .bytes = bytes, .error = std::nullopt};
  };

  const auto publish = [&](std::size_t rank,
                           ParallelResult<ast::IndexOneOutcome> &result) {
    const ParallelIndexTarget &target = targets[rank];
    ast::IndexOneOutcome &outcome = result.payload;
    if (result.error) {
      // The worker threw. Report it exactly like a failed parse so the run's
      // failure accounting and diagnostics are the same on both paths.
      outcome.parse_failed = true;
      if (outcome.error.empty()) {
        outcome.error = target.path + ": extraction failed: " + *result.error;
      }
    }
    bool published = false;
    if (!outcome.parse_failed && !outcome.source_changed &&
        outcome.publication) {
      cidx::storage::FactBatchWriter writer(db);
      const cidx::storage::FactBatchPublicationContext context{
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
      cidx::storage::FactBatchWriterResult applied;
      try {
        applied = writer.apply(outcome.publication->batch, context);
      } catch (const std::exception &error) {
        // The writer throws rather than reporting on lock acquisition failure
        // and on schema errors. A publication failure must fail this
        // translation unit, never abort the run: the writer's transaction has
        // already rolled back, so the file simply stays pending.
        applied.error =
            std::string("FactBatch publication failed: ") + error.what();
      }
      if (applied.ok()) {
        published = true;
      } else {
        // A writer or commit failure must not leave a partially committed
        // translation unit: the writer's own transaction rolled back, and the
        // outcome is reported as a failure so the file stays pending.
        //
        // The owned-header grants must be dropped with it. Nothing was written
        // for those headers, so leaving the grants standing would deny them to
        // a later translation unit that would then report them "already" while
        // no row exists -- serially the next unit finds no committed row and
        // indexes them.
        oracle.revoke_grants(rank);
        outcome.parse_failed = true;
        outcome.error = applied.error.value_or("FactBatch publication failed");
        outcome.dependency_facts = {};
      }
    } else if (!outcome.parse_failed && !outcome.source_changed) {
      outcome.parse_failed = true;
      if (outcome.error.empty()) {
        outcome.error = "FactBatch extraction produced no publication batch";
      }
    }
    if (published) {
      ++report.published;
    } else {
      ++report.failed;
    }
    return observer(target, outcome, published);
  };

  report.status = run_parallel_extraction<ast::IndexOneOutcome>(
      report.plan, targets.size(), extract, publish, cancelled,
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
  profile::add_counter("parallel.header_claim_candidates",
                       report.claims.candidates);
  profile::add_counter("parallel.header_claim_granted", report.claims.granted);
  profile::add_counter("parallel.header_claim_denied_already_indexed",
                       report.claims.denied_already_indexed);
  profile::add_counter("parallel.header_claim_denied_in_flight_owner",
                       report.claims.denied_in_flight_owner);
  profile::add_counter("parallel.header_claim_regranted_on_retry",
                       report.claims.regranted_on_retry);
  profile::add_counter("parallel.source_change_retries", report.retries);
  profile::add_counter("parallel.retry_exhausted", report.retry_exhausted);
  profile::add_counter("parallel.peak_rss_bytes",
                       profile::process_peak_rss_bytes());
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
