// Qualification tests for the explicit SQLite runtime profiles.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "profile/index_profile.hpp"
#include "storage/sqlite.hpp"
#include "storage/storage.hpp"
#include "util/errors.hpp"

namespace {

std::string make_temp_dir() {
  char tmpl[] = "/tmp/cidx_storage_profile_XXXXXX";
  char *directory = ::mkdtemp(tmpl);
  REQUIRE(directory != nullptr);
  return directory;
}

std::string pragma_text(cidx::SqliteDb &db, const char *name) {
  auto statement = db.prepare(std::string("PRAGMA ") + name);
  REQUIRE(statement.step());
  return statement.col_text(0);
}

int64_t pragma_int(cidx::SqliteDb &db, const char *name) {
  auto statement = db.prepare(std::string("PRAGMA ") + name);
  REQUIRE(statement.step());
  return statement.col_int64(0);
}

std::vector<std::string> directory_snapshot(const std::string &path) {
  std::vector<std::string> names;
  for (const auto &entry : std::filesystem::directory_iterator(path)) {
    names.push_back(entry.path().filename().string());
  }
  std::ranges::sort(names);
  return names;
}

} // namespace

TEST_CASE("SQLite profiles are explicit and WAL is not a production default") {
  const auto indexing =
      cidx::sqlite_profile_settings(cidx::SqliteProfile::indexing);
  CHECK(indexing.busy_timeout_ms == 5000);
  CHECK(indexing.foreign_keys);
  CHECK_FALSE(indexing.query_only);
  CHECK(indexing.rollback_journal);
  CHECK(indexing.full_synchronous);
  CHECK(cidx::sqlite_profile_name(cidx::SqliteProfile::read_only_replay) ==
        "read_only_replay");

  const auto replay =
      cidx::sqlite_profile_settings(cidx::SqliteProfile::read_only_replay);
  CHECK(replay.busy_timeout_ms == 5000);
  CHECK(replay.foreign_keys);
  CHECK(replay.query_only);
  CHECK_FALSE(replay.rollback_journal);
  CHECK_FALSE(replay.full_synchronous);
}

TEST_CASE("read-only replay is non-mutating and backup preserves identity") {
  const std::string directory = make_temp_dir();
  const std::string database_path = directory + "/index.db";
  const std::string backup_path = directory + "/backup.db";

  cidx::Stats before;
  {
    cidx::Storage database(database_path);
    CHECK(database.raw_db().profile() == cidx::SqliteProfile::indexing);
    CHECK(pragma_text(database.raw_db(), "journal_mode") == "delete");
    CHECK(pragma_int(database.raw_db(), "synchronous") == 2);
    CHECK(pragma_int(database.raw_db(), "foreign_keys") == 1);
    CHECK(pragma_int(database.raw_db(), "query_only") == 0);
    CHECK(database.integrity_ok());
    CHECK(database.foreign_keys_ok());
    before = database.stats();
    database.run_maintenance();
    database.refresh_statistics();
    database.backup_to(backup_path);
  }

  const auto before_snapshot = directory_snapshot(directory);
  const auto before_size = std::filesystem::file_size(database_path);
  {
    cidx::Storage replay(database_path, cidx::Storage::OpenMode::read_only);
    CHECK(replay.raw_db().profile() == cidx::SqliteProfile::read_only_replay);
    CHECK(pragma_int(replay.raw_db(), "foreign_keys") == 1);
    CHECK(pragma_int(replay.raw_db(), "query_only") == 1);
    CHECK(replay.integrity_ok());
    CHECK(replay.foreign_keys_ok());
    CHECK(replay.stats().symbols == before.symbols);
    CHECK_THROWS_AS(
        replay.raw_db().exec("CREATE TABLE should_fail(id INTEGER)"),
        cidx::StorageError);
  }
  CHECK(directory_snapshot(directory) == before_snapshot);
  CHECK(std::filesystem::file_size(database_path) == before_size);

  {
    cidx::SqliteDb interactive(database_path, true,
                               cidx::SqliteProfile::interactive_read);
    CHECK(interactive.profile() == cidx::SqliteProfile::interactive_read);
    CHECK(pragma_int(interactive, "query_only") == 1);
    CHECK_THROWS_AS(
        interactive.exec("CREATE TABLE should_fail_again(id INTEGER)"),
        cidx::StorageError);
  }

  CHECK_THROWS_AS(cidx::SqliteDb(database_path, false,
                                 cidx::SqliteProfile::interactive_read),
                  cidx::StorageError);
  CHECK_THROWS_AS(
      cidx::SqliteDb(database_path, true, cidx::SqliteProfile::indexing),
      cidx::StorageError);

  cidx::Storage restored(backup_path, cidx::Storage::OpenMode::read_only);
  CHECK(restored.integrity_ok());
  CHECK(restored.foreign_keys_ok());
  CHECK(restored.stats().symbols == before.symbols);
  CHECK(restored.stats().edges == before.edges);
  CHECK(pragma_text(restored.raw_db(), "journal_mode") == "delete");
}

TEST_CASE("SQLite changes and variable limits are connection-local") {
  cidx::SqliteDb database(":memory:");
  database.exec(
      "CREATE TABLE values_table(id INTEGER PRIMARY KEY, value TEXT)");
  const int original_limit = database.variable_limit();
  CHECK(original_limit > 0);
  {
    const int lowered = std::max(1, original_limit - 1);
    cidx::SqliteDb::VariableLimitOverrideForTesting override(database, lowered);
    CHECK(database.variable_limit() == lowered);
  }
  CHECK(database.variable_limit() == original_limit);

  auto insert = database.prepare(
      "INSERT OR IGNORE INTO values_table(id, value) VALUES (?, ?)");
  insert.bind(1, int64_t{1});
  insert.bind(2, std::string_view{"first"});
  insert.step_done();
  CHECK(database.changes() == 1);
  insert.reset();
  insert.bind(1, int64_t{1});
  insert.bind(2, std::string_view{"duplicate"});
  insert.step_done();
  CHECK(database.changes() == 0);

  auto update =
      database.prepare("UPDATE values_table SET value = ? WHERE id = ?");
  update.bind(1, std::string_view{"updated"});
  update.bind(2, int64_t{1});
  update.step_done();
  CHECK(database.changes() == 1);

  auto remove = database.prepare("DELETE FROM values_table WHERE id = ?");
  remove.bind(1, int64_t{1});
  remove.step_done();
  CHECK(database.changes() == 1);
}

TEST_CASE("profile records prepared SQL text bytes") {
  const std::string directory = make_temp_dir();
  const std::string profile_path = directory + "/profile.json";
  {
    cidx::profile::Session session(profile_path, std::nullopt);
    cidx::SqliteDb database(":memory:");
    auto statement = database.prepare("SELECT 1");
    CHECK(statement.step());
  }
  std::ifstream input(profile_path);
  const std::string profile((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
  CHECK(profile.find("prepared_sql_text_bytes") != std::string::npos);
}
