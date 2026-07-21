#pragma once

#include "functional_calculation/config.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <variant>

namespace functional_calculation {

struct ConfigurationError {
  std::size_t line{};
  std::string message;
};

using ConfigurationResult = std::variant<FeeConfiguration, ConfigurationError>;

// Pure: the returned value depends only on `text`.
ConfigurationResult parse_configuration(std::string_view text);

// Effectful boundary: reads bytes from the filesystem.
class ConfigurationReader {
public:
  [[nodiscard]] ConfigurationResult
  read(const std::filesystem::path &path) const;
};

} // namespace functional_calculation
