#include "functional_calculation/calculation.hpp"
#include "functional_calculation/config_reader.hpp"

#include <charconv>
#include <iostream>
#include <string_view>

namespace fc = functional_calculation;

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cerr << "usage: functional-calculation CONFIG AMOUNT_CENTS\n";
    return 2;
  }

  const auto configuration = fc::ConfigurationReader{}.read(argv[1]);
  if (const auto *error = std::get_if<fc::ConfigurationError>(&configuration)) {
    std::cerr << "configuration error";
    if (error->line != 0) {
      std::cerr << " on line " << error->line;
    }
    std::cerr << ": " << error->message << '\n';
    return 1;
  }

  fc::Money amount{};
  const std::string_view amount_text{argv[2]};
  const auto [end, parse_error] = std::from_chars(
      amount_text.data(), amount_text.data() + amount_text.size(), amount);
  if (parse_error != std::errc{} ||
      end != amount_text.data() + amount_text.size()) {
    std::cerr << "amount must be an integer number of cents\n";
    return 2;
  }

  const auto outcome = fc::FeeCalculator{}.calculate(
      std::get<fc::FeeConfiguration>(configuration),
      fc::CalculationInput{.transfer_amount_cents = amount});
  if (const auto *error = std::get_if<fc::CalculationError>(&outcome)) {
    std::cerr << "calculation error: " << fc::describe(*error) << '\n';
    return 1;
  }

  const auto &result = std::get<fc::CalculationResult>(outcome);
  std::cout << "percentage_fee_cents=" << result.percentage_fee_cents << '\n'
            << "fee_cents=" << result.fee_cents << '\n'
            << "total_debit_cents=" << result.total_debit_cents << '\n';
}
