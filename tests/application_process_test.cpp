#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "storage/storage.hpp"
#include "util/subprocess.hpp"

namespace {

std::string g_cidx_binary;

TEST_CASE("typed process path preserves analysis list and read-only artifact "
          "policy") {
  const std::filesystem::path cache =
      std::filesystem::temp_directory_path() / "cidx-application-process";
  std::error_code ec;
  std::filesystem::remove_all(cache, ec);
  ::setenv("INDEXER_CACHE", cache.c_str(), 1);

  const auto list = cidx::run({g_cidx_binary, "analyze", "--list"});
  REQUIRE(list.exit_code == 0);
  CHECK(list.out ==
        "{\n"
        "  \"rules\": [\n"
        "    {\n"
        "      \"name\": \"callgraph\",\n"
        "      \"description\": \"direct and transitive call graph (outputs: "
        "call, call_transitive)\"\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"cycles\",\n"
        "      \"description\": \"call cycles over calls/dispatch_calls edges "
        "(outputs: cycle_member, cycle_edge)\"\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"unused\",\n"
        "      \"description\": \"defined functions with no incoming call or "
        "override edge (outputs: unused)\"\n"
        "    }\n"
        "  ]\n"
        "}\n");
  CHECK_FALSE(std::filesystem::exists(cache));

  const auto query = cidx::run({g_cidx_binary, "query", "nodes()", "--json"});
  CHECK(query.exit_code == 1);
  CHECK_FALSE(std::filesystem::exists(cache));

  const auto version = cidx::run({g_cidx_binary, "--version"});
  CHECK(version.exit_code == 0);
  CHECK(version.out.starts_with("cidx "));
  CHECK_FALSE(std::filesystem::exists(cache));
}

TEST_CASE("typed process adapter executes AST and diff requests") {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "cidx-application-process-data";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  REQUIRE_FALSE(ec);
  const std::filesystem::path source = root / "fixture.cpp";
  const std::filesystem::path index = root / "application.sqlite";
  {
    std::ofstream output(source);
    REQUIRE(output.good());
    output << "int answer() { return 42; }\n";
    cidx::Storage db(index.string());
    db.add_component("app", root.string());
    db.add_file_path(source.string(), std::nullopt, std::nullopt,
                     std::vector<std::string>{"-std=c++17"},
                     std::string("c++"));
  }

  const auto ast = cidx::run({g_cidx_binary, "ast", "dump", source.string(),
                              "--db", index.string(), "--json"});
  REQUIRE(ast.exit_code == 0);
  CHECK(ast.out.find("cursor_nodes") != std::string::npos);

  for (const std::string action : {"locals", "conditions"}) {
    const auto unsupported =
        cidx::run({g_cidx_binary, "ast", action, source.string(), "--db",
                   index.string()});
    CHECK(unsupported.exit_code == 1);
    CHECK(unsupported.err.find("not implemented yet") != std::string::npos);
  }

  const auto diff = cidx::run({g_cidx_binary, "diff", "index", "index-a",
                               "index-b", "--db", index.string()});
  REQUIRE(diff.exit_code == 0);
  CHECK(diff.out.find("\"equal\": false") != std::string::npos);

  const auto configuration = cidx::run(
      {g_cidx_binary, "diff", "configuration", source.string(), source.string(),
       "--db", index.string(), "--left-configuration", "-std=c++17",
       "--right-configuration", "-std=c++20"});
  REQUIRE(configuration.exit_code == 0);
  CHECK(configuration.out.find("\"identical\": false") != std::string::npos);

  const auto index_run = cidx::run({g_cidx_binary, "index", "fixture.cpp",
                                    "--source", "app", "--db", index.string()});
  REQUIRE(index_run.exit_code == 0);
  CHECK(index_run.out ==
        "file: " + source.string() +
            "\n  -> 1 symbols; headers: 0 indexed (+0 symbols), 0 already, "
            "0 system, 0 unowned\nindex: 1 indexed, 0 failed, 0 already "
            "indexed\n");

  const auto index_skip =
      cidx::run({g_cidx_binary, "index", "fixture.cpp", "--source", "app",
                 "--db", index.string()});
  REQUIRE(index_skip.exit_code == 0);
  CHECK(index_skip.out ==
        "file: " + source.string() +
            "\n  already indexed\nindex: 0 indexed, 0 failed, "
            "1 already indexed\n");

  const auto index_unknown =
      cidx::run({g_cidx_binary, "index", "missing.cpp", "--source", "app",
                 "--db", index.string()});
  CHECK(index_unknown.exit_code == 1);
  CHECK(index_unknown.err == "error: not in index database: " +
                                 (root / "missing.cpp").string() + "\n");

  const std::filesystem::path facts = root / "facts";
  const auto export_facts =
      cidx::run({g_cidx_binary, "analyze", "--export-facts", facts.string(),
                 "--db", index.string()});
  REQUIRE(export_facts.exit_code == 0);
  CHECK(export_facts.out == facts.string() + ": 10 fact files, 61 rows\n");

  const auto bad_ast = cidx::run({g_cidx_binary, "ast", "unknown",
                                  source.string(), "--db", index.string()});
  CHECK(bad_ast.exit_code == 2);
  CHECK(bad_ast.err.find("unknown AST action") != std::string::npos);

  const auto bad_diff =
      cidx::run({g_cidx_binary, "diff", "unknown", "left", "right"});
  CHECK(bad_diff.exit_code == 2);
  CHECK(bad_diff.err.find("unknown diff scope") != std::string::npos);

  std::filesystem::remove_all(root, ec);
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    return 2;
  }
  g_cidx_binary = argv[1];
  doctest::Context context(argc - 1, argv + 1);
  return context.run();
}
