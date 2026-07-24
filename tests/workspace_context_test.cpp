// Shared workspace snapshot and translation-unit descriptor contract tests.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "toolchain/toolchain.hpp"
#include "util/logger.hpp"
#include "workspace/context.hpp"

namespace fs = std::filesystem;

namespace {

std::string make_temp_dir() {
  char tmpl[] = "/tmp/cidx_workspace_XXXXXX";
  char *dir = ::mkdtemp(tmpl);
  REQUIRE(dir != nullptr);
  return dir;
}

void write_file(const fs::path &path, const std::string &contents) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  REQUIRE(out.good());
  out << contents;
}

} // namespace

TEST_CASE("workspace identity includes active clone and is canonical") {
  cidx::WorkspaceSnapshot first;
  first.repositories.push_back(cidx::Repository{.name = "repo",
                                                .kind = "repo"});
  first.components.push_back(cidx::Component{.name = "repo",
                                             .path = "/tmp/repo",
                                             .kind = "repo"});
  first.recompute_identity();

  cidx::WorkspaceSnapshot second = first;
  second.active_clones.push_back(
      cidx::Clone{.repository_id = 1, .path = "/tmp/repo-worktree"});
  second.recompute_identity();

  CHECK(first.canonical_json != second.canonical_json);
  CHECK(first.identity != second.identity);
  CHECK(first.canonical_json.find("\"components\"") == 1);
}

TEST_CASE("descriptor resolver returns deterministic ambiguity diagnostics") {
  const fs::path root = make_temp_dir();
  const fs::path source = root / "src" / "main.cpp";
  const fs::path index = root / "index.db";
  const fs::path compile_db = root / "compile_commands.json";
  write_file(source, "int main() { return 0; }\n");
  write_file(compile_db,
             "[{\"directory\":\"" + root.string() +
                 "\",\"file\":\"" + source.string() +
                 "\",\"arguments\":[\"clang++\",\"-std=c++23\",\"-DVAR=1\"]},"
                 "{\"directory\":\"" + root.string() +
                 "\",\"file\":\"" + source.string() +
                 "\",\"arguments\":[\"clang++\",\"-std=c++23\",\"-DVAR=2\"]}]");

  {
    cidx::Storage storage(index.string());
    const int64_t repository_id = storage.add_repository("repo", "repo");
    storage.add_component("repo", root.string(), "repo", std::nullopt);
    storage.set_active_clone(repository_id, std::nullopt);
  }

  cidx::WorkspaceContext context = cidx::WorkspaceContext::open(
      index.string(), cidx::WorkspaceReadWriteMode::read_only);
  cidx::Toolchain toolchain(cidx::Logger::root());
  toolchain.set_resource_include_for_test(std::nullopt);
  cidx::TranslationUnitConfigurationService resolver(context, toolchain,
                                                     compile_db.string());

  const auto descriptors = resolver.resolve_all(source.string());
  REQUIRE(descriptors.size() == 2);
  CHECK(descriptors[0].semantic_hash < descriptors[1].semantic_hash);
  CHECK_THROWS_AS(static_cast<void>(resolver.resolve(source.string())),
                  cidx::WorkspaceError);
  try {
    (void)resolver.resolve(source.string());
  } catch (const cidx::WorkspaceError &error) {
    CHECK(error.code() ==
          cidx::WorkspaceErrorCode::ambiguous_translation_unit);
    CHECK(error.candidates().size() == 2);
  }
}

TEST_CASE("unregistered files have typed workspace errors") {
  const fs::path root = make_temp_dir();
  const fs::path index = root / "index.db";
  const fs::path compile_db = root / "compile_commands.json";
  write_file(compile_db, "[]");
  { cidx::Storage storage(index.string()); }
  cidx::WorkspaceContext context = cidx::WorkspaceContext::open(
      index.string(), cidx::WorkspaceReadWriteMode::read_only);
  cidx::Toolchain toolchain(cidx::Logger::root());
  cidx::TranslationUnitConfigurationService resolver(context, toolchain,
                                                     compile_db.string());

  try {
    (void)resolver.resolve((root / "missing.hpp").string());
    FAIL("expected a typed workspace error");
  } catch (const cidx::WorkspaceError &error) {
    CHECK(error.code() == cidx::WorkspaceErrorCode::unregistered_file);
    CHECK(error.candidates().size() == 1);
  }
}
