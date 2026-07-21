#include "functional_calculation/calculation.hpp"

#include <algorithm>
#include <limits>

namespace functional_calculation {
namespace {

constexpr Money basis_points_denominator = 10'000;

bool addition_overflows(Money left, Money right) {
  return right > 0 && left > std::numeric_limits<Money>::max() - right;
}

} // namespace

std::string_view describe(CalculationError error) {
  switch (error) {
  case CalculationError::negative_transfer_amount:
    return "transfer amount must not be negative";
  case CalculationError::negative_fixed_fee:
    return "fixed fee must not be negative";
  case CalculationError::rate_out_of_range:
    return "variable rate must be between 0 and 10000 basis points";
  case CalculationError::negative_maximum_fee:
    return "maximum fee must not be negative";
  case CalculationError::arithmetic_overflow:
    return "calculation exceeds the supported money range";
  }
  return "unknown calculation error";
}

MoneyResult calculate_percentage_fee(Money transfer_amount_cents,
                                     BasisPoints rate_basis_points) {
  if (transfer_amount_cents < 0) {
    return CalculationError::negative_transfer_amount;
  }
  if (rate_basis_points < 0 || rate_basis_points > basis_points_denominator) {
    return CalculationError::rate_out_of_range;
  }

  const auto rate = static_cast<Money>(rate_basis_points);
  if (rate != 0 &&
      transfer_amount_cents > std::numeric_limits<Money>::max() / rate) {
    return CalculationError::arithmetic_overflow;
  }
  return transfer_amount_cents * rate / basis_points_denominator;
}

Money apply_fee_cap(Money fee_cents, std::optional<Money> maximum_fee_cents) {
  return maximum_fee_cents ? std::min(fee_cents, *maximum_fee_cents)
                           : fee_cents;
}

CalculationOutcome
FeeCalculator::calculate(const FeeConfiguration &configuration,
                         const CalculationInput &input) const {
  if (input.transfer_amount_cents < 0) {
    return CalculationError::negative_transfer_amount;
  }
  if (configuration.fixed_fee_cents < 0) {
    return CalculationError::negative_fixed_fee;
  }
  if (configuration.variable_rate_basis_points < 0 ||
      configuration.variable_rate_basis_points > basis_points_denominator) {
    return CalculationError::rate_out_of_range;
  }
  if (configuration.maximum_fee_cents && *configuration.maximum_fee_cents < 0) {
    return CalculationError::negative_maximum_fee;
  }

  const auto percentage = calculate_percentage_fee(
      input.transfer_amount_cents, configuration.variable_rate_basis_points);
  if (const auto *calculation_error =
          std::get_if<CalculationError>(&percentage)) {
    return *calculation_error;
  }

  const auto percentage_fee = std::get<Money>(percentage);
  if (addition_overflows(percentage_fee, configuration.fixed_fee_cents)) {
    return CalculationError::arithmetic_overflow;
  }

  const auto uncapped_fee = percentage_fee + configuration.fixed_fee_cents;
  const auto fee = apply_fee_cap(uncapped_fee, configuration.maximum_fee_cents);
  if (addition_overflows(input.transfer_amount_cents, fee)) {
    return CalculationError::arithmetic_overflow;
  }

  return CalculationResult{
      .percentage_fee_cents = percentage_fee,
      .fee_cents = fee,
      .total_debit_cents = input.transfer_amount_cents + fee,
  };
}

} // namespace functional_calculation
