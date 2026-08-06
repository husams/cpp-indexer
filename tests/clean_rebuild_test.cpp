// S-101: clean rebuild — input capture, candidate verification, and verified
// atomic publication.
//
// The contract these cases hold the implementation to is that the database in
// service is never edited: it is read to capture the rebuild inputs and then
// replaced, in one rename, only by a candidate that passed every check. Each
// failure case therefore asserts the serving database's sha256 is byte-for-byte
// what it was before the attempt.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "application/clean_rebuild.hpp"
#include "storage/database_verification.hpp"
#include "storage/sqlite.hpp"
#include "storage/storage.hpp"
#include "util/hashing.hpp"

namespace {

using cidx::Storage;
using cidx::application::capture_clean_rebuild_inputs;
using cidx::application::clean_rebuild_candidate_path;
using cidx::application::clean_rebuild_failure_point_from_name;
using cidx::application::clean_rebuild_failure_point_name;
using cidx::application::clean_rebuild_file_digest;
using cidx::application::clean_rebuild_source_refusal;
using cidx::application::CleanRebuildFailurePoint;
using cidx::application::CleanRebuildInputs;
using cidx::application::CleanRebuildVerification;
using cidx::application::publish_clean_rebuild_candidate;
using cidx::application::replay_clean_rebuild_inputs;
using cidx::application::verify_clean_rebuild_candidate;

std::filesystem::path test_root(const char *suffix) {
  const auto root = std::filesystem::temp_directory_path() /
                    (std::string("cidx-clean-rebuild-") + suffix);
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::create_directories(root);
  return root;
}

void write_file(const std::filesystem::path &path, const std::string &body) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << body;
}

// A serving database with the catalog shapes a rebuild has to reproduce: two
// semantic universes, a repository with two clones (the second one active), a
// grouped component stored clone-relative, an ungrouped component, a versioned
// component, a label, and files carrying compile options and a driver.
struct Fixture {
  std::filesystem::path root;
  std::filesystem::path index_path;
  std::filesystem::path source_root;

  explicit Fixture(const char *suffix) : root(test_root(suffix)) {
    index_path = root / "index.db";
    source_root = root / "checkout";
    std::filesystem::create_directories(source_root / "src");
    write_file(source_root / "src" / "a.cpp", "int a() { return 1; }\n");
    write_file(source_root / "src" / "b.cpp", "int b() { return 2; }\n");
    write_file(source_root / "include" / "a.hpp", "int a();\n");

    Storage db(index_path.string());
    const std::int64_t universe =
        db.add_semantic_universe("workspace:test", "Test", "explicit");
    db.add_semantic_universe("workspace:other", "Other", "explicit");
    const std::int64_t repository = db.add_repository(
        "checkout", "repo", "git@example.com:acme/x.git", universe);
    db.add_clone(repository, (root / "stale-clone").string(), "stale");
    const std::int64_t active =
        db.add_clone(repository, source_root.string(), "primary");
    db.set_active_clone(repository, active);

    const std::int64_t grouped =
        db.add_component("checkout", source_root.string(), "repo");
    db.set_component_repository(grouped, repository);
    db.relativize_component(grouped, source_root.string());

    const std::int64_t external = db.add_component(
        "vendor", (root / "vendor").string(), "external", "1.4.0");
    db.set_component_semantic_universe(external, universe);

    db.add_label("acme-include", "$ACME/include");

    db.add_file_path((source_root / "src" / "a.cpp").string(), 1.0, "aaa",
                     std::vector<std::string>{"-std=c++17", "-DA=1"}, "c++");
    db.add_file_path((source_root / "src" / "b.cpp").string(), 2.0, "bbb",
                     std::vector<std::string>{"-std=c++20"}, "c++");
    db.add_file_path((source_root / "include" / "a.hpp").string(), 3.0, "ccc",
                     std::vector<std::string>{"-std=c++17"}, "c++");
  }
};

std::string digest_of(const std::filesystem::path &path) {
  return clean_rebuild_file_digest(path.string()).value_or("<missing>");
}

} // namespace

TEST_CASE("failure point names round-trip and reject unknown spellings") {
  for (const CleanRebuildFailurePoint point :
       {CleanRebuildFailurePoint::none,
        CleanRebuildFailurePoint::after_inputs_captured,
        CleanRebuildFailurePoint::after_candidate_created,
        CleanRebuildFailurePoint::after_inputs_replayed,
        CleanRebuildFailurePoint::during_extraction,
        CleanRebuildFailurePoint::during_writer_commit,
        CleanRebuildFailurePoint::after_candidate_indexed,
        CleanRebuildFailurePoint::after_verification,
        CleanRebuildFailurePoint::before_publication,
        CleanRebuildFailurePoint::during_publication,
        CleanRebuildFailurePoint::after_rename}) {
    const std::string name(clean_rebuild_failure_point_name(point));
    const auto parsed = clean_rebuild_failure_point_from_name(name);
    REQUIRE(parsed.has_value());
    CHECK(*parsed == point);
  }
  CHECK_FALSE(clean_rebuild_failure_point_from_name("publish").has_value());
  CHECK_FALSE(clean_rebuild_failure_point_from_name("").has_value());
}

// The four in-phase points are the ones AC2 names. They must be spellable and
// must be distinct from the boundary points they are often confused with: a
// fault BETWEEN phases proves nothing about a fault INSIDE one.
TEST_CASE("the in-phase failure points are distinct from the boundaries") {
  const auto point = [](const char *name) {
    const auto parsed = clean_rebuild_failure_point_from_name(name);
    REQUIRE(parsed.has_value());
    return *parsed;
  };
  CHECK(point("during-extraction") ==
        CleanRebuildFailurePoint::during_extraction);
  CHECK(point("during-writer-commit") ==
        CleanRebuildFailurePoint::during_writer_commit);
  CHECK(point("during-publication") ==
        CleanRebuildFailurePoint::during_publication);
  CHECK(point("after-rename") == CleanRebuildFailurePoint::after_rename);

  CHECK(point("during-extraction") !=
        point("after-candidate-indexed")); // extraction: inside vs after
  CHECK(point("during-writer-commit") !=
        point("after-candidate-indexed")); // commit: inside vs after
  CHECK(point("during-publication") !=
        point("before-publication")); // rename window: inside vs before
}

TEST_CASE("the candidate is a sibling of the target so publication renames") {
  const std::string candidate =
      clean_rebuild_candidate_path("/var/data/cidx/index.db");
  CHECK(std::filesystem::path(candidate).parent_path() ==
        std::filesystem::path("/var/data/cidx"));
  CHECK(candidate != "/var/data/cidx/index.db");
}

TEST_CASE("capture and replay reproduce the catalog exactly") {
  const Fixture fixture("capture-replay");
  const CleanRebuildInputs inputs =
      capture_clean_rebuild_inputs(fixture.index_path.string());

  // The schema seeds the `legacy` universe, so the fixture's two explicit
  // universes make three in total; all of them must be replayed.
  CHECK(inputs.universes.size() == 3);
  CHECK(inputs.repositories.size() == 1);
  CHECK(inputs.clones.size() == 2);
  CHECK(inputs.components.size() == 2);
  CHECK(inputs.files.size() == 3);
  CHECK(inputs.labels.size() == 1);
  CHECK_FALSE(inputs.source_digest.empty());

  const auto candidate_path = fixture.root / "candidate.db";
  {
    Storage candidate(candidate_path.string());
    replay_clean_rebuild_inputs(candidate, inputs);
  }

  const auto expected = cidx::storage::read_database_catalog_identity(
      fixture.index_path.string());
  const auto actual =
      cidx::storage::read_database_catalog_identity(candidate_path.string());
  CHECK(actual.components == expected.components);
  CHECK(actual.files == expected.files);
  CHECK(actual.clones == expected.clones);
  CHECK(actual.labels == expected.labels);
  // The digest covers the clone-relative component path, the active clone, the
  // versioned component and every file's compile options, so equality here is
  // the real assertion that nothing about the inputs drifted.
  CHECK(actual.digest == expected.digest);
  CHECK(actual == expected);
}

TEST_CASE("capturing inputs does not modify the serving database") {
  const Fixture fixture("capture-readonly");
  const std::string before = digest_of(fixture.index_path);
  const CleanRebuildInputs inputs =
      capture_clean_rebuild_inputs(fixture.index_path.string());
  CHECK(inputs.source_digest == before);
  CHECK(digest_of(fixture.index_path) == before);
}

TEST_CASE("capture of a missing database yields empty inputs") {
  const auto root = test_root("missing");
  const CleanRebuildInputs inputs =
      capture_clean_rebuild_inputs((root / "absent.db").string());
  CHECK(inputs.files.empty());
  CHECK(inputs.components.empty());
  CHECK(inputs.source_digest.empty());
}

// AC4, migration compatibility -- the version reported must be the version ON
// DISK, and the captured catalog must travel with the inputs rather than being
// re-read from the serving file afterwards.
TEST_CASE("capture reports the schema version the serving database carries") {
  const Fixture fixture("capture-schema");
  const CleanRebuildInputs inputs =
      capture_clean_rebuild_inputs(fixture.index_path.string());
  CHECK(inputs.schema_version == cidx::kSchemaVersion);
  CHECK_FALSE(inputs.migrated_for_capture);
  CHECK(inputs.catalog == cidx::storage::read_database_catalog_identity(
                              fixture.index_path.string()));
}

TEST_CASE(
    "a current, self-contained serving database is an acceptable source") {
  const Fixture fixture("source-ok");
  CHECK(clean_rebuild_source_refusal(fixture.index_path.string()).empty());
}

TEST_CASE("no database in service is an acceptable source") {
  const auto root = test_root("source-absent");
  CHECK(clean_rebuild_source_refusal((root / "absent.db").string()).empty());
}

// AC4, migration compatibility -- the property the criterion actually asks for.
// An older serving database is NOT refused: it is brought forward on a PRIVATE
// COPY, the inputs are captured from that copy, and the file in service is left
// byte-for-byte unchanged. Preservation, not refusal.
TEST_CASE("an out-of-date serving database is migrated on a private copy") {
  const Fixture fixture("capture-migrate");
  const auto expected = cidx::storage::read_database_catalog_identity(
      fixture.index_path.string());
  {
    cidx::SqliteDb raw(fixture.index_path.string());
    raw.exec("UPDATE meta SET value = '39' WHERE key = 'schema_version'");
  }
  const std::string before = digest_of(fixture.index_path);
  REQUIRE(clean_rebuild_source_refusal(fixture.index_path.string()).empty());

  const CleanRebuildInputs inputs =
      capture_clean_rebuild_inputs(fixture.index_path.string());
  CHECK(inputs.migrated_for_capture);
  CHECK(inputs.schema_version == 39);
  // The catalog survived the migration intact: this is the "preserves" half.
  CHECK(inputs.catalog == expected);
  CHECK(inputs.files.size() == 3);
  CHECK(inputs.components.size() == 2);

  // And the database in service was never written to.
  CHECK(digest_of(fixture.index_path) == before);
  CHECK(cidx::storage::inspect_database_integrity(fixture.index_path.string())
            .schema_version == 39);
  // The working copy is a working file, not a result.
  CHECK_FALSE(std::filesystem::exists(
      cidx::application::clean_rebuild_migration_copy_path(
          fixture.index_path.string())));
}

// The other direction is refused: this binary cannot bring a future schema
// backwards, and must say so rather than misread it.
TEST_CASE("a newer-than-binary serving database is refused") {
  const Fixture fixture("source-newer");
  {
    cidx::SqliteDb raw(fixture.index_path.string());
    raw.exec("UPDATE meta SET value = '9999' WHERE key = 'schema_version'");
  }
  const std::string refusal =
      clean_rebuild_source_refusal(fixture.index_path.string());
  REQUIRE_FALSE(refusal.empty());
  CHECK(refusal.contains("schema v9999"));
  CHECK(refusal.contains("cannot downgrade"));
}

// AC4, backup/recovery. Publication replaces the main database file with one
// rename. In WAL mode the committed content is not all in that file, so the
// rename would publish a main file against a sidecar describing a different
// database. Refused before anything is built, and named as WAL rather than as
// a generic read failure — a WAL database cannot be opened by the read-only
// qualification path at all, so the header is what has to answer.
TEST_CASE("a WAL-mode serving database is refused") {
  const Fixture fixture("source-wal");
  CHECK_FALSE(cidx::storage::database_uses_write_ahead_log(
      fixture.index_path.string()));
  {
    cidx::SqliteDb raw(fixture.index_path.string());
    raw.exec("PRAGMA journal_mode = WAL");
  }
  // Sidecars are removed so the header check, not the sidecar check, is what
  // refuses: WAL mode is persisted in the database file header.
  for (const std::string &sidecar :
       cidx::storage::database_sidecar_paths(fixture.index_path.string())) {
    std::error_code ignored;
    std::filesystem::remove(sidecar, ignored);
  }
  CHECK(cidx::storage::database_uses_write_ahead_log(
      fixture.index_path.string()));

  const std::string refusal =
      clean_rebuild_source_refusal(fixture.index_path.string());
  REQUIRE_FALSE(refusal.empty());
  CHECK(refusal.contains("WAL journal mode"));
}

// The other rollback modes are NOT refused, and that is deliberate: TRUNCATE
// and PERSIST leave the committed database wholly inside the main file exactly
// as DELETE does, so a rename does replace the database. What they can leave
// behind is a -journal sidecar, which the sidecar check above covers.
TEST_CASE("the other rollback journal modes are accepted") {
  for (const char *mode : {"TRUNCATE", "PERSIST", "MEMORY", "OFF"}) {
    CAPTURE(mode);
    const Fixture fixture("source-rollback");
    {
      cidx::SqliteDb raw(fixture.index_path.string());
      raw.exec(std::string("PRAGMA journal_mode = ") + mode);
    }
    for (const std::string &sidecar :
         cidx::storage::database_sidecar_paths(fixture.index_path.string())) {
      std::error_code ignored;
      std::filesystem::remove(sidecar, ignored);
    }
    CHECK_FALSE(cidx::storage::database_uses_write_ahead_log(
        fixture.index_path.string()));
    CHECK(clean_rebuild_source_refusal(fixture.index_path.string()).empty());
  }
}

// AC4, backup/recovery. Even in DELETE mode a leftover sidecar means the main
// file is not the whole database; publishing over it would strand state.
TEST_CASE("a serving database with a stranded sidecar is refused") {
  for (const char *suffix : {"-wal", "-shm", "-journal"}) {
    CAPTURE(suffix);
    const Fixture fixture("source-sidecar");
    const std::string sidecar = fixture.index_path.string() + suffix;
    write_file(sidecar, "leftover");

    const std::string refusal =
        clean_rebuild_source_refusal(fixture.index_path.string());
    REQUIRE_FALSE(refusal.empty());
    CHECK(refusal.contains("sidecar"));
  }
}

TEST_CASE("verification accepts a faithful candidate") {
  const Fixture fixture("verify-ok");
  const CleanRebuildInputs inputs =
      capture_clean_rebuild_inputs(fixture.index_path.string());
  const auto candidate_path = fixture.root / "candidate.db";
  {
    Storage candidate(candidate_path.string());
    replay_clean_rebuild_inputs(candidate, inputs);
    // A rebuild is only publishable once every replayed file is indexed.
    for (const auto &[file, path] : candidate.list_files()) {
      candidate.set_file_indexed(file.id, true);
    }
  }
  const auto expected = cidx::storage::read_database_catalog_identity(
      fixture.index_path.string());
  const CleanRebuildVerification checks =
      verify_clean_rebuild_candidate(candidate_path.string(), expected, true);
  CHECK(checks.opened);
  CHECK(checks.integrity_ok);
  CHECK(checks.foreign_keys_ok);
  CHECK(checks.schema_ok);
  CHECK(checks.catalog_ok);
  CHECK(checks.complete);
  CHECK(checks.schema_version == cidx::kSchemaVersion);
  CHECK(checks.passed());
  CHECK(checks.failure_reason().empty());
  CHECK_FALSE(checks.semantic_digest.empty());
}

TEST_CASE("verification refuses a candidate with pending files") {
  const Fixture fixture("verify-pending");
  const CleanRebuildInputs inputs =
      capture_clean_rebuild_inputs(fixture.index_path.string());
  const auto candidate_path = fixture.root / "candidate.db";
  {
    Storage candidate(candidate_path.string());
    replay_clean_rebuild_inputs(candidate, inputs);
  }
  const auto expected = cidx::storage::read_database_catalog_identity(
      fixture.index_path.string());
  const CleanRebuildVerification checks =
      verify_clean_rebuild_candidate(candidate_path.string(), expected, true);
  CHECK(checks.catalog_ok);
  CHECK_FALSE(checks.complete);
  CHECK(checks.files_pending == 3);
  CHECK_FALSE(checks.passed());
  CHECK(checks.failure_reason().find("pending") != std::string::npos);
}

TEST_CASE("verification refuses a candidate whose catalog drifted") {
  const Fixture fixture("verify-catalog");
  const CleanRebuildInputs inputs =
      capture_clean_rebuild_inputs(fixture.index_path.string());
  const auto candidate_path = fixture.root / "candidate.db";
  {
    Storage candidate(candidate_path.string());
    replay_clean_rebuild_inputs(candidate, inputs);
    // One extra registered file is a catalog the serving database never had.
    candidate.add_file_path((fixture.source_root / "src" / "c.cpp").string(),
                            4.0, "ddd", std::vector<std::string>{"-std=c++17"},
                            "c++");
    for (const auto &[file, path] : candidate.list_files()) {
      candidate.set_file_indexed(file.id, true);
    }
  }
  const auto expected = cidx::storage::read_database_catalog_identity(
      fixture.index_path.string());
  const CleanRebuildVerification checks =
      verify_clean_rebuild_candidate(candidate_path.string(), expected, true);
  CHECK_FALSE(checks.catalog_ok);
  CHECK_FALSE(checks.passed());
  CHECK(checks.failure_reason().find("catalog") != std::string::npos);
}

TEST_CASE("verification refuses a file that is not a database") {
  const auto root = test_root("verify-garbage");
  const auto candidate_path = root / "candidate.db";
  write_file(candidate_path, "this is not a SQLite database at all");
  const CleanRebuildVerification checks = verify_clean_rebuild_candidate(
      candidate_path.string(), cidx::storage::DatabaseCatalogIdentity{}, true);
  CHECK_FALSE(checks.passed());
  CHECK_FALSE(checks.failure_reason().empty());
}

TEST_CASE("publication replaces the target atomically") {
  const Fixture fixture("publish-ok");
  const std::string source_digest = digest_of(fixture.index_path);
  const auto candidate_path = fixture.root / "candidate.db";
  {
    Storage candidate(candidate_path.string());
    candidate.add_component("published", (fixture.root / "p").string(), "repo");
  }
  const std::string candidate_digest = digest_of(candidate_path);
  REQUIRE(candidate_digest != source_digest);

  const std::string refusal = publish_clean_rebuild_candidate(
      candidate_path.string(), fixture.index_path.string(), source_digest,
      CleanRebuildFailurePoint::none);
  CHECK(refusal.empty());
  CHECK(digest_of(fixture.index_path) == candidate_digest);
  CHECK_FALSE(std::filesystem::exists(candidate_path));
}

// AC4, backup/recovery. The precondition is re-established at the rename
// itself, not only at capture: a sidecar that appeared beside EITHER file while
// the rebuild ran means a bare rename no longer replaces a whole database.
TEST_CASE("publication is refused when a sidecar sits beside either file") {
  for (const bool beside_target : {true, false}) {
    CAPTURE(beside_target);
    const Fixture fixture("publish-sidecar");
    const std::string source_digest = digest_of(fixture.index_path);
    const auto candidate_path = fixture.root / "candidate.db";
    {
      Storage candidate(candidate_path.string());
      candidate.add_component("published", (fixture.root / "p").string(),
                              "repo");
    }
    write_file((beside_target ? fixture.index_path.string()
                              : candidate_path.string()) +
                   "-wal",
               "leftover");

    const std::string refusal = publish_clean_rebuild_candidate(
        candidate_path.string(), fixture.index_path.string(), source_digest,
        CleanRebuildFailurePoint::none);
    REQUIRE_FALSE(refusal.empty());
    CHECK(refusal.contains("-wal"));
    // Refused means refused: the serving database is still the one captured and
    // the candidate is still on disk for the caller to clean up.
    CHECK(digest_of(fixture.index_path) == source_digest);
    CHECK(std::filesystem::exists(candidate_path));
  }
}

TEST_CASE("publication is refused when the serving database changed") {
  const Fixture fixture("publish-concurrent");
  const std::string captured = digest_of(fixture.index_path);
  const auto candidate_path = fixture.root / "candidate.db";
  {
    Storage candidate(candidate_path.string());
    candidate.add_component("published", (fixture.root / "p").string(), "repo");
  }
  // A concurrent writer touches the serving database after capture.
  {
    Storage serving(fixture.index_path.string());
    serving.add_component("late", (fixture.root / "late").string(), "repo");
  }
  const std::string mutated = digest_of(fixture.index_path);
  REQUIRE(mutated != captured);

  const std::string refusal = publish_clean_rebuild_candidate(
      candidate_path.string(), fixture.index_path.string(), captured,
      CleanRebuildFailurePoint::none);
  CHECK_FALSE(refusal.empty());
  CHECK(refusal.find("changed") != std::string::npos);
  // The concurrent writer's database survives untouched, and the candidate is
  // still on disk for the caller to clean up.
  CHECK(digest_of(fixture.index_path) == mutated);
  CHECK(std::filesystem::exists(candidate_path));
}

TEST_CASE("publication is refused when the serving database disappeared") {
  const Fixture fixture("publish-vanished");
  const std::string captured = digest_of(fixture.index_path);
  const auto candidate_path = fixture.root / "candidate.db";
  {
    Storage candidate(candidate_path.string());
    candidate.add_component("published", (fixture.root / "p").string(), "repo");
  }
  std::filesystem::remove(fixture.index_path);
  const std::string refusal = publish_clean_rebuild_candidate(
      candidate_path.string(), fixture.index_path.string(), captured,
      CleanRebuildFailurePoint::none);
  CHECK_FALSE(refusal.empty());
  CHECK(refusal.find("disappeared") != std::string::npos);
  CHECK_FALSE(std::filesystem::exists(fixture.index_path));
}

TEST_CASE("publication is refused when a database appeared unexpectedly") {
  const auto root = test_root("publish-appeared");
  const auto index_path = root / "index.db";
  const auto candidate_path = root / "candidate.db";
  {
    Storage candidate(candidate_path.string());
    candidate.add_component("published", (root / "p").string(), "repo");
  }
  {
    Storage appeared(index_path.string());
    appeared.add_component("appeared", (root / "a").string(), "repo");
  }
  const std::string appeared_digest = digest_of(index_path);
  const std::string refusal = publish_clean_rebuild_candidate(
      candidate_path.string(), index_path.string(), "",
      CleanRebuildFailurePoint::none);
  CHECK_FALSE(refusal.empty());
  CHECK(digest_of(index_path) == appeared_digest);
}

TEST_CASE("an injected failure before publication leaves the target intact") {
  const Fixture fixture("publish-injected");
  const std::string captured = digest_of(fixture.index_path);
  const auto candidate_path = fixture.root / "candidate.db";
  {
    Storage candidate(candidate_path.string());
    candidate.add_component("published", (fixture.root / "p").string(), "repo");
  }
  const std::string refusal = publish_clean_rebuild_candidate(
      candidate_path.string(), fixture.index_path.string(), captured,
      CleanRebuildFailurePoint::before_publication);
  CHECK_FALSE(refusal.empty());
  CHECK(refusal.find("injected failure") != std::string::npos);
  CHECK(digest_of(fixture.index_path) == captured);
  CHECK(std::filesystem::exists(candidate_path));
}

TEST_CASE("publication of a first database needs no predecessor") {
  const auto root = test_root("publish-first");
  const auto index_path = root / "index.db";
  const auto candidate_path = root / "candidate.db";
  {
    Storage candidate(candidate_path.string());
    candidate.add_component("first", (root / "f").string(), "repo");
  }
  const std::string candidate_digest = digest_of(candidate_path);
  const std::string refusal = publish_clean_rebuild_candidate(
      candidate_path.string(), index_path.string(), "",
      CleanRebuildFailurePoint::none);
  CHECK(refusal.empty());
  CHECK(digest_of(index_path) == candidate_digest);
}

TEST_CASE("the semantic digest is stable and content-sensitive") {
  const Fixture fixture("semantic-digest");
  const std::string first =
      cidx::storage::read_database_semantic_digest(fixture.index_path.string());
  const std::string second =
      cidx::storage::read_database_semantic_digest(fixture.index_path.string());
  CHECK(first == second);

  // Two independently built databases holding the same content agree.
  const CleanRebuildInputs inputs =
      capture_clean_rebuild_inputs(fixture.index_path.string());
  const auto twin_path = fixture.root / "twin.db";
  {
    Storage twin(twin_path.string());
    replay_clean_rebuild_inputs(twin, inputs);
  }
  CHECK(cidx::storage::read_database_semantic_digest(twin_path.string()) ==
        first);

  // A new registered file is new content and must move the digest.
  {
    Storage twin(twin_path.string());
    twin.add_file_path((fixture.source_root / "src" / "z.cpp").string(), 9.0,
                       "zzz", std::vector<std::string>{"-std=c++17"}, "c++");
  }
  CHECK(cidx::storage::read_database_semantic_digest(twin_path.string()) !=
        first);
}

TEST_CASE("the semantic digest covers the published fact families") {
  const auto families = cidx::storage::semantic_digest_fact_families();
  CHECK(families.size() == 6);
  for (const std::string_view family :
       {"file", "symbol", "edge", "definition", "diagnostic", "include_edge"}) {
    CHECK(std::ranges::find(families, family) != families.end());
  }
}

TEST_CASE("integrity inspection reports a readable database and a broken one") {
  const Fixture fixture("integrity");
  const cidx::storage::DatabaseIntegrityReport good =
      cidx::storage::inspect_database_integrity(fixture.index_path.string());
  CHECK(good.opened);
  CHECK(good.integrity_ok);
  CHECK(good.foreign_keys_ok);
  CHECK(good.schema_version == cidx::kSchemaVersion);

  const auto broken = fixture.root / "broken.db";
  write_file(broken, "not a database");
  const cidx::storage::DatabaseIntegrityReport bad =
      cidx::storage::inspect_database_integrity(broken.string());
  // A file that is not a database either refuses to open or fails
  // integrity_check; either way it must never look healthy.
  const bool looks_healthy = bad.opened && bad.integrity_ok;
  CHECK_FALSE(looks_healthy);
  CHECK_FALSE(bad.detail.empty());
}
