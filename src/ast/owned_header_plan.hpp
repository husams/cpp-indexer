// Scheduler-owned, immutable file and configuration routing plan.
#pragma once

#include "ast/fact_route.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cidx::ast {

enum class PlannedFileRole : std::uint8_t {
  translation_unit,
  owned_header,
};

struct PlannedSourceSnapshot {
  std::optional<double> mtime;
  std::optional<std::string> md5;
};

struct OwnedHeaderRouteCandidate {
  PlannedFileRole role = PlannedFileRole::owned_header;
  std::string translation_unit;
  std::int64_t translation_unit_file_id = -1;
  std::string path;
  std::size_t discovery_ordinal = 0;
  std::optional<std::int64_t> existing_file_id;
  PlannedSourceSnapshot snapshot;
  std::optional<std::vector<std::string>> compile_options;
  std::optional<std::string> driver;
  bool cleanup_symbols = false;
  FactPartitionKey partition;
};

struct PlannedFileRoute {
  PlannedFileRole role = PlannedFileRole::owned_header;
  std::string translation_unit;
  std::int64_t translation_unit_file_id = -1;
  std::string path;
  std::size_t discovery_ordinal = 0;
  std::optional<std::int64_t> existing_file_id;
  PlannedSourceSnapshot snapshot;
  std::optional<std::vector<std::string>> compile_options;
  std::optional<std::string> driver;
  bool cleanup_symbols = false;
  FactRoutePartition extraction;
};

class OwnedHeaderRoutePlan {
public:
  OwnedHeaderRoutePlan();

  [[nodiscard]] auto generation() const -> const std::string &;
  [[nodiscard]] auto token() const -> const std::string &;
  [[nodiscard]] auto routes() const -> const std::vector<PlannedFileRoute> &;
  [[nodiscard]] auto serial_route(std::string_view translation_unit) const
      -> SerialFactRoute;

private:
  struct Data {
    std::string generation;
    std::string token;
    std::vector<PlannedFileRoute> routes;
  };

  explicit OwnedHeaderRoutePlan(std::shared_ptr<const Data> data);
  std::shared_ptr<const Data> data_;

  friend auto
  plan_owned_header_routes(std::string generation,
                           std::vector<OwnedHeaderRouteCandidate> candidates)
      -> OwnedHeaderRoutePlan;
};

[[nodiscard]] auto
plan_owned_header_routes(std::string generation,
                         std::vector<OwnedHeaderRouteCandidate> candidates)
    -> OwnedHeaderRoutePlan;

} // namespace cidx::ast
