// S-101: process-level failure atomicity for the clean-rebuild publication.
//
// These cases drive the real `cidx` binary over a real corpus, so they perform
// real parses and carry the "clang" label. Every case records the sha256 of the
// serving database before the attempt and asserts it byte-for-byte afterwards:
// extraction failure, verification failure, writer/commit failure and SIGINT
// during publication must all leave the database that is in service intact,
// readable, and unchanged.
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "application/clean_rebuild.hpp"
#include "storage/database_verification.hpp"
#include "storage/sqlite.hpp"
#include "storage/storage.hpp"

extern char **environ;

namespace {

std::string g_cidx_binary;

struct RunOutcome {
  int exit_code = -1;
  int signal_number = 0;
};

void write_file(const std::filesystem::path &path, const std::string &body) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << body;
}

std::string digest_of(const std::filesystem::path &path) {
  return cidx::application::clean_rebuild_file_digest(path.string())
      .value_or("<missing>");
}

// Spawn cidx with an explicit environment. When `interrupt_after_ms` is
// non-zero the child is sent SIGINT after that delay instead of being waited
// out, which is how the interrupted-publication case is produced.
RunOutcome run_cidx(const std::vector<std::string> &args,
                    const std::string &cache_dir,
                    const std::string &fail_at = {},
                    int interrupt_after_ms = 0) {
  std::vector<std::string> storage;
  storage.reserve(args.size() + 1);
  storage.push_back(g_cidx_binary);
  for (const std::string &arg : args) {
    storage.push_back(arg);
  }
  std::vector<char *> argv;
  argv.reserve(storage.size() + 1);
  for (std::string &value : storage) {
    argv.push_back(value.data());
  }
  argv.push_back(nullptr);

  std::vector<std::string> env_storage{"INDEXER_CACHE=" + cache_dir};
  for (char **entry = environ; *entry != nullptr; ++entry) {
    const std::string value(*entry);
    if (value.starts_with("INDEXER_CACHE=") ||
        value.starts_with("CIDX_CLEAN_REBUILD_FAIL_AT=")) {
      continue;
    }
    env_storage.push_back(value);
  }
  if (!fail_at.empty()) {
    env_storage.push_back("CIDX_CLEAN_REBUILD_FAIL_AT=" + fail_at);
  }
  std::vector<char *> envp;
  envp.reserve(env_storage.size() + 1);
  for (std::string &value : env_storage) {
    envp.push_back(value.data());
  }
  envp.push_back(nullptr);

  pid_t pid = 0;
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null",
                                   O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                   O_WRONLY, 0);
  const int rc = posix_spawn(&pid, g_cidx_binary.c_str(), &actions, nullptr,
                             argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);
  if (rc != 0) {
    return RunOutcome{.exit_code = 127, .signal_number = 0};
  }

  if (interrupt_after_ms > 0) {
    ::usleep(static_cast<useconds_t>(interrupt_after_ms) * 1000);
    ::kill(pid, SIGINT);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  RunOutcome outcome;
  if (WIFEXITED(status)) {
    outcome.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    outcome.signal_number = WTERMSIG(status);
    outcome.exit_code = -outcome.signal_number;
  }
  return outcome;
}

// A real, indexable corpus plus a populated index database.
struct Corpus {
  std::filesystem::path root;
  std::filesystem::path cache;
  std::filesystem::path index_path;
  bool ready = false;

  explicit Corpus(const char *suffix) {
    root = std::filesystem::temp_directory_path() /
           (std::string("cidx-clean-rebuild-proc-") + suffix);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root / "src");
    cache = root / "cache";
    index_path = cache / "index.db";

    write_file(root / "src" / "lib.hpp",
               "#pragma once\nint add(int a, int b);\nstruct Point { int x; "
               "int y; };\n");
    write_file(root / "src" / "lib.cpp",
               "#include \"lib.hpp\"\nint add(int a, int b) { return a + b; }\n"
               "int twice(int v) { return add(v, v); }\n");
    const std::string source = (root / "src" / "lib.cpp").string();
    write_file(root / "compile_commands.json",
               "[{\"directory\":\"" + root.string() + "\",\"file\":\"" +
                   source + "\",\"command\":\"c++ -std=c++17 -c " + source +
                   "\"}]");

    if (run_cidx({"import", "--db", root.string(), "--name", "corpus"},
                 cache.string())
            .exit_code != 0) {
      return;
    }
    if (run_cidx({"index"}, cache.string()).exit_code != 0) {
      return;
    }
    ready = std::filesystem::exists(index_path);
  }

  [[nodiscard]] std::vector<std::filesystem::path> leftover_candidates() const {
    std::vector<std::filesystem::path> found;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(cache, ec)) {
      if (entry.path().filename().string().starts_with(".cidx-rebuild-")) {
        found.push_back(entry.path());
      }
    }
    return found;
  }
};

} // namespace

TEST_SUITE("clang") {

  TEST_CASE("a clean rebuild publishes a verified candidate") {
    Corpus corpus("publish");
    REQUIRE(corpus.ready);
    const std::string before = digest_of(corpus.index_path);
    const std::string before_semantic =
        cidx::storage::read_database_semantic_digest(
            corpus.index_path.string());

    const RunOutcome outcome =
        run_cidx({"index", "rebuild", "--clean"}, corpus.cache.string());
    CHECK(outcome.exit_code == 0);

    // The file was replaced, and what replaced it is a healthy database holding
    // the same semantics as the index it superseded.
    CHECK(digest_of(corpus.index_path) != before);
    const cidx::storage::DatabaseIntegrityReport report =
        cidx::storage::inspect_database_integrity(corpus.index_path.string());
    CHECK(report.opened);
    CHECK(report.integrity_ok);
    CHECK(report.foreign_keys_ok);
    CHECK(report.schema_version == cidx::kSchemaVersion);
    CHECK(cidx::storage::read_database_pending_file_count(
              corpus.index_path.string()) == 0);
    CHECK(cidx::storage::read_database_semantic_digest(
              corpus.index_path.string()) == before_semantic);
    CHECK(corpus.leftover_candidates().empty());
  }

  // AC4, backup/recovery of the PUBLISHED output. A published database is only
  // useful if it can be backed up and the backup restored, so this takes a real
  // backup through SQLite's online backup API, restores it to a third path, and
  // holds the restored copy to every property the published file has to carry.
  TEST_CASE("a published database survives a backup and restore round trip") {
    Corpus corpus("backup-restore");
    REQUIRE(corpus.ready);

    REQUIRE(run_cidx({"index", "rebuild", "--clean"}, corpus.cache.string())
                .exit_code == 0);
    const std::string published_semantic =
        cidx::storage::read_database_semantic_digest(
            corpus.index_path.string());
    const cidx::storage::DatabaseCatalogIdentity published_catalog =
        cidx::storage::read_database_catalog_identity(
            corpus.index_path.string());

    const std::filesystem::path backup = corpus.cache / "backup.db";
    {
      // Read-only: taking a backup must not modify what it backs up.
      cidx::Storage published(corpus.index_path.string(),
                              cidx::Storage::OpenMode::read_only);
      published.backup_to(backup.string());
    }
    const std::string after_backup = digest_of(corpus.index_path);

    // Restore is a plain file copy of the backup to a new location, which is
    // what an operator's restore actually is.
    const std::filesystem::path restored = corpus.cache / "restored.db";
    std::filesystem::copy_file(backup, restored);

    const cidx::storage::DatabaseIntegrityReport report =
        cidx::storage::inspect_database_integrity(restored.string());
    CHECK(report.opened);
    CHECK(report.integrity_ok);
    CHECK(report.foreign_keys_ok);
    CHECK(report.schema_version == cidx::kSchemaVersion);
    CHECK(cidx::storage::read_database_pending_file_count(restored.string()) ==
          0);
    // The restored database holds the same facts and the same catalog.
    CHECK(cidx::storage::read_database_semantic_digest(restored.string()) ==
          published_semantic);
    CHECK(cidx::storage::read_database_catalog_identity(restored.string()) ==
          published_catalog);
    // Backing up did not disturb the published file, and the backup is itself
    // a self-contained rollback-journal database.
    CHECK(digest_of(corpus.index_path) == after_backup);
    CHECK_FALSE(
        cidx::storage::database_uses_write_ahead_log(restored.string()));

    // A restored database is a legitimate source for the next clean rebuild.
    CHECK(cidx::application::clean_rebuild_source_refusal(restored.string())
              .empty());
  }

  // AC4, migration compatibility, end to end through the real binary: an older
  // serving database is rebuilt and published at the current schema, and is
  // untouched until the rename.
  TEST_CASE("a clean rebuild brings an out-of-date serving database forward") {
    Corpus corpus("migrate");
    REQUIRE(corpus.ready);
    {
      cidx::SqliteDb raw(corpus.index_path.string());
      raw.exec("UPDATE meta SET value = '39' WHERE key = 'schema_version'");
    }
    REQUIRE(
        cidx::storage::inspect_database_integrity(corpus.index_path.string())
            .schema_version == 39);
    const std::string before = digest_of(corpus.index_path);

    const RunOutcome outcome =
        run_cidx({"index", "rebuild", "--clean"}, corpus.cache.string());
    CHECK(outcome.exit_code == 0);

    // Published at the current schema, from a database that was two versions
    // behind, and healthy.
    CHECK(digest_of(corpus.index_path) != before);
    const cidx::storage::DatabaseIntegrityReport report =
        cidx::storage::inspect_database_integrity(corpus.index_path.string());
    CHECK(report.opened);
    CHECK(report.integrity_ok);
    CHECK(report.foreign_keys_ok);
    CHECK(report.schema_version == cidx::kSchemaVersion);
    CHECK(cidx::storage::read_database_pending_file_count(
              corpus.index_path.string()) == 0);
    // Neither the candidate nor the migration copy outlives the run.
    CHECK(corpus.leftover_candidates().empty());
  }

  TEST_CASE("every injected failure leaves the serving database unchanged") {
    Corpus corpus("injected");
    REQUIRE(corpus.ready);
    const std::string before = digest_of(corpus.index_path);

    // The full publication sequence: inputs captured, candidate created, inputs
    // replayed, candidate indexed (extraction complete), verification complete,
    // and the final gate immediately before the rename.
    for (const char *point :
         {"after-inputs-captured", "after-candidate-created",
          "after-inputs-replayed", "after-candidate-indexed",
          "after-verification", "before-publication"}) {
      CAPTURE(point);
      const RunOutcome outcome = run_cidx({"index", "rebuild", "--clean"},
                                          corpus.cache.string(), point);
      CHECK(outcome.exit_code != 0);
      CHECK(digest_of(corpus.index_path) == before);
      CHECK(corpus.leftover_candidates().empty());
    }

    // Still readable and complete after six aborted attempts.
    const cidx::storage::DatabaseIntegrityReport report =
        cidx::storage::inspect_database_integrity(corpus.index_path.string());
    CHECK(report.opened);
    CHECK(report.integrity_ok);
    CHECK(cidx::storage::read_database_pending_file_count(
              corpus.index_path.string()) == 0);
  }

  // AC2, in-phase faults. The boundary points above abort BETWEEN phases and so
  // never observe a partially applied translation unit. These two abort INSIDE
  // the pipeline: `during-extraction` throws while that unit's facts are being
  // handed to the storage adapter, and `during-writer-commit` throws inside the
  // controlled writer as the unit of work commits, with its facts staged.
  TEST_CASE("an in-phase extraction or commit fault leaves the serving "
            "database unchanged") {
    Corpus corpus("in-phase");
    REQUIRE(corpus.ready);
    const std::string before = digest_of(corpus.index_path);
    const std::string before_semantic =
        cidx::storage::read_database_semantic_digest(
            corpus.index_path.string());

    for (const char *point : {"during-extraction", "during-writer-commit"}) {
      CAPTURE(point);
      const RunOutcome outcome = run_cidx({"index", "rebuild", "--clean"},
                                          corpus.cache.string(), point);
      // The candidate could not be completed, so nothing is publishable.
      CHECK(outcome.exit_code != 0);
      CHECK(digest_of(corpus.index_path) == before);
      CHECK(corpus.leftover_candidates().empty());
    }

    const cidx::storage::DatabaseIntegrityReport report =
        cidx::storage::inspect_database_integrity(corpus.index_path.string());
    CHECK(report.opened);
    CHECK(report.integrity_ok);
    CHECK(report.foreign_keys_ok);
    CHECK(cidx::storage::read_database_pending_file_count(
              corpus.index_path.string()) == 0);
    // Unchanged bytes already imply this; asserted separately so a failure says
    // which property broke.
    CHECK(cidx::storage::read_database_semantic_digest(
              corpus.index_path.string()) == before_semantic);
  }

  // AC2, SIGINT inside the publication window. The signal is delivered from the
  // publication sequence itself — after the candidate and the directory are on
  // stable storage, immediately before rename(2) — so the instant under test is
  // chosen, not raced from a wall clock.
  TEST_CASE("SIGINT between the fsync and the rename leaves the serving "
            "database intact") {
    Corpus corpus("sigint-publish");
    REQUIRE(corpus.ready);
    const std::string before = digest_of(corpus.index_path);

    const RunOutcome outcome =
        run_cidx({"index", "rebuild", "--clean"}, corpus.cache.string(),
                 "during-publication");
    CHECK(outcome.signal_number == SIGINT);

    CHECK(digest_of(corpus.index_path) == before);
    const cidx::storage::DatabaseIntegrityReport report =
        cidx::storage::inspect_database_integrity(corpus.index_path.string());
    CHECK(report.opened);
    CHECK(report.integrity_ok);
    CHECK(report.foreign_keys_ok);
    CHECK(cidx::storage::read_database_pending_file_count(
              corpus.index_path.string()) == 0);
  }

  // The other half of the window. Dying immediately after rename(2) returns is
  // the case the atomicity claim exists for: the serving path now holds the
  // candidate, and what a reader finds there must be a whole, healthy database
  // rather than a torn file. The process died before any cleanup could run.
  TEST_CASE("SIGINT immediately after the rename leaves a whole published "
            "database") {
    Corpus corpus("sigint-after-rename");
    REQUIRE(corpus.ready);
    const std::string before = digest_of(corpus.index_path);
    const std::string before_semantic =
        cidx::storage::read_database_semantic_digest(
            corpus.index_path.string());

    const RunOutcome outcome = run_cidx({"index", "rebuild", "--clean"},
                                        corpus.cache.string(), "after-rename");
    CHECK(outcome.signal_number == SIGINT);

    // The rename completed, so the serving path holds the candidate now.
    CHECK(digest_of(corpus.index_path) != before);
    const cidx::storage::DatabaseIntegrityReport report =
        cidx::storage::inspect_database_integrity(corpus.index_path.string());
    CHECK(report.opened);
    CHECK(report.integrity_ok);
    CHECK(report.foreign_keys_ok);
    CHECK(report.schema_version == cidx::kSchemaVersion);
    CHECK(cidx::storage::read_database_pending_file_count(
              corpus.index_path.string()) == 0);
    // Whole, not merely openable: it holds the same semantics as the index it
    // replaced.
    CHECK(cidx::storage::read_database_semantic_digest(
              corpus.index_path.string()) == before_semantic);
    CHECK(corpus.leftover_candidates().empty());
  }

  // AC4, backup/recovery. A sidecar beside the serving database means the main
  // file is not the whole database, so the rebuild is refused before it starts
  // rather than publishing over it.
  TEST_CASE("a clean rebuild refuses a serving database with a sidecar") {
    Corpus corpus("sidecar");
    REQUIRE(corpus.ready);
    const std::string before = digest_of(corpus.index_path);

    const std::filesystem::path sidecar =
        corpus.index_path.string() + std::string("-wal");
    write_file(sidecar, "leftover");

    const RunOutcome outcome =
        run_cidx({"index", "rebuild", "--clean"}, corpus.cache.string());
    CHECK(outcome.exit_code != 0);
    CHECK(digest_of(corpus.index_path) == before);
    CHECK(corpus.leftover_candidates().empty());

    std::error_code ignored;
    std::filesystem::remove(sidecar, ignored);
  }

  TEST_CASE(
      "SIGINT during a clean rebuild leaves the serving database intact") {
    Corpus corpus("sigint");
    REQUIRE(corpus.ready);
    const std::string before = digest_of(corpus.index_path);

    // Interrupt while the candidate is being built. The serving database is
    // only ever read at this point, so the kill can land anywhere before the
    // rename.
    const RunOutcome outcome = run_cidx({"index", "rebuild", "--clean"},
                                        corpus.cache.string(), {}, 40);
    CHECK(outcome.exit_code != 0);
    CHECK(outcome.signal_number == SIGINT);

    CHECK(digest_of(corpus.index_path) == before);
    const cidx::storage::DatabaseIntegrityReport report =
        cidx::storage::inspect_database_integrity(corpus.index_path.string());
    CHECK(report.opened);
    CHECK(report.integrity_ok);
    CHECK(report.foreign_keys_ok);
    CHECK(cidx::storage::read_database_pending_file_count(
              corpus.index_path.string()) == 0);
  }

  TEST_CASE("a clean rebuild refuses partial scopes") {
    Corpus corpus("scope");
    REQUIRE(corpus.ready);
    const std::string before = digest_of(corpus.index_path);

    CHECK(run_cidx({"index", "rebuild", "--clean", "--source", "corpus"},
                   corpus.cache.string())
              .exit_code != 0);
    CHECK(run_cidx({"index", "rebuild", "--clean", "src/lib.cpp"},
                   corpus.cache.string())
              .exit_code != 0);
    // `--clean` without `rebuild` is a usage error (exit 2), not a rebuild.
    CHECK(run_cidx({"index", "--clean"}, corpus.cache.string()).exit_code == 2);

    CHECK(digest_of(corpus.index_path) == before);
    CHECK(corpus.leftover_candidates().empty());
  }

} // TEST_SUITE("clang")

int main(int argc, char **argv) {
  doctest::Context context;
  std::vector<const char *> forwarded;
  forwarded.push_back(argv[0]);
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (g_cidx_binary.empty() && !arg.starts_with("-")) {
      g_cidx_binary = arg;
      continue;
    }
    forwarded.push_back(argv[i]);
  }
  if (g_cidx_binary.empty()) {
    return 77; // no binary handed over: the harness skips this gate
  }
  context.applyCommandLine(static_cast<int>(forwarded.size()),
                           forwarded.data());
  return context.run();
}
