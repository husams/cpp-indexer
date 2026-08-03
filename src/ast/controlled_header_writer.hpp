// Controlled writer for planned file-row and stale-symbol lifecycle mutation.
#pragma once

#include "ast/owned_header_plan.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace cidx::ast {

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

using PlannedSourceValidator =
    std::function<bool(const std::string &, const PlannedSourceSnapshot &)>;
using PlannedFileRowWriter =
    std::function<std::int64_t(const PlannedFileRoute &)>;
using PlannedSymbolCleanup = std::function<void(std::int64_t)>;

class ControlledHeaderWriter {
public:
  ControlledHeaderWriter(PlannedFileRowWriter write_file_row,
                         PlannedSymbolCleanup cleanup_symbols);

  [[nodiscard]] auto apply(const OwnedHeaderRoutePlan &plan,
                           std::string_view translation_unit,
                           std::string_view expected_generation,
                           const PlannedSourceValidator &source_is_current)
      -> ControlledHeaderWriteResult;

private:
  PlannedFileRowWriter write_file_row_;
  PlannedSymbolCleanup cleanup_symbols_;
};

} // namespace cidx::ast
