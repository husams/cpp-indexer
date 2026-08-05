// One definition of "a bounded numeric CLI option", shared by every parser
// implementation (S-074).
//
// `cidx analyze --jobs` already had the contract: a positive integer, nothing
// else, with one fixed message. `cidx index --jobs` and its budget options must
// reject exactly the same values with exactly the same text, and the project
// runs two independent parsers (the typed application adapter and the CLI11
// grammar). Sharing the predicate here is what keeps them from drifting.
#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cidx::cli {

// The single error phrasing both parsers emit. `option` is the spelling as
// written, e.g. "--jobs".
[[nodiscard]] inline auto positive_integer_error(std::string_view option)
    -> std::string {
  return std::string(option) + " must be a positive integer";
}

// Strictly positive, decimal, no sign, no suffix, no trailing characters.
// Rejects "0", "-1", "4x", " 4", "" and anything that overflows.
[[nodiscard]] inline auto parse_positive_integer(std::string_view value)
    -> std::optional<int> {
  int parsed = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size() ||
      parsed < 1) {
    return std::nullopt;
  }
  return parsed;
}

// Same contract widened to a byte budget, which legitimately exceeds INT_MAX.
[[nodiscard]] inline auto parse_positive_bytes(std::string_view value)
    -> std::optional<std::uint64_t> {
  std::uint64_t parsed = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size() ||
      parsed < 1) {
    return std::nullopt;
  }
  return parsed;
}

} // namespace cidx::cli
