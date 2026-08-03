#include "application/controlled_header_writer.hpp"

#include "storage/ports.hpp"

#include <utility>

namespace cidx::application {

ControlledHeaderWriter::ControlledHeaderWriter(
    storage::SourceStoreWritePort &source, storage::SymbolWritePort &symbols)
    : source_(&source), symbols_(&symbols) {}

auto ControlledHeaderWriter::apply(
    const ast::OwnedHeaderRoutePlan &plan, std::string_view translation_unit,
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

  for (const ast::PlannedFileRoute &route : plan.routes()) {
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
    if (route.role == ast::PlannedFileRole::translation_unit &&
        !route.existing_file_id.has_value()) {
      return {.file_ids = {},
              .rejection = HeaderPlanRejection{
                  .kind = HeaderPlanRejectionKind::invalid_route,
                  .path = route.path,
                  .detail = "translation-unit route has no existing file row"}};
    }
  }

  ControlledHeaderWriteResult result;
  for (const ast::PlannedFileRoute &route : plan.routes()) {
    if (route.translation_unit != translation_unit) {
      continue;
    }
    const std::int64_t file_id =
        route.existing_file_id
            ? *route.existing_file_id
            : source_->add_file_path(route.path, route.snapshot.mtime,
                                     route.snapshot.md5, route.compile_options,
                                     route.driver);
    if (route.cleanup_symbols) {
      symbols_->delete_symbols_for_file(file_id);
    }
    if (route.extraction.transient_file_handle) {
      result.file_ids.emplace(*route.extraction.transient_file_handle, file_id);
    }
  }
  return result;
}

} // namespace cidx::application
