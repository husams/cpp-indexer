#pragma once

#include "functional_calculation/config.hpp"

#include <string_view>
#include <variant>

namespace functional_calculation {

struct CalculationInput {
  Money transfer_amount_cents{};
};

struct CalculationResult {
  Money percentage_fee_cents{};
  Money fee_cents{};
  Money total_debit_cents{};

  friend bool operator==(const CalculationResult &,
                         const CalculationResult &) = default;
};

enum class CalculationError {
  negative_transfer_amount,
  negative_fixed_fee,
  rate_out_of_range,
  negative_maximum_fee,
  arithmetic_overflow,
};

using MoneyResult = std::variant<Money, CalculationError>;
using CalculationOutcome = std::variant<CalculationResult, CalculationError>;

[[nodiscard]] std::string_view describe(CalculationError error);

// The functions below form the pure calculation core. They do not read files,
// mutate shared state, throw exceptions, or depend on hidden inputs.
[[nodiscard]] MoneyResult
calculate_percentage_fee(Money transfer_amount_cents,
                         BasisPoints rate_basis_points);

[[nodiscard]] Money apply_fee_cap(Money fee_cents,
                                  std::optional<Money> maximum_fee_cents);

class FeeCalculator {
public:
  [[nodiscard]] CalculationOutcome
  calculate(const FeeConfiguration &configuration,
            const CalculationInput &input) const;
};

} // namespace functional_calculation
