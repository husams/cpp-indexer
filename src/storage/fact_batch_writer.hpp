// Deterministic, set-based publication of one immutable translation-unit batch.
#pragma once

#include "ast/fact_batch.hpp"
#include "ast/owned_header_plan.hpp"
#include "storage/records.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

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
  std::uint64_t inserted = 0;
  std::uint64_t updated = 0;
  std::uint64_t ignored = 0;
  std::uint64_t deleted = 0;
};

struct FactBatchWriterReport {
  std::uint64_t statements_prepared = 0;
  std::uint64_t statements_reused = 0;
  std::uint64_t statements_eliminated = 0;
  std::uint64_t statement_executions = 0;
  std::uint64_t virtual_machine_steps = 0;
  double prepare_seconds = 0.0;
  double virtual_machine_seconds = 0.0;
  double commit_seconds = 0.0;
  bool commit_attempted = false;
  bool committed = false;
  std::map<ast::FactFamily, FactBatchWriterRows> families;
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

class FactBatchWriter {
public:
  explicit FactBatchWriter(cidx::SqliteStorageService &storage);

  [[nodiscard]] auto apply(const ast::FactBatch &batch,
                           const FactBatchPublicationContext &context)
      -> FactBatchWriterResult;

private:
  cidx::SqliteStorageService &storage_;
};

} // namespace cidx::storage
