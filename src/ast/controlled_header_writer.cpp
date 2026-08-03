#include "ast/controlled_header_writer.hpp"

#include <utility>

namespace cidx::ast {

ControlledHeaderWriter::ControlledHeaderWriter(
    PlannedFileRowWriter write_file_row, PlannedSymbolCleanup cleanup_symbols)
    : write_file_row_(std::move(write_file_row)),
      cleanup_symbols_(std::move(cleanup_symbols)) {}

auto ControlledHeaderWriter::apply(
    const OwnedHeaderRoutePlan &plan, std::string_view translation_unit,
    std::string_view expected_generation,
    const PlannedSourceValidator &source_is_current)
    -> ControlledHeaderWriteResult {
  if (plan.generation() != expected_generation) {
    return {.file_ids = {},
            .rejection = HeaderPlanRejection{
                .kind = HeaderPlanRejectionKind::generation_mismatch,
                .path = {},
                .detail = "owned-header plan generation is stale"}};
  }

  for (const PlannedFileRoute &route : plan.routes()) {
    if (route.translation_unit != translation_unit) {
      continue;
    }
    if (!source_is_current(route.path, route.snapshot)) {
      return {
          .file_ids = {},
          .rejection = HeaderPlanRejection{
              .kind = HeaderPlanRejectionKind::stale_source,
              .path = route.path,
              .detail = "planned source snapshot changed before publication"}};
    }
    if (route.role == PlannedFileRole::translation_unit &&
        !route.existing_file_id.has_value()) {
      return {.file_ids = {},
              .rejection = HeaderPlanRejection{
                  .kind = HeaderPlanRejectionKind::invalid_route,
                  .path = route.path,
                  .detail = "translation-unit route has no existing file row"}};
    }
  }

  ControlledHeaderWriteResult result;
  for (const PlannedFileRoute &route : plan.routes()) {
    if (route.translation_unit != translation_unit) {
      continue;
    }
    const std::int64_t file_id = route.existing_file_id
                                     ? *route.existing_file_id
                                     : write_file_row_(route);
    if (route.cleanup_symbols) {
      cleanup_symbols_(file_id);
    }
    if (route.extraction.transient_file_handle) {
      result.file_ids.emplace(*route.extraction.transient_file_handle, file_id);
    }
  }
  return result;
}

} // namespace cidx::ast
