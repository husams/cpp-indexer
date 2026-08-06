#include "index/parallel_index.hpp"

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
      // This scheduler owns its own writer, so it also owns publishing that
      // writer's telemetry. Without this the default (parallel) configuration
      // reports no `fact_batch_writer.*` counters or timings at all, and the
      // production measurement gate -- which requires them -- cannot run
      // against the mode that actually ships.
      ast::record_writer_profile(applied.report, profile::active());
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
