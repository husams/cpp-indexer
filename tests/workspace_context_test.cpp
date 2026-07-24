// Shared workspace snapshot and translation-unit descriptor contract tests.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "compiledb/compiledb.hpp"
#include "storage/storage.hpp"
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

std::vector<cidx::WorkspaceCompileCommand>
load_commands(const fs::path &path) {
  std::vector<cidx::WorkspaceCompileCommand> commands;
  for (const cidx::CompileCommand &command : cidx::CompileDb::load(path.string())) {
    commands.push_back({.directory = command.directory,
                        .filename = command.filename,
                        .driver = command.driver,
                        .args = command.args});
  }
  return commands;
}

} // namespace

TEST_CASE("workspace identity ignores database ids but preserves relationships") {
  cidx::WorkspaceSnapshot first;
  first.semantic_universes.push_back(
      cidx::SemanticUniverse{.id = 7, .key = "program", .name = "Program"});
  first.repositories.push_back(cidx::Repository{
      .id = 41,
      .name = "repo",
      .kind = "repo",
      .remote_url = std::string("https://example.test/repo.git"),
      .active_clone_id = 100,
      .semantic_universe_id = 7});
  first.active_clones.push_back(cidx::Clone{
      .id = 100, .repository_id = 41, .path = "/tmp/repo-worktree"});
  first.components.push_back(cidx::Component{.id = 5,
                                             .name = "repo",
                                             .path = "/tmp/repo",
                                             .kind = "repo",
                                             .repository_id = 41,
                                             .semantic_universe_id = 7});
  first.recompute_identity();

  cidx::WorkspaceSnapshot same_workspace = first;
  same_workspace.semantic_universes.front().id = 700;
  same_workspace.repositories.front().id = 941;
  same_workspace.repositories.front().active_clone_id = 8100;
  same_workspace.repositories.front().semantic_universe_id = 700;
  same_workspace.active_clones.front().id = 8100;
  same_workspace.active_clones.front().repository_id = 941;
  same_workspace.components.front().id = 105;
  same_workspace.components.front().repository_id = 941;
  same_workspace.components.front().semantic_universe_id = 700;
  same_workspace.recompute_identity();

  CHECK(first.canonical_json == same_workspace.canonical_json);
  CHECK(first.identity == same_workspace.identity);
  CHECK(first.canonical_json.find("active_clone_id") == std::string::npos);

  cidx::WorkspaceSnapshot changed_relationship = same_workspace;
  changed_relationship.components.front().repository_id.reset();
  changed_relationship.recompute_identity();
  CHECK(changed_relationship.identity != same_workspace.identity);
}

TEST_CASE("storage adapter supplies the authoritative schema version") {
  cidx::Storage storage(":memory:");
  cidx::StorageWorkspaceAdapter workspace_data(storage);

  CHECK(cidx::IndexIdentity{}.schema_version == 0);
  CHECK(workspace_data.index_identity().schema_version ==
        cidx::kSchemaVersion);
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

  cidx::Storage storage(index.string(), cidx::Storage::OpenMode::read_only);
  cidx::StorageWorkspaceAdapter workspace_data(storage);
  cidx::WorkspaceContext context = cidx::WorkspaceContext::borrow(
      workspace_data, cidx::WorkspaceReadWriteMode::read_only);
  cidx::Toolchain toolchain(cidx::Logger::root());
  toolchain.set_resource_include_for_test(std::nullopt);
  cidx::TranslationUnitConfigurationService resolver(
      context, toolchain, load_commands(compile_db));

  const auto descriptors = resolver.resolve_all(source.string());
  REQUIRE(descriptors.size() == 2);
  CHECK(descriptors[0].semantic_hash < descriptors[1].semantic_hash);
  const auto invocation =
      cidx::TranslationUnitConfigurationService::invocation_arguments(
          source.string(), descriptors.front());
  CHECK(invocation == descriptors.front().configuration.arguments);
  CHECK(invocation.size() == descriptors.front().configuration.arguments.size());
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
  cidx::Storage storage(index.string(), cidx::Storage::OpenMode::read_only);
  cidx::StorageWorkspaceAdapter workspace_data(storage);
  cidx::WorkspaceContext context = cidx::WorkspaceContext::borrow(
      workspace_data, cidx::WorkspaceReadWriteMode::read_only);
  cidx::Toolchain toolchain(cidx::Logger::root());
  cidx::TranslationUnitConfigurationService resolver(
      context, toolchain, load_commands(compile_db));

  try {
    (void)resolver.resolve((root / "missing.hpp").string());
    FAIL("expected a typed workspace error");
  } catch (const cidx::WorkspaceError &error) {
    CHECK(error.code() == cidx::WorkspaceErrorCode::unregistered_file);
    CHECK(error.candidates().size() == 1);
  }
}

TEST_CASE("header resolution uses owner applicability and reports ambiguity") {
  const fs::path root = make_temp_dir();
  const fs::path index = root / "index.db";
  const fs::path header_path = root / "include" / "shared.hpp";
  write_file(header_path, "#pragma once\n");

  cidx::Storage storage(index.string());
  const int64_t component = storage.add_component("headers", root.string());
  const int64_t directory = storage.add_directory(component, "include");
  const int64_t header = storage.add_file(directory, "shared.hpp");

  cidx::TranslationUnitConfig first;
  first.driver = "clang++";
  first.arguments = {"-std=c++23", "-DFIRST=1"};
  const int64_t first_id = storage.add_translation_unit_config(first);

  cidx::StorageWorkspaceAdapter workspace_data(storage);
  cidx::WorkspaceContext context = cidx::WorkspaceContext::borrow(
      workspace_data, cidx::WorkspaceReadWriteMode::read_only);
  cidx::Toolchain toolchain(cidx::Logger::root());
  toolchain.set_resource_include_for_test(std::nullopt);
  cidx::TranslationUnitConfigurationService resolver(context, toolchain);

  try {
    (void)resolver.resolve(header_path.string());
    FAIL("expected an unregistered header without an owner");
  } catch (const cidx::WorkspaceError &error) {
    CHECK(error.code() == cidx::WorkspaceErrorCode::unregistered_file);
  }

  storage.add_file_config({header, first_id, "header",
                           cidx::TranslationUnitConfigState::registered,
                           std::nullopt});
  const auto one = resolver.resolve_all(header_path.string());
  REQUIRE(one.size() == 1);

  cidx::TranslationUnitConfig second = first;
  second.arguments.push_back("-DSECOND=1");
  const int64_t second_id = storage.add_translation_unit_config(second);
  storage.add_file_config({header, second_id, "header",
                           cidx::TranslationUnitConfigState::registered,
                           std::nullopt});

  const auto two = resolver.resolve_all(header_path.string());
  REQUIRE(two.size() == 2);
  CHECK(two[0].semantic_hash < two[1].semantic_hash);
  try {
    (void)resolver.resolve(header_path.string());
    FAIL("expected a typed ambiguity error");
  } catch (const cidx::WorkspaceError &error) {
    CHECK(error.code() == cidx::WorkspaceErrorCode::ambiguous_translation_unit);
    CHECK(error.candidates().size() == 2);
  }
}
