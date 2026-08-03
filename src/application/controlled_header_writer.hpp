// Controlled writer for planned file-row and stale-symbol lifecycle mutation.
#pragma once

#include "ast/owned_header_plan.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace cidx::storage {
class SourceStoreWritePort;
class SymbolWritePort;
} // namespace cidx::storage

namespace cidx::application {

enum class HeaderPlanRejectionKind : std::uint8_t {
  generation_mismatch,
  stale_source,
  invalid_route,
};

struct HeaderPlanRejection {
  HeaderPlanRejectionKind kind = HeaderPlanRejectionKind::invalid_route;
  std::string path;
  std::string detail;
};

struct ControlledHeaderWriteResult {
  std::map<std::int64_t, std::int64_t> file_ids;
  std::optional<HeaderPlanRejection> rejection;

  [[nodiscard]] auto ok() const -> bool { return !rejection.has_value(); }
};

using PlannedSourceValidator = std::function<bool(
    const std::string &, const ast::PlannedSourceSnapshot &)>;

class ControlledHeaderWriter {
public:
  ControlledHeaderWriter(storage::SourceStoreWritePort &source,
                         storage::SymbolWritePort &symbols);

  [[nodiscard]] auto apply(const ast::OwnedHeaderRoutePlan &plan,
                           std::string_view translation_unit,
                           std::string_view expected_generation,
                           const PlannedSourceValidator &source_is_current)
      -> ControlledHeaderWriteResult;

private:
  storage::SourceStoreWritePort *source_;
  storage::SymbolWritePort *symbols_;
};

} // namespace cidx::application
