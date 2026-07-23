#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "storage/artifacts.hpp"
#include "storage/storage.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path test_root(const char *suffix) {
  const auto root = std::filesystem::temp_directory_path() /
                    (std::string("cidx-artifacts-") + suffix);
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::create_directories(root);
  return root;
}

cidx::ArtifactSpec complete_spec() {
  cidx::ArtifactSpec spec;
  spec.logical_id = "tu:1:astgraph";
  spec.kind = "astgraph";
  spec.producer_version = "test-producer/1";
  spec.engine_version = "test-engine/1";
  spec.workspace_identity = "workspace:test";
  spec.tu_identity = "tu:1";
  spec.configuration_identity = "config:1";
  spec.input_fact_set_identity = "facts:1";
  spec.completeness = cidx::ArtifactCompleteness::complete;
  spec.truncation = cidx::ArtifactTruncation::none;
  spec.trust = cidx::ArtifactTrust::trusted;
  spec.attachment_name = "astgraph";
  spec.exposed_relations = {"node", "edge"};
  return spec;
}

} // namespace

TEST_CASE("artifact publication is deterministic and read-only") {
  cidx::Storage storage(":memory:");
  const auto root = test_root("publish");
  cidx::ArtifactStore artifacts(storage, root);

  const auto record =
      artifacts.publish(complete_spec(), [](cidx::SqliteDb &db) {
        db.exec("CREATE TABLE node(id INTEGER PRIMARY KEY, usr TEXT NOT NULL);"
                "INSERT INTO node VALUES (1, 'stable:node:1');");
      });
  CHECK(record.relative_path.starts_with("artifacts/"));
  CHECK(record.content_hash.size() == 40);
  CHECK(artifacts.current(record.spec.logical_id)->content_hash ==
        record.content_hash);
  CHECK(artifacts.validate(record.spec.logical_id).usable());

  auto attachment = artifacts.attach_current(record.spec.logical_id);
  {
    auto query = storage.raw_db().prepare("SELECT usr FROM astgraph.node");
    REQUIRE(query.step());
    CHECK(query.col_text(0) == "stable:node:1");
  }
  CHECK_THROWS(storage.raw_db().exec(
      "INSERT INTO astgraph.node VALUES (2, 'should-not-write')"));
  attachment.reset();
  CHECK_NOTHROW(storage.raw_db().exec(
      "CREATE TABLE core_write_guard(value TEXT);"
      "INSERT INTO core_write_guard VALUES ('after-attach')"));

  cidx::ArtifactStore limited(storage, root, 1);
  auto limited_attachment = limited.attach_current(record.spec.logical_id);
  CHECK_THROWS(
      static_cast<void>(limited.attach_current(record.spec.logical_id)));
  limited_attachment.reset();

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

TEST_CASE("artifact validation reports partial and missing results") {
  cidx::Storage storage(":memory:");
  const auto root = test_root("diagnostics");
  cidx::ArtifactStore artifacts(storage, root);
  auto spec = complete_spec();
  spec.logical_id = "tu:2:proof";
  spec.kind = "proof";
  spec.attachment_name = "proof";
  spec.completeness = cidx::ArtifactCompleteness::partial;
  spec.trust = cidx::ArtifactTrust::unknown;
  const auto record = artifacts.publish(spec, [](cidx::SqliteDb &db) {
    db.exec("CREATE TABLE proof(id INTEGER PRIMARY KEY)");
  });
  const auto validation = artifacts.validate(spec.logical_id);
  CHECK_FALSE(validation.usable());
  CHECK(validation.diagnostics.size() >= 2);
  CHECK_THROWS(static_cast<void>(artifacts.attach_current(spec.logical_id)));
  std::filesystem::remove(root / record.relative_path);
  const auto missing = artifacts.validate(spec.logical_id);
  REQUIRE_FALSE(missing.usable());
  CHECK(missing.diagnostics.front().code == "missing");
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

TEST_CASE("existing astgraph output is adopted by the manifest policy") {
  cidx::Storage storage(":memory:");
  const auto root = test_root("adopt");
  const auto source = root / "sample-tu.db";
  {
    cidx::SqliteDb sidecar(source.string());
    sidecar.exec("CREATE TABLE node(id INTEGER PRIMARY KEY);"
                 "INSERT INTO node VALUES (7);");
  }
  cidx::ArtifactStore artifacts(storage, root);
  auto spec = complete_spec();
  spec.logical_id = "tu:7:astgraph";
  const auto record = artifacts.publish_existing(spec, source);
  CHECK_FALSE(std::filesystem::exists(source));
  CHECK(artifacts.validate(spec.logical_id).usable());
  CHECK(std::filesystem::exists(root / record.relative_path));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

TEST_CASE("current selection and recovery respect leases and pins") {
  cidx::Storage storage(":memory:");
  const auto root = test_root("recovery");
  cidx::ArtifactStore artifacts(storage, root);
  auto first_spec = complete_spec();
  const auto first = artifacts.publish(first_spec, [](cidx::SqliteDb &db) {
    db.exec("CREATE TABLE node(id INTEGER PRIMARY KEY); INSERT INTO node "
            "VALUES (1)");
  });
  artifacts.lease(first_spec.logical_id, "replay-1", "pinned replay");
  auto second_spec = first_spec;
  const auto second = artifacts.publish(second_spec, [](cidx::SqliteDb &db) {
    db.exec("CREATE TABLE node(id INTEGER PRIMARY KEY); INSERT INTO node "
            "VALUES (2)");
  });
  CHECK(artifacts.current(first_spec.logical_id)->content_hash ==
        second.content_hash);
  CHECK(std::filesystem::exists(root / first.relative_path));
  CHECK(artifacts.recover() == 0);
  artifacts.unlease(first_spec.logical_id, "replay-1");
  CHECK(artifacts.recover() >= 1);
  CHECK_FALSE(std::filesystem::exists(root / first.relative_path));
  CHECK(std::filesystem::exists(root / second.relative_path));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

TEST_CASE("v34 database is upgraded with the artifact manifest") {
  const auto root = test_root("migration");
  const auto path = root / "index.db";
  {
    cidx::Storage storage(path.string());
    storage.raw_db().exec(
        "PRAGMA foreign_keys = OFF;"
        "DROP TABLE artifact_pin;"
        "DROP TABLE artifact_lease;"
        "DROP TABLE artifact_identity_map;"
        "DROP TABLE artifact_relation;"
        "DROP TABLE artifact;"
        "UPDATE meta SET value = '34' WHERE key = 'schema_version';");
  }
  {
    cidx::Storage migrated(path.string());
    auto schema = migrated.raw_db().prepare(
        "SELECT value FROM meta WHERE key = 'schema_version'");
    REQUIRE(schema.step());
    CHECK(schema.col_text(0) == "35");
    auto artifact =
        migrated.raw_db().prepare("SELECT name FROM sqlite_master WHERE type = "
                                  "'table' AND name = 'artifact'");
    CHECK(artifact.step());
  }
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}
