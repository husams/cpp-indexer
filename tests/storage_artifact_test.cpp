#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "storage/artifacts.hpp"
#include "storage/storage.hpp"
#include "util/hashing.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

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
  spec.artifact_schema = "cidx-astgraph/v3";
  spec.producer_version = "cidx-astgraph test/1";
  spec.engine_version = "cidx test/1";
  spec.workspace_identity = "workspace:test";
  spec.tu_identity = "tu:1";
  spec.configuration_identity = "config:1";
  spec.input_fact_set_identity = "facts:1";
  spec.completeness = cidx::ArtifactCompleteness::complete;
  spec.truncation = cidx::ArtifactTruncation::none;
  spec.trust = cidx::ArtifactTrust::producer_verified;
  spec.attachment_name = "astgraph";
  spec.exposed_relations = {"node", "edge", "symbol", "meta"};
  return spec;
}

void create_astgraph_tables(cidx::SqliteDb &db) {
  db.exec("CREATE TABLE node(id INTEGER PRIMARY KEY);"
          "CREATE TABLE edge(id INTEGER PRIMARY KEY);"
          "CREATE TABLE symbol(id INTEGER PRIMARY KEY);"
          "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);");
}

} // namespace

TEST_CASE("artifact publication is deterministic and read-only") {
  cidx::Storage storage(":memory:");
  const auto root = test_root("publish");
  cidx::ArtifactStore artifacts(storage, root);

  const auto record =
      artifacts.publish(complete_spec(), [](cidx::SqliteDb &db) {
        db.exec("CREATE TABLE node(id INTEGER PRIMARY KEY, usr TEXT NOT NULL);"
                "CREATE TABLE edge(id INTEGER PRIMARY KEY);"
                "CREATE TABLE symbol(id INTEGER PRIMARY KEY);"
                "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);"
                "INSERT INTO node VALUES (1, 'stable:node:1');");
      });
  CHECK(record.relative_path.starts_with("artifacts/"));
  CHECK(record.content_hash.starts_with("sha256:"));
  REQUIRE(artifacts.current(record.spec.logical_id).has_value());
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

TEST_CASE("identical publication is idempotent") {
  cidx::Storage storage(":memory:");
  const auto root = test_root("idempotent");
  cidx::ArtifactStore artifacts(storage, root);
  const auto first = artifacts.publish(complete_spec(), [](cidx::SqliteDb &db) {
    create_astgraph_tables(db);
    db.exec("INSERT INTO node VALUES (1)");
  });
  const auto second =
      artifacts.publish(complete_spec(), [](cidx::SqliteDb &db) {
        create_astgraph_tables(db);
        db.exec("INSERT INTO node VALUES (1)");
      });
  CHECK(second.id == first.id);
  CHECK(second.content_hash == first.content_hash);
  auto count = storage.raw_db().prepare(
      "SELECT COUNT(*) FROM artifact WHERE logical_id = ?");
  count.bind(1, std::string_view(first.spec.logical_id));
  REQUIRE(count.step());
  CHECK(count.col_int64(0) == 1);
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
  spec.artifact_schema = "cidx-proof/v1";
  spec.producer_version = "cidx-proof test/1";
  spec.attachment_name = "proof";
  spec.completeness = cidx::ArtifactCompleteness::partial;
  spec.trust = cidx::ArtifactTrust::unverified;
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
                 "CREATE TABLE edge(id INTEGER PRIMARY KEY);"
                 "CREATE TABLE symbol(id INTEGER PRIMARY KEY);"
                 "CREATE TABLE meta(key TEXT PRIMARY KEY);"
                 "INSERT INTO node VALUES (7);");
  }
  cidx::ArtifactStore artifacts(storage, root);
  auto spec = complete_spec();
  spec.logical_id = "tu:7:astgraph";
  const auto record = artifacts.publish_existing(
      spec, source, [&](const cidx::ArtifactRecord &published) {
        artifacts.record_identity_mapping(published.spec.logical_id,
                                          "usr:resolved", "usr", "usr:resolved",
                                          "resolved", 7);
        artifacts.record_identity_mapping(
            published.spec.logical_id, "usr:missing", "usr", "usr:missing",
            "unresolved", std::nullopt, "core symbol is not present in index");
      });
  CHECK_FALSE(std::filesystem::exists(source));
  CHECK(artifacts.validate(spec.logical_id).usable());
  CHECK(std::filesystem::exists(root / record.relative_path));
  auto mappings =
      storage.raw_db().prepare("SELECT COUNT(*), SUM(resolution_state = "
                               "'resolved') FROM artifact_identity_map");
  REQUIRE(mappings.step());
  CHECK(mappings.col_int64(0) == 2);
  CHECK(mappings.col_int64(1) == 1);
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

TEST_CASE("current selection and recovery respect leases and pins") {
  cidx::Storage storage(":memory:");
  const auto root = test_root("recovery");
  cidx::ArtifactStore artifacts(storage, root);
  auto first_spec = complete_spec();
  const auto first = artifacts.publish(first_spec, [](cidx::SqliteDb &db) {
    create_astgraph_tables(db);
    db.exec("INSERT INTO node VALUES (1)");
  });
  artifacts.lease(first_spec.logical_id, "replay-1", "pinned replay");
  auto second_spec = first_spec;
  const auto second = artifacts.publish(second_spec, [](cidx::SqliteDb &db) {
    create_astgraph_tables(db);
    db.exec("INSERT INTO node VALUES (2)");
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

TEST_CASE("stale pins can be released after superseding") {
  cidx::Storage storage(":memory:");
  const auto root = test_root("stale-pin");
  cidx::ArtifactStore artifacts(storage, root);
  auto spec = complete_spec();
  const auto first = artifacts.publish(spec, [](cidx::SqliteDb &db) {
    create_astgraph_tables(db);
    db.exec("INSERT INTO node VALUES (11)");
  });
  artifacts.pin(spec.logical_id, "rebuild", "keep old generation");
  artifacts.publish(spec, [](cidx::SqliteDb &db) {
    create_astgraph_tables(db);
    db.exec("INSERT INTO node VALUES (12)");
  });
  artifacts.unpin(spec.logical_id, "rebuild");
  CHECK(artifacts.recover() >= 1);
  CHECK_FALSE(std::filesystem::exists(root / first.relative_path));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

TEST_CASE("attachment names and query-only state are connection-wide") {
  cidx::Storage storage(":memory:");
  const auto root = test_root("attachments");
  cidx::ArtifactStore artifacts(storage, root);
  auto first_spec = complete_spec();
  first_spec.logical_id = "tu:attach:one";
  first_spec.attachment_name = "astgraph_one";
  auto second_spec = first_spec;
  second_spec.logical_id = "tu:attach:two";
  second_spec.attachment_name = "astgraph_two";
  artifacts.publish(first_spec, [](cidx::SqliteDb &db) {
    db.exec("CREATE TABLE node(id INTEGER PRIMARY KEY);"
            "CREATE TABLE edge(id INTEGER PRIMARY KEY);"
            "CREATE TABLE symbol(id INTEGER PRIMARY KEY);"
            "CREATE TABLE meta(key TEXT PRIMARY KEY);"
            "INSERT INTO node VALUES (1)");
  });
  artifacts.publish(second_spec, [](cidx::SqliteDb &db) {
    db.exec("CREATE TABLE node(id INTEGER PRIMARY KEY);"
            "CREATE TABLE edge(id INTEGER PRIMARY KEY);"
            "CREATE TABLE symbol(id INTEGER PRIMARY KEY);"
            "CREATE TABLE meta(key TEXT PRIMARY KEY);"
            "INSERT INTO node VALUES (2)");
  });
  auto first = artifacts.attach_current(first_spec.logical_id);
  auto second = artifacts.attach_current(second_spec.logical_id);
  auto query_only = storage.raw_db().prepare("PRAGMA query_only");
  REQUIRE(query_only.step());
  CHECK(query_only.col_int64(0) == 1);
  first.reset();
  query_only = storage.raw_db().prepare("PRAGMA query_only");
  REQUIRE(query_only.step());
  CHECK(query_only.col_int64(0) == 1);
  second.reset();
  query_only = storage.raw_db().prepare("PRAGMA query_only");
  REQUIRE(query_only.step());
  CHECK(query_only.col_int64(0) == 0);

  second = artifacts.attach_current(second_spec.logical_id);
  first = artifacts.attach_current(first_spec.logical_id);
  second.reset();
  first.reset();
  query_only = storage.raw_db().prepare("PRAGMA query_only");
  REQUIRE(query_only.step());
  CHECK(query_only.col_int64(0) == 0);
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

TEST_CASE("validation rejects unsupported contracts and missing relations") {
  cidx::Storage storage(":memory:");
  const auto root = test_root("validation-contract");
  cidx::ArtifactStore artifacts(storage, root);
  auto unsupported = complete_spec();
  unsupported.logical_id = "unsupported";
  unsupported.artifact_schema = "cidx-astgraph/v999";
  const auto unsupported_record =
      artifacts.publish(unsupported, [](cidx::SqliteDb &db) {
        db.exec("CREATE TABLE node(id INTEGER PRIMARY KEY);"
                "CREATE TABLE edge(id INTEGER PRIMARY KEY);");
      });
  CHECK_FALSE(artifacts.validate(unsupported_record.spec.logical_id).usable());

  auto missing_relation = complete_spec();
  missing_relation.logical_id = "missing-relation";
  missing_relation.exposed_relations = {"node", "edge"};
  const auto missing_record =
      artifacts.publish(missing_relation, [](cidx::SqliteDb &db) {
        db.exec("CREATE TABLE node(id INTEGER PRIMARY KEY);");
      });
  const auto validation = artifacts.validate(missing_record.spec.logical_id);
  CHECK_FALSE(validation.usable());
  CHECK(std::ranges::any_of(
      validation.diagnostics, [](const cidx::ArtifactDiagnostic &diagnostic) {
        return diagnostic.code == "missing_required_relation";
      }));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

TEST_CASE("publication and adoption reject symlink escapes") {
  cidx::Storage storage(":memory:");
  const auto root = test_root("symlink-escape");
  const auto outside = test_root("symlink-escape-outside");
  cidx::ArtifactStore artifacts(storage, root);
  auto spec = complete_spec();

  const auto source = outside / "source.db";
  {
    cidx::SqliteDb sidecar(source.string());
    create_astgraph_tables(sidecar);
  }
  const auto source_link = root / "input";
  std::filesystem::create_directory_symlink(outside, source_link);
  CHECK_THROWS(static_cast<void>(
      artifacts.publish_existing(spec, source_link / source.filename())));

  const auto outside_artifacts = outside / "artifacts";
  std::filesystem::create_directories(outside_artifacts);
  const auto kind_directory = root / "artifacts" / cidx::sha256_hex(spec.kind);
  std::filesystem::create_directories(kind_directory.parent_path());
  std::filesystem::create_directory_symlink(outside_artifacts, kind_directory);
  CHECK_THROWS(static_cast<void>(artifacts.publish(
      spec, [](cidx::SqliteDb &db) { create_astgraph_tables(db); })));

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::remove_all(outside, ignored);
}

TEST_CASE("attachment reset is safe after store destruction") {
  cidx::Storage storage(":memory:");
  const auto root = test_root("attachment-lifetime");
  std::unique_ptr<cidx::ArtifactAttachment> attachment;
  {
    cidx::ArtifactStore artifacts(storage, root);
    const auto record =
        artifacts.publish(complete_spec(), [](cidx::SqliteDb &db) {
          db.exec("CREATE TABLE node(id INTEGER PRIMARY KEY);"
                  "CREATE TABLE edge(id INTEGER PRIMARY KEY);"
                  "CREATE TABLE symbol(id INTEGER PRIMARY KEY);"
                  "CREATE TABLE meta(key TEXT PRIMARY KEY);");
        });
    attachment = artifacts.attach_current(record.spec.logical_id);
  }
  CHECK_NOTHROW(attachment.reset());
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
    CHECK(schema.col_text(0) == "36");
    auto artifact =
        migrated.raw_db().prepare("SELECT name FROM sqlite_master WHERE type = "
                                  "'table' AND name = 'artifact'");
    CHECK(artifact.step());
  }
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

TEST_CASE("v35 artifact manifests migrate to the generated contract") {
  const auto root = test_root("migration-v35");
  const auto path = root / "index.db";
  {
    cidx::Storage storage(path.string());
    storage.raw_db().exec(
        "PRAGMA foreign_keys = OFF;"
        "DROP TABLE artifact_pin; DROP TABLE artifact_lease;"
        "DROP TABLE artifact_identity_map; DROP TABLE artifact_relation;"
        "DROP TABLE artifact;"
        "CREATE TABLE artifact (id INTEGER PRIMARY KEY, logical_id TEXT NOT "
        "NULL, "
        "kind TEXT NOT NULL, artifact_schema TEXT NOT NULL, catalog_version "
        "TEXT NOT NULL, "
        "producer_version TEXT NOT NULL, engine_version TEXT NOT NULL, "
        "workspace_identity TEXT NOT NULL, "
        "tu_identity TEXT NOT NULL DEFAULT '', configuration_identity TEXT NOT "
        "NULL DEFAULT '', "
        "input_fact_set_identity TEXT NOT NULL DEFAULT '', completeness TEXT "
        "NOT NULL, "
        "truncation TEXT NOT NULL, trust TEXT NOT NULL, attachment_name TEXT "
        "NOT NULL, "
        "retention_policy TEXT NOT NULL DEFAULT 'retain', relative_path TEXT "
        "NOT NULL, "
        "content_hash TEXT NOT NULL, byte_size INTEGER NOT NULL, state TEXT "
        "NOT NULL, "
        "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP, published_at "
        "TEXT, "
        "UNIQUE(logical_id, content_hash));"
        "CREATE TABLE artifact_relation (artifact_id INTEGER NOT NULL, "
        "relation_name TEXT NOT NULL, PRIMARY KEY (artifact_id, "
        "relation_name));"
        "CREATE TABLE artifact_identity_map (artifact_id INTEGER NOT NULL, "
        "local_identity TEXT NOT NULL, identity_kind TEXT NOT NULL, "
        "stable_identity TEXT NOT NULL, resolution_state TEXT NOT NULL, "
        "core_symbol_id INTEGER, diagnostic TEXT NOT NULL DEFAULT '', PRIMARY "
        "KEY (artifact_id, local_identity, identity_kind));"
        "CREATE TABLE artifact_lease (artifact_id INTEGER NOT NULL, lease_id "
        "TEXT NOT NULL, purpose TEXT NOT NULL, PRIMARY KEY (artifact_id, "
        "lease_id));"
        "CREATE TABLE artifact_pin (artifact_id INTEGER NOT NULL, pin_id TEXT "
        "NOT NULL, reason TEXT NOT NULL, PRIMARY KEY (artifact_id, pin_id));"
        "INSERT INTO artifact(logical_id, kind, artifact_schema, "
        "catalog_version, producer_version, engine_version, "
        "workspace_identity, completeness, truncation, trust, attachment_name, "
        "relative_path, content_hash, byte_size, state) "
        "VALUES ('legacy', 'astgraph', 'cidx-artifact/v1', "
        "'semantic-catalog/v1', 'p', 'e', 'workspace:legacy', 'complete', "
        "'none', 'trusted', 'legacy', 'artifacts/a.db', 'hash', 1, 'current');"
        "UPDATE meta SET value = '35' WHERE key = 'schema_version';"
        "PRAGMA foreign_keys = ON;");
  }
  {
    cidx::Storage migrated(path.string());
    auto schema = migrated.raw_db().prepare(
        "SELECT value FROM meta WHERE key = 'schema_version'");
    REQUIRE(schema.step());
    CHECK(schema.col_text(0) == "36");
    auto contract = migrated.raw_db().prepare(
        "SELECT catalog_version, catalog_hash, trust, evidence FROM artifact");
    REQUIRE(contract.step());
    CHECK(contract.col_int64(0) == 0);
    CHECK(contract.col_text(1).empty());
    CHECK(contract.col_text(2) == "unverified");
    CHECK(contract.col_text(3) == "assumption");
    auto state = migrated.raw_db().prepare(
        "SELECT state, content_hash FROM artifact WHERE logical_id = 'legacy'");
    REQUIRE(state.step());
    CHECK(state.col_text(0) == "stale");
    CHECK(state.col_text(1) == "legacy-sha1:hash");
  }
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}
