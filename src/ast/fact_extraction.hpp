// Pure serial extraction boundary: frontend session in, immutable batch out.
#pragma once

#include "ast/fact_batch.hpp"
#include "ast/pass_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace cidx::ast {

struct FactRoutePartition {
  FactPartitionKey partition;
  std::optional<std::int64_t> transient_file_handle;
};

struct SerialFactRoute {
  std::vector<FactRoutePartition> partitions;
  std::size_t main_partition = 0;
};

enum class FactExtractionFailureKind : std::uint8_t {
  invalid_route,
  pass_failed,
  budget_exceeded,
  prepublication_failed,
};

struct FactExtractionFailure {
  FactExtractionFailureKind kind = FactExtractionFailureKind::pass_failed;
  std::string pass;
  std::string detail;
};

struct SerialFactExtractionResult {
  std::optional<FactBatch> batch;
  PassExecutionReport report;
  std::optional<FactExtractionFailure> failure;

  [[nodiscard]] auto ok() const -> bool {
    return batch.has_value() && !failure.has_value();
  }
};

using PrepublicationProbe = std::function<void(const FactBatchRecorder &)>;

[[nodiscard]] auto extract_serial_fact_batch(
    FrontendSession session, const ExtractionPassRegistry &registry,
    const IndexingPlan &plan, const SerialFactRoute &route,
    const PrepublicationProbe &before_publication = {})
    -> SerialFactExtractionResult;

} // namespace cidx::ast
