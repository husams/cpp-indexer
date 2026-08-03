#include "ast/owned_header_plan.hpp"

#include "ast/fact_identity.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace cidx::ast {

namespace {

auto candidate_order(const OwnedHeaderRouteCandidate &candidate) {
  return std::tuple(candidate.translation_unit, candidate.discovery_ordinal,
                    candidate.path);
}

auto plan_token(std::string_view generation,
                const std::vector<PlannedFileRoute> &routes) -> std::string {
  std::string material(generation);
  for (const PlannedFileRoute &route : routes) {
    material += '\0';
    material += std::to_string(std::to_underlying(route.role));
    material += '\0';
    material += route.translation_unit;
    material += '\0';
    material += route.path;
    material += '\0';
    material += route.snapshot.md5.value_or("<missing>");
    material += '\0';
    material += route.extraction.partition.stable_string();
  }
  return std::to_string(stable_fact_hash(material));
}

auto planned_route(OwnedHeaderRouteCandidate candidate,
                   CollisionSafeHandleIndex &handles) -> PlannedFileRoute {
  const std::int64_t transient_handle = handles.find_or_insert(
      candidate.translation_unit + '\0' + candidate.path + '\0' +
      candidate.partition.stable_string());
  return {.role = candidate.role,
          .translation_unit = std::move(candidate.translation_unit),
          .translation_unit_file_id = candidate.translation_unit_file_id,
          .path = std::move(candidate.path),
          .discovery_ordinal = candidate.discovery_ordinal,
          .existing_file_id = candidate.existing_file_id,
          .snapshot = std::move(candidate.snapshot),
          .compile_options = std::move(candidate.compile_options),
          .driver = std::move(candidate.driver),
          .cleanup_symbols = candidate.cleanup_symbols,
          .extraction = {.partition = std::move(candidate.partition),
                         .transient_file_handle = transient_handle}};
}

} // namespace

OwnedHeaderRoutePlan::OwnedHeaderRoutePlan()
    : data_(std::make_shared<const Data>()) {}

OwnedHeaderRoutePlan::OwnedHeaderRoutePlan(std::shared_ptr<const Data> data)
    : data_(std::move(data)) {}

auto OwnedHeaderRoutePlan::generation() const -> const std::string & {
  return data_->generation;
}

auto OwnedHeaderRoutePlan::token() const -> const std::string & {
  return data_->token;
}

auto OwnedHeaderRoutePlan::routes() const
    -> const std::vector<PlannedFileRoute> & {
  return data_->routes;
}

auto OwnedHeaderRoutePlan::serial_route(std::string_view translation_unit) const
    -> SerialFactRoute {
  SerialFactRoute result;
  for (const PlannedFileRoute &route : data_->routes) {
    if (route.translation_unit != translation_unit) {
      continue;
    }
    if (route.role == PlannedFileRole::translation_unit) {
      result.main_partition = result.partitions.size();
    }
    result.partitions.push_back(route.extraction);
  }
  return result;
}

auto plan_owned_header_routes(std::string generation,
                              std::vector<OwnedHeaderRouteCandidate> candidates)
    -> OwnedHeaderRoutePlan {
  std::vector<OwnedHeaderRouteCandidate> selected;
  std::map<std::string, OwnedHeaderRouteCandidate> headers;
  for (OwnedHeaderRouteCandidate &candidate : candidates) {
    if (candidate.translation_unit.empty() || candidate.path.empty()) {
      throw std::invalid_argument(
          "owned-header route requires translation-unit and file identities");
    }
    if (candidate.role == PlannedFileRole::translation_unit) {
      selected.push_back(std::move(candidate));
      continue;
    }
    const auto found = headers.find(candidate.path);
    if (found == headers.end() ||
        candidate_order(candidate) < candidate_order(found->second)) {
      headers.insert_or_assign(candidate.path, std::move(candidate));
    }
  }
  for (auto &[path, candidate] : headers) {
    static_cast<void>(path);
    selected.push_back(std::move(candidate));
  }
  std::ranges::sort(selected, [](const OwnedHeaderRouteCandidate &left,
                                 const OwnedHeaderRouteCandidate &right) {
    const auto role_order = [](PlannedFileRole role) {
      return role == PlannedFileRole::translation_unit ? 0 : 1;
    };
    return std::tuple(left.translation_unit, role_order(left.role),
                      left.discovery_ordinal, left.path) <
           std::tuple(right.translation_unit, role_order(right.role),
                      right.discovery_ordinal, right.path);
  });

  CollisionSafeHandleIndex handles;
  std::vector<PlannedFileRoute> routes;
  routes.reserve(selected.size());
  for (OwnedHeaderRouteCandidate &candidate : selected) {
    routes.push_back(planned_route(std::move(candidate), handles));
  }
  auto data = std::make_shared<OwnedHeaderRoutePlan::Data>();
  data->generation = std::move(generation);
  data->routes = std::move(routes);
  data->token = plan_token(data->generation, data->routes);
  return OwnedHeaderRoutePlan(std::move(data));
}

} // namespace cidx::ast
