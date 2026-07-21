#include "functional_calculation/config_reader.hpp"

#include <charconv>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_set>

namespace functional_calculation {
namespace {

std::string_view trim(std::string_view value) {
  const auto first = value.find_first_not_of(" \t\r");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(" \t\r");
  return value.substr(first, last - first + 1);
}

template <typename Integer>
std::optional<Integer> parse_integer(std::string_view value) {
  Integer parsed{};
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size()) {
    return std::nullopt;
  }
  return parsed;
}

ConfigurationError error(std::size_t line, std::string message) {
  return ConfigurationError{line, std::move(message)};
}

} // namespace

ConfigurationResult parse_configuration(std::string_view text) {
  FeeConfiguration configuration;
  bool has_fixed_fee = false;
  bool has_rate = false;
  std::unordered_set<std::string> keys;

  std::size_t line_number = 0;
  while (!text.empty()) {
    ++line_number;
    const auto newline = text.find('\n');
    auto line = trim(text.substr(0, newline));
    text = newline == std::string_view::npos ? std::string_view{}
                                             : text.substr(newline + 1);

    if (line.empty() || line.starts_with('#')) {
      continue;
    }

    const auto separator = line.find('=');
    if (separator == std::string_view::npos) {
      return error(line_number, "expected key=value");
    }

    const auto key = trim(line.substr(0, separator));
    const auto value = trim(line.substr(separator + 1));
    if (key.empty() || value.empty()) {
      return error(line_number, "key and value must not be empty");
    }
    if (!keys.emplace(key).second) {
      return error(line_number, "duplicate key: " + std::string(key));
    }

    if (key == "fixed_fee_cents") {
      const auto parsed = parse_integer<Money>(value);
      if (!parsed) {
        return error(line_number, "fixed_fee_cents must be an integer");
      }
      configuration.fixed_fee_cents = *parsed;
      has_fixed_fee = true;
    } else if (key == "variable_rate_basis_points") {
      const auto parsed = parse_integer<BasisPoints>(value);
      if (!parsed) {
        return error(line_number,
                     "variable_rate_basis_points must be an integer");
      }
      configuration.variable_rate_basis_points = *parsed;
      has_rate = true;
    } else if (key == "maximum_fee_cents") {
      const auto parsed = parse_integer<Money>(value);
      if (!parsed) {
        return error(line_number, "maximum_fee_cents must be an integer");
      }
      configuration.maximum_fee_cents = *parsed;
    } else {
      return error(line_number, "unknown key: " + std::string(key));
    }
  }

  if (!has_fixed_fee) {
    return error(0, "missing fixed_fee_cents");
  }
  if (!has_rate) {
    return error(0, "missing variable_rate_basis_points");
  }
  return configuration;
}

ConfigurationResult
ConfigurationReader::read(const std::filesystem::path &path) const {
  std::ifstream input(path);
  if (!input) {
    return error(0, "cannot open configuration file");
  }

  const std::string text{std::istreambuf_iterator<char>{input},
                         std::istreambuf_iterator<char>{}};
  if (!input.eof() && input.fail()) {
    return error(0, "cannot read configuration file");
  }
  return parse_configuration(text);
}

} // namespace functional_calculation
