// Immutable extraction-side routing identities for one translation unit.
#pragma once

#include "ast/fact_identity.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
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

} // namespace cidx::ast
