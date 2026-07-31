#include "application/agent_tools.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "query/exec.hpp"
#include "storage/storage.hpp"
#include "util/pathutil.hpp"
#include "workspace/context.hpp"

namespace {

std::string default_index_path() {
  if (const char *cache = std::getenv("INDEXER_CACHE");
      cache != nullptr && *cache != '\0') {
    return cidx::pathutil::join(cache, "index.db");
  }
  return cidx::pathutil::expanduser("~/.cache/cidx/index.db");
}

std::string parse_index_path(int argc, char **argv) {
  std::string path = default_index_path();
  for (int i = 1; i < argc; ++i) {
    const std::string argument(argv[i]);
    if (argument == "--help" || argument == "-h") {
      std::cout << "Usage: cidx-agent [--index PATH]\n"
                   "Reads cidx.agent/v1 JSON requests from stdin and writes "
                   "one response per line.\n";
      std::exit(0);
    }
    if (argument == "--index" && i + 1 < argc) {
      path = argv[++i];
      continue;
    }
    throw std::invalid_argument("unknown or incomplete option: " + argument);
  }
  return path;
}

} // namespace

int main(int argc, char **argv) {
  try {
    cidx::Storage db(parse_index_path(argc, argv),
                     cidx::Storage::OpenMode::read_only);
    cidx::StorageWorkspaceAdapter workspace_data(db);
    cidx::WorkspaceContext workspace = cidx::WorkspaceContext::borrow(
        workspace_data, cidx::WorkspaceReadWriteMode::read_only);
    cidx::query::SqliteQueryReadAdapter query_read(db);
    cidx::application::ApplicationReadPorts read_ports{.query = &query_read};
    cidx::application::ApplicationContext context(
        workspace,
        cidx::application::ApplicationPolicy{
            .access = cidx::application::AccessMode::read_only,
            .capabilities = cidx::application::capability_bit(
                cidx::application::Capability::index_read)},
        read_ports);
    std::string line;
    while (std::getline(std::cin, line)) {
      std::cout << cidx::json_out::dumps_compact(
                       cidx::agent::ToolService::invoke_json(line, context))
                << '\n';
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
