#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cli/args.hpp"
#include "cli/json_out.hpp"
#include "storage/storage.hpp"
#include "util/subprocess.hpp"
#include "workspace/context.hpp"

namespace {

std::string g_cidx_binary;

cidx::json_out::Value empty_file_diff_golden(const std::string &file,
                                             const std::string &index) {
  using cidx::json_out::Value;
  const auto side = [&] {
    return Value::obj({{"file", Value::of(file)},
                       {"db", Value::of(index)},
                       {"tu", Value::null()},
                       {"driver", Value::of(std::string("c++"))},
                       {"std", Value::of(std::string("c++17"))},
                       {"target", Value::null()}});
  };
  const auto empty = [] { return cidx::json_out::Array{}; };
  return Value::obj(
      {{"tool", Value::of(std::string("cidx-diff"))},
       {"version", Value::of(std::string(cidx::cli::kVersion))},
       {"report_version", Value::of(1)},
       {"mode", Value::of(std::string("both"))},
       {"scope", Value::of(std::string("file"))},
       {"match", Value::of(std::string("heuristic"))},
       {"left", side()},
       {"right", side()},
       {"config_delta",
        Value::obj({{"identical", Value::of(true)},
                    {"std", Value::null()},
                    {"target", Value::null()},
                    {"driver", Value::null()},
                    {"definitions_added", Value::arr(empty())},
                    {"definitions_removed", Value::arr(empty())},
                    {"definitions_reordered", Value::of(false)},
                    {"includes_changed", Value::of(false)},
                    {"options_left_only", Value::arr(empty())},
                    {"options_right_only", Value::arr(empty())},
                    {"options_reordered", Value::of(false)}})},
       {"entities", Value::arr(empty())},
       {"syntax", Value::obj({{"status", Value::of(std::string("unchanged"))},
                              {"edit_count", Value::of(0)},
                              {"truncated", Value::of(false)}})},
       {"semantic",
        Value::obj(
            {{"verdict", Value::of(std::string("equivalent"))},
             {"evidence",
              Value::of(std::string("identical-source-and-config"))},
             {"assumptions",
              Value::arr({Value::of(std::string("same-standard-library")),
                          Value::of(std::string("no-undefined-behavior"))})},
             {"unsupported_count", Value::of(0)},
             {"detail", Value::of(std::string(
                            "identical source and configuration (no indexed "
                            "entities)"))}})}});
}

cidx::json_out::Value symbol_diff_golden(const std::string &file,
                                         const std::string &index) {
  using cidx::json_out::Value;
  const auto side = [&] {
    return Value::obj({{"file", Value::of(file)},
                       {"db", Value::of(index)},
                       {"tu", Value::null()},
                       {"driver", Value::of(std::string("c++"))},
                       {"std", Value::of(std::string("c++17"))},
                       {"target", Value::null()}});
  };
  const auto empty = [] { return cidx::json_out::Array{}; };
  const auto range = [] {
    return Value::obj({{"line", Value::of(1)},
                       {"col", Value::of(1)},
                       {"end_line", Value::of(1)},
                       {"end_col", Value::of(28)}});
  };
  const auto syntax = [&] {
    return Value::obj({{"status", Value::of(std::string("unchanged"))},
                       {"edit_count", Value::of(0)},
                       {"truncated", Value::of(false)},
                       {"edits", Value::arr(empty())}});
  };
  const auto semantic = [&] {
    return Value::obj(
        {{"verdict", Value::of(std::string("equivalent"))},
         {"evidence", Value::of(std::string("identical-source-and-config"))},
         {"detail",
          Value::of(std::string("identical source and configuration"))},
         {"changes", Value::arr(empty())},
         {"unsupported", Value::arr(empty())}});
  };
  return Value::obj(
      {{"tool", Value::of(std::string("cidx-diff"))},
       {"version", Value::of(std::string(cidx::cli::kVersion))},
       {"report_version", Value::of(1)},
       {"mode", Value::of(std::string("both"))},
       {"scope", Value::of(std::string("symbol"))},
       {"match", Value::of(std::string("heuristic"))},
       {"left", side()},
       {"right", side()},
       {"config_delta",
        Value::obj({{"identical", Value::of(true)},
                    {"std", Value::null()},
                    {"target", Value::null()},
                    {"driver", Value::null()},
                    {"definitions_added", Value::arr(empty())},
                    {"definitions_removed", Value::arr(empty())},
                    {"definitions_reordered", Value::of(false)},
                    {"includes_changed", Value::of(false)},
                    {"options_left_only", Value::arr(empty())},
                    {"options_right_only", Value::arr(empty())},
                    {"options_reordered", Value::of(false)}})},
       {"entities", Value::arr({Value::obj(
                        {{"kind", Value::of(std::string("function"))},
                         {"name", Value::of(std::string("answer"))},
                         {"signature", Value::of(std::string("answer()"))},
                         {"status", Value::of(std::string("matched"))},
                         {"match", Value::of(std::string("usr"))},
                         {"confidence", Value::of(100)},
                         {"left_usr", Value::of(std::string("c:@F@answer#"))},
                         {"right_usr", Value::of(std::string("c:@F@answer#"))},
                         {"left_range", range()},
                         {"right_range", range()},
                         {"syntax", syntax()},
                         {"semantic", semantic()}})})},
       {"syntax", Value::obj({{"status", Value::of(std::string("unchanged"))},
                              {"edit_count", Value::of(0)},
                              {"truncated", Value::of(false)}})},
       {"semantic",
        Value::obj(
            {{"verdict", Value::of(std::string("equivalent"))},
             {"evidence",
              Value::of(std::string("identical-source-and-config"))},
             {"assumptions",
              Value::arr({Value::of(std::string("same-standard-library")),
                          Value::of(std::string("no-undefined-behavior"))})},
             {"unsupported_count", Value::of(0)},
             {"detail", Value::of(std::string(
                            "identical source and configuration"))}})}});
}

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

  const auto bare_index =
      cidx::run({g_cidx_binary, "index", "--db", index.string()});
  REQUIRE(bare_index.exit_code == 0);
  CHECK(bare_index.out ==
        "indexing " + source.string() +
            "\n  -> 1 symbols; headers: 0 indexed (+0 symbols), 0 already, 0 "
            "system, 0 unowned\n"
            "index: 1 indexed, 0 failed, 0 already indexed\n");

  const auto status =
      cidx::run({g_cidx_binary, "index", "status", "--db", index.string()});
  REQUIRE(status.exit_code == 0);
  CHECK(status.out == "index: 1 files, 1 symbols, 0 edges\n");

  const auto status_json = cidx::run(
      {g_cidx_binary, "index", "status", "--json", "--db", index.string()});
  REQUIRE(status_json.exit_code == 0);
  CHECK(status_json.out ==
        "{\n  \"files\": 1,\n  \"symbols\": 1,\n  \"edges\": 0\n}\n");

  cidx::Storage inspect(index.string(), cidx::Storage::OpenMode::read_only);
  cidx::StorageWorkspaceAdapter workspace_data(inspect);
  const cidx::WorkspaceContext workspace = cidx::WorkspaceContext::borrow(
      workspace_data, cidx::WorkspaceReadWriteMode::read_only, index.string());
  const auto explain =
      cidx::run({g_cidx_binary, "index", "explain", "--db", index.string()});
  REQUIRE(explain.exit_code == 0);
  CHECK(explain.out ==
        "workspace: " + workspace.snapshot().identity + "\nindex: current\n");

  const auto explain_json = cidx::run(
      {g_cidx_binary, "index", "explain", "--json", "--db", index.string()});
  REQUIRE(explain_json.exit_code == 0);
  CHECK(explain_json.out ==
        cidx::json_out::dumps_indent2(cidx::json_out::Value::obj(
            {{"workspace",
              cidx::json_out::Value::of(workspace.snapshot().identity)},
             {"index", cidx::json_out::Value::of(std::string("current"))}})) +
            "\n");

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

  const std::filesystem::path empty = root / "empty.cpp";
  std::ofstream(empty) << "";
  {
    cidx::Storage db(index.string());
    db.add_file_path(empty.string(), std::nullopt, std::nullopt,
                     std::vector<std::string>{"-std=c++17"},
                     std::string("c++"));
  }
  const auto empty_index = cidx::run(
      {g_cidx_binary, "index", empty.string(), "--db", index.string()});
  REQUIRE(empty_index.exit_code == 0);

  const auto file_diff =
      cidx::run({g_cidx_binary, "diff", "file", empty.string(), empty.string(),
                 "--db", index.string()});
  REQUIRE(file_diff.exit_code == 0);

  const auto file_diff_json =
      cidx::run({g_cidx_binary, "diff", "file", empty.string(), empty.string(),
                 "--json", "--db", index.string()});
  REQUIRE(file_diff_json.exit_code == 0);

  CHECK(file_diff.out ==
        "cidx-diff: file  mode: both  match: heuristic\nleft:  " +
            empty.string() + "\nright: " + empty.string() +
            "\nconfig: identical\nsyntax: unchanged\n"
            "semantic: equivalent (identical source and configuration (no "
            "indexed entities))\n"
            "  assumptions: same-standard-library no-undefined-behavior\n");
  CHECK(file_diff_json.out ==
        cidx::json_out::dumps_indent2(
            empty_file_diff_golden(empty.string(), index.string())) +
            "\n");

  const auto symbol_diff = cidx::run(
      {g_cidx_binary, "diff", "symbol", source.string(), source.string(),
       "--selector", "answer", "--db", index.string()});
  REQUIRE(symbol_diff.exit_code == 0);
  CHECK(symbol_diff.out ==
        "cidx-diff: symbol  mode: both  match: heuristic\nleft:  " +
            source.string() + " :: answer()\nright: " + source.string() +
            " :: answer()\nconfig: identical\nsyntax: unchanged\n"
            "semantic: equivalent (identical source and configuration)\n"
            "  assumptions: same-standard-library no-undefined-behavior\n");

  const auto symbol_diff_json = cidx::run(
      {g_cidx_binary, "diff", "symbol", source.string(), source.string(),
       "--selector", "answer", "--json", "--db", index.string()});
  REQUIRE(symbol_diff_json.exit_code == 0);
  CHECK(symbol_diff_json.out ==
        cidx::json_out::dumps_indent2(
            symbol_diff_golden(source.string(), index.string())) +
            "\n");

  const auto index_run = cidx::run({g_cidx_binary, "index", "fixture.cpp",
                                    "--source", "app", "--db", index.string()});
  REQUIRE(index_run.exit_code == 0);
  CHECK(index_run.out == "file: " + source.string() + "\n  already indexed\n");

  const auto index_skip =
      cidx::run({g_cidx_binary, "index", "fixture.cpp", "--source", "app",
                 "--db", index.string()});
  REQUIRE(index_skip.exit_code == 0);
  CHECK(index_skip.out == "file: " + source.string() + "\n  already indexed\n");

  const auto index_unknown =
      cidx::run({g_cidx_binary, "index", "missing.cpp", "--source", "app",
                 "--db", index.string()});
  CHECK(index_unknown.exit_code == 1);
  CHECK(index_unknown.err == "error: not in index database: " +
                                 (root / "missing.cpp").string() + "\n");

  const std::filesystem::path fresh = root / "fresh.cpp";
  std::ofstream(fresh) << "int fresh() { return 7; }\n";
  {
    cidx::Storage db(index.string());
    db.add_file_path(fresh.string(), std::nullopt, std::nullopt,
                     std::vector<std::string>{"-std=c++17"},
                     std::string("c++"));
  }

  const std::filesystem::path broken = root / "broken.cpp";
  {
    cidx::Storage db(index.string());
    db.add_file_path(broken.string(), std::nullopt, std::nullopt,
                     std::vector<std::string>{"-std=c++17"},
                     std::string("c++"));
  }
  const auto mixed =
      cidx::run({g_cidx_binary, "index", "--db", index.string()});
  CHECK(mixed.out == "indexing " + broken.string() + "\nindexing " +
                         fresh.string() +
                         "\n  -> 1 symbols; headers: 0 indexed (+0 symbols), 0 "
                         "already, 0 system, 0 unowned\n"
                         "index: 1 indexed, 1 failed, 2 already indexed\n");
  CHECK(mixed.err == "Error while processing " + broken.string() +
                         ".\nerror: cannot parse " + broken.string() + "\n");

  const std::filesystem::path facts = root / "facts";
  const auto export_facts =
      cidx::run({g_cidx_binary, "analyze", "--export-facts", facts.string(),
                 "--db", index.string()});
  REQUIRE(export_facts.exit_code == 0);
  CHECK(export_facts.out == facts.string() + ": 10 fact files, 65 rows\n");

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
