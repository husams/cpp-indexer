#include "util/front_end_reuse.hpp"

#include "util/files.hpp"
#include "util/pathutil.hpp"

#include <filesystem>
#include <fstream>
#include <string_view>

namespace cidx {

namespace {

std::string resolve_input(const std::string &input,
                          const std::optional<std::string> &working_dir) {
  if (pathutil::isabs(input)) {
    return pathutil::normpath(input);
  }
  return pathutil::abspath(
      pathutil::join(working_dir.value_or(pathutil::getcwd()), input));
}

} // namespace

std::optional<std::string>
preflight_build_declared_pch(const TranslationUnitConfig &configuration) {
  const auto &arguments = configuration.arguments;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    std::string input;
    if (arguments[index] == "-include-pch") {
      if (index + 1 >= arguments.size()) {
        return "build-declared PCH input is missing after -include-pch";
      }
      input = arguments[++index];
    } else if (arguments[index].starts_with("-include-pch=")) {
      input = arguments[index].substr(std::string_view("-include-pch=").size());
    } else {
      continue;
    }
    const std::string path = resolve_input(input, configuration.working_dir);
    if (!files::is_regular_file(path)) {
      return "build-declared PCH input is missing or unreadable: " + path;
    }
    const auto permissions = std::filesystem::status(path).permissions();
    const auto read_permissions = std::filesystem::perms::owner_read |
                                  std::filesystem::perms::group_read |
                                  std::filesystem::perms::others_read;
    if ((permissions & read_permissions) == std::filesystem::perms::none) {
      return "build-declared PCH input is missing or unreadable: " + path;
    }
    const std::ifstream input_file(path, std::ios::in | std::ios::binary);
    if (!input_file.is_open()) {
      return "build-declared PCH input is missing or unreadable: " + path;
    }
  }
  return std::nullopt;
}

} // namespace cidx
