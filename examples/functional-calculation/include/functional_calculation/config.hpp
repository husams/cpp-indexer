#pragma once

#include <cstdint>
#include <optional>

namespace functional_calculation {

using Money = std::int64_t;
using BasisPoints = std::int32_t;

struct FeeConfiguration {
  Money fixed_fee_cents{};
  BasisPoints variable_rate_basis_points{};
  std::optional<Money> maximum_fee_cents;

  friend bool operator==(const FeeConfiguration &,
                         const FeeConfiguration &) = default;
};

} // namespace functional_calculation
