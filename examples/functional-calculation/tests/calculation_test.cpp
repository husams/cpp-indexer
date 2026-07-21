#include "functional_calculation/calculation.hpp"
#include "functional_calculation/config_reader.hpp"

#include <cassert>
#include <limits>
#include <variant>

namespace fc = functional_calculation;

int main() {
  const auto parsed = fc::parse_configuration("fixed_fee_cents=25\n"
                                              "variable_rate_basis_points=150\n"
                                              "maximum_fee_cents=500\n");
  assert(std::holds_alternative<fc::FeeConfiguration>(parsed));

  const auto configuration = std::get<fc::FeeConfiguration>(parsed);
  const auto outcome = fc::FeeCalculator{}.calculate(
      configuration, fc::CalculationInput{.transfer_amount_cents = 10'000});
  assert(std::holds_alternative<fc::CalculationResult>(outcome));
  assert((std::get<fc::CalculationResult>(outcome) ==
          fc::CalculationResult{
              .percentage_fee_cents = 150,
              .fee_cents = 175,
              .total_debit_cents = 10'175,
          }));

  const auto capped = fc::FeeCalculator{}.calculate(
      configuration, fc::CalculationInput{.transfer_amount_cents = 100'000});
  assert(std::holds_alternative<fc::CalculationResult>(capped));
  assert(std::get<fc::CalculationResult>(capped).fee_cents == 500);

  const auto negative = fc::FeeCalculator{}.calculate(
      configuration, fc::CalculationInput{.transfer_amount_cents = -1});
  assert(std::get<fc::CalculationError>(negative) ==
         fc::CalculationError::negative_transfer_amount);

  const fc::FeeConfiguration overflow_configuration{
      .fixed_fee_cents = 0,
      .variable_rate_basis_points = 10'000,
      .maximum_fee_cents = std::nullopt,
  };
  const auto overflow = fc::FeeCalculator{}.calculate(
      overflow_configuration,
      fc::CalculationInput{
          .transfer_amount_cents = std::numeric_limits<fc::Money>::max(),
      });
  assert(std::get<fc::CalculationError>(overflow) ==
         fc::CalculationError::arithmetic_overflow);
}
