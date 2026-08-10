// Deterministic, set-based publication of one immutable translation-unit batch.
#pragma once

#include "ast/fact_batch.hpp"
#include "ast/owned_header_plan.hpp"
#include "storage/records.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cidx {
class SqliteStorageService;
}

namespace cidx::storage {

enum class FactBatchWriterPhase : std::uint8_t {
  validate_plan,
  resolve_file_rows,
  load_temporary_staging,
  resolve_natural_keys,
  apply_entities,
  apply_annotations,
  apply_relations,
  apply_sites_and_external_identities,
  publish_includes_and_applicability,
  cleanup_stale_facts,
  revalidate_sources,
  mark_current,
  commit,
};

inline constexpr std::array kFactBatchWriterPhaseOrder{
    FactBatchWriterPhase::validate_plan,
    FactBatchWriterPhase::resolve_file_rows,
    FactBatchWriterPhase::load_temporary_staging,
    FactBatchWriterPhase::resolve_natural_keys,
    FactBatchWriterPhase::apply_entities,
    FactBatchWriterPhase::apply_annotations,
    FactBatchWriterPhase::apply_relations,
    FactBatchWriterPhase::apply_sites_and_external_identities,
    FactBatchWriterPhase::publish_includes_and_applicability,
    FactBatchWriterPhase::cleanup_stale_facts,
    FactBatchWriterPhase::revalidate_sources,
    FactBatchWriterPhase::mark_current,
    FactBatchWriterPhase::commit,
};

[[nodiscard]] auto fact_batch_writer_phase_name(FactBatchWriterPhase phase)
    -> std::string_view;

struct FactBatchWriterRows {
  std::uint64_t staged = 0;
  std::uint64_t coalesced = 0;
  std::uint64_t inserted = 0;
  std::uint64_t updated = 0;
  std::uint64_t ignored = 0;
  std::uint64_t deleted = 0;
};

struct FactBatchWriterApplicability {
  std::uint64_t attempted = 0;
  std::uint64_t unique = 0;
  std::uint64_t virtual_machine_steps = 0;
  std::uint64_t fullscan_steps = 0;
  double seconds = 0.0;
};

struct FactBatchWriterReport {
  std::uint64_t windows_started = 0;
  std::uint64_t windows_committed = 0;
  std::uint64_t windows_rolled_back = 0;
  std::uint64_t translation_units_replayed = 0;
  std::uint64_t window_items = 0;
  std::uint64_t window_bytes = 0;
  std::uint64_t transactions_started = 0;
  std::uint64_t temporary_tables_checked = 0;
  std::uint64_t temporary_rows_cleared = 0;
  std::uint64_t statements_prepared = 0;
  std::uint64_t statements_reused = 0;
  // Statements eliminated relative to the replaced row-at-a-time path is not
  // observable from inside the writer: it is a difference against a baseline
  // build. It is reported by benchmarks/indexing/production.py as
  // baseline prepare_calls - candidate prepare_calls, never synthesized here.
  std::uint64_t statement_executions = 0;
  std::uint64_t virtual_machine_steps = 0;
  std::uint64_t fullscan_steps = 0;
  std::uint64_t include_fullscan_steps = 0;
  std::uint64_t applicability_fullscan_steps = 0;
  double prepare_seconds = 0.0;
  double virtual_machine_seconds = 0.0;
  double transaction_begin_seconds = 0.0;
  double temporary_schema_seconds = 0.0;
  double temporary_clear_seconds = 0.0;
  double staging_seconds = 0.0;
  double classification_seconds = 0.0;
  double include_seconds = 0.0;
  double applicability_seconds = 0.0;
  double apply_seconds = 0.0;
  double commit_seconds = 0.0;
  bool commit_attempted = false;
  bool committed = false;
  std::map<ast::FactFamily, FactBatchWriterRows> families;
  std::map<std::string, FactBatchWriterApplicability, std::less<>>
      applicability;
  std::map<FactBatchWriterPhase, double> phase_seconds;
  std::map<FactBatchWriterPhase, std::uint64_t> phase_fullscan_steps;
};

enum class FactBatchWriterFailurePoint : std::uint8_t {
  none,
  temporary_load,
  natural_key_resolution,
  entity_apply,
  annotation_apply,
  relation_apply,
  site_apply,
  publication,
  cleanup,
  before_commit,
  commit,
};

struct FactBatchPublicationContext {
  ast::OwnedHeaderRoutePlan route_plan;
  std::string translation_unit;
  std::string expected_generation;
  ast::PlannedSourceValidator source_is_current;
  std::int64_t configuration_id = -1;
  std::optional<cidx::TranslationUnitConfig> configuration;
  FactBatchWriterFailurePoint failure = FactBatchWriterFailurePoint::none;
  // Opt in to per-step VM-step/timing sampling for this publication. Off by
  // default so an ordinary index run pays no measurement cost; an active
  // profiling session enables sampling independently of this flag.
  bool measure_statements = false;
};

struct FactBatchWriterResult {
  FactBatchWriterReport report;
  std::map<std::int64_t, std::int64_t> file_ids;
  std::map<std::int64_t, std::int64_t> symbol_ids;
  std::map<std::int64_t, std::int64_t> relation_ids;
  std::map<std::int64_t, std::int64_t> type_ids;
  std::map<std::int64_t, std::int64_t> definition_ids;
  std::int64_t configuration_id = -1;
  std::optional<std::string> error;

  [[nodiscard]] auto ok() const -> bool {
    return report.committed && !error.has_value();
  }
};

struct FactBatchWriterWindowItem {
  const ast::FactBatch *batch = nullptr;
  FactBatchPublicationContext context;
  std::uint64_t approximate_bytes = 0;
};

struct FactBatchWriterWindowResult {
  std::vector<FactBatchWriterResult> results;
  FactBatchWriterReport report;
  bool replayed = false;
  std::optional<std::string> speculative_error;

  [[nodiscard]] auto ok() const -> bool {
    return std::ranges::all_of(results, &FactBatchWriterResult::ok);
  }
};

enum class FactBatchWriterWindowMode : std::uint8_t {
  speculative,
  replay_only,
};

class FactBatchWriter {
public:
  explicit FactBatchWriter(cidx::SqliteStorageService &storage);

  [[nodiscard]] auto apply(const ast::FactBatch &batch,
                           const FactBatchPublicationContext &context)
      -> FactBatchWriterResult;

  // Apply a bounded consecutive-rank window in one transaction. Any failure
  // rolls the whole speculative window back, then replays every item through
  // apply() with failure injection disabled so the durable result has exactly
  // the established one-translation-unit transaction semantics.
  [[nodiscard]] auto apply_window(
      std::span<const FactBatchWriterWindowItem> items,
      const std::function<bool()> &cancelled = {},
      FactBatchWriterWindowMode mode = FactBatchWriterWindowMode::speculative)
      -> FactBatchWriterWindowResult;

private:
  enum class TemporaryRowPolicy : std::uint8_t { per_item, shared_window };

  [[nodiscard]] auto
  apply_in_transaction(const ast::FactBatch &batch,
                       const FactBatchPublicationContext &context,
                       TemporaryRowPolicy temporary_rows,
                       std::size_t window_ordinal = 0) -> FactBatchWriterResult;

  cidx::SqliteStorageService &storage_;
};

} // namespace cidx::storage
