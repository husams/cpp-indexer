#include "application/clean_rebuild.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <system_error>
#include <utility>

#include "storage/storage.hpp"
#include "util/durable_publish.hpp"
#include "util/env.hpp"
#include "util/errors.hpp"
#include "util/hashing.hpp"

namespace cidx::application {
namespace {

struct FailurePointName {
  CleanRebuildFailurePoint point;
  std::string_view name;
};

constexpr std::array<FailurePointName, 11> kFailurePointNames{{
    {.point = CleanRebuildFailurePoint::none, .name = "none"},
    {.point = CleanRebuildFailurePoint::after_inputs_captured,
     .name = "after-inputs-captured"},
    {.point = CleanRebuildFailurePoint::after_candidate_created,
     .name = "after-candidate-created"},
    {.point = CleanRebuildFailurePoint::after_inputs_replayed,
     .name = "after-inputs-replayed"},
    {.point = CleanRebuildFailurePoint::during_extraction,
     .name = "during-extraction"},
    {.point = CleanRebuildFailurePoint::during_writer_commit,
     .name = "during-writer-commit"},
    {.point = CleanRebuildFailurePoint::after_candidate_indexed,
     .name = "after-candidate-indexed"},
    {.point = CleanRebuildFailurePoint::after_verification,
     .name = "after-verification"},
    {.point = CleanRebuildFailurePoint::before_publication,
     .name = "before-publication"},
    {.point = CleanRebuildFailurePoint::during_publication,
     .name = "during-publication"},
    {.point = CleanRebuildFailurePoint::after_rename, .name = "after-rename"},
}};

auto parent_directory(const std::string &path) -> std::string {
  const std::filesystem::path parent =
      std::filesystem::path(path).parent_path();
  return parent.empty() ? std::string(".") : parent.string();
}

// Owns the private copy a capture migrates. The copy is a working file, not a
// result: it must not outlive the capture, including on the throwing path, or
// the next run would find a stale sibling.
class MigrationCopy {
public:
  explicit MigrationCopy(const std::string &path)
      : file_(path), journal_(path + "-journal") {}
  ~MigrationCopy() noexcept {
    std::error_code discard;
    std::filesystem::remove(file_, discard);
    std::filesystem::remove(journal_, discard);
  }
  MigrationCopy(const MigrationCopy &) = delete;
  auto operator=(const MigrationCopy &) -> MigrationCopy & = delete;
  MigrationCopy(MigrationCopy &&) = delete;
  auto operator=(MigrationCopy &&) -> MigrationCopy & = delete;

private:
  // Held as paths so removal constructs nothing on the destructor path.
  std::filesystem::path file_;
  std::filesystem::path journal_;
};

} // namespace

auto clean_rebuild_failure_point_name(CleanRebuildFailurePoint point)
    -> std::string_view {
  for (const FailurePointName &entry : kFailurePointNames) {
    if (entry.point == point) {
      return entry.name;
    }
  }
  return "none";
}

auto clean_rebuild_failure_point_from_name(std::string_view name)
    -> std::optional<CleanRebuildFailurePoint> {
  for (const FailurePointName &entry : kFailurePointNames) {
    if (entry.name == name) {
      return entry.point;
    }
  }
  return std::nullopt;
}

auto clean_rebuild_failure_point_from_environment()
    -> CleanRebuildFailurePoint {
  const std::optional<std::string> value =
      get_env("CIDX_CLEAN_REBUILD_FAIL_AT");
  if (!value || value->empty()) {
    return CleanRebuildFailurePoint::none;
  }
  return clean_rebuild_failure_point_from_name(*value).value_or(
      CleanRebuildFailurePoint::none);
}

auto CleanRebuildVerification::failure_reason() const -> std::string {
  if (!opened) {
    return "candidate database cannot be opened: " + integrity_detail;
  }
  if (!integrity_ok) {
    return "candidate failed integrity_check: " + integrity_detail;
  }
  if (!foreign_keys_ok) {
    return "candidate has " + std::to_string(foreign_key_violations) +
           " foreign-key violation(s)";
  }
  if (!schema_ok) {
    return "candidate schema is v" + std::to_string(schema_version) +
           ", expected v" + std::to_string(kSchemaVersion);
  }
  if (!catalog_ok) {
    return "candidate catalog identity does not match the captured inputs";
  }
  if (!complete) {
    return "candidate still has " + std::to_string(files_pending) +
           " pending file(s)";
  }
  return {};
}

auto clean_rebuild_file_digest(const std::string &path)
    -> std::optional<std::string> {
  return sha256_of(path);
}

auto clean_rebuild_migration_copy_path(const std::string &index_path)
    -> std::string {
  const std::filesystem::path target(index_path);
  const std::string name = ".cidx-rebuild-migrate-" +
                           std::to_string(util::current_process_id()) + "-" +
                           target.filename().string();
  return (std::filesystem::path(parent_directory(index_path)) / name).string();
}

auto clean_rebuild_candidate_path(const std::string &index_path)
    -> std::string {
  const std::filesystem::path target(index_path);
  const std::string name = ".cidx-rebuild-" +
                           std::to_string(util::current_process_id()) + "-" +
                           target.filename().string();
  return (std::filesystem::path(parent_directory(index_path)) / name).string();
}

auto clean_rebuild_source_refusal(const std::string &index_path)
    -> std::string {
  std::error_code ec;
  if (!std::filesystem::exists(index_path, ec) || ec) {
    // Nothing in service: no catalog to misread, no sidecar to strand.
    return {};
  }

  // A stranded sidecar is checked first: it is the condition under which the
  // main file is not the whole database, so nothing read out of the main file
  // can be trusted to describe what is in service.
  for (const std::string &sidecar :
       storage::database_sidecar_paths(index_path)) {
    if (std::filesystem::exists(sidecar, ec) && !ec) {
      return "the index database at " + index_path + " has a sidecar file (" +
             std::filesystem::path(sidecar).filename().string() +
             "). A clean rebuild publishes by replacing the main database file "
             "alone, which would strand that sidecar; close every writer, or "
             "checkpoint the database into DELETE journal mode, and retry";
    }
  }

  // Read from the file header, and read BEFORE anything tries to open the
  // database: a WAL database cannot be opened by the read-only qualification
  // path at all, so asking the integrity report first would report "cannot
  // read" and hide the actual, fixable reason.
  if (storage::database_uses_write_ahead_log(index_path)) {
    return "the index database at " + index_path +
           " is in WAL journal mode. Atomic publication replaces the main "
           "database file with one rename, which replaces the database only "
           "when the database is wholly contained in that file; a WAL "
           "database's committed content also lives in its -wal sidecar. "
           "Checkpoint it into a rollback journal mode first";
  }

  const storage::DatabaseIntegrityReport report =
      storage::inspect_database_integrity(index_path);
  if (!report.opened) {
    return "cannot read the index database at " + index_path + ": " +
           report.detail;
  }
  // Older is fine — the capture migrates a private copy. Newer is not: this
  // binary has no way to bring a future schema backwards.
  if (report.schema_version > kSchemaVersion) {
    return "the index database at " + index_path + " is schema v" +
           std::to_string(report.schema_version) + " but this binary builds v" +
           std::to_string(kSchemaVersion) +
           ". A clean rebuild cannot downgrade a database; use a build that "
           "matches it";
  }
  return {};
}

auto capture_clean_rebuild_inputs(const std::string &index_path)
    -> CleanRebuildInputs {
  CleanRebuildInputs inputs;
  std::error_code ec;
  if (!std::filesystem::exists(index_path, ec) || ec) {
    // Nothing in service yet: a clean rebuild publishes the first database.
    inputs.schema_version = kSchemaVersion;
    return inputs;
  }
  // The digest is taken BEFORE the read-only handle is opened and re-checked
  // immediately before publication, so any concurrent write is detected.
  inputs.source_digest = sha256_of(index_path).value_or("");
  if (inputs.source_digest.empty()) {
    throw CidxError("cannot read index database " + index_path);
  }

  // The version actually on disk, not the one this binary targets. Reporting
  // kSchemaVersion here unconditionally would make an out-of-date serving
  // database indistinguishable from a current one.
  inputs.schema_version =
      storage::inspect_database_integrity(index_path).schema_version;

  // Migration compatibility. An older database is brought forward on a PRIVATE
  // COPY: the file in service is copied, the copy is migrated by an ordinary
  // read-write open, and the inputs are read from the copy. The file in service
  // is never opened for writing, so the byte-for-byte guarantee is untouched
  // and the rebuild still publishes a database at the current schema.
  //
  // Declared before `serving` so the read-only handle is closed before the copy
  // is removed.
  std::optional<MigrationCopy> migration;
  std::string read_path = index_path;
  if (inputs.schema_version > 0 && inputs.schema_version < kSchemaVersion) {
    const std::string copy_path = clean_rebuild_migration_copy_path(index_path);
    std::filesystem::remove(copy_path, ec);
    std::filesystem::copy_file(
        index_path, copy_path,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      throw CidxError("cannot copy " + index_path +
                      " for migration: " + ec.message());
    }
    migration.emplace(copy_path);
    {
      // An ordinary read-write open migrates. Scoped so the writer is closed
      // before anything reads the result.
      Storage upgrading(copy_path);
    }
    const int migrated =
        storage::inspect_database_integrity(copy_path).schema_version;
    if (migrated != kSchemaVersion) {
      throw CidxError("migrating a copy of " + index_path +
                      " reached schema v" + std::to_string(migrated) +
                      ", expected v" + std::to_string(kSchemaVersion));
    }
    inputs.migrated_for_capture = true;
    read_path = copy_path;
  }

  // A read-only Storage performs no mutation on connect: no directory
  // creation, no migrate(), no schema script, no backfill.
  Storage serving(read_path, Storage::OpenMode::read_only);
  // Read from the same database the inputs come from, so a migrated capture is
  // held to the catalog it actually captured.
  inputs.catalog = storage::read_database_catalog_identity(read_path);

  std::map<std::int64_t, std::string> universe_key;
  for (const SemanticUniverse &universe : serving.list_semantic_universes()) {
    universe_key.emplace(universe.id, universe.key);
    inputs.universes.push_back(universe);
  }

  std::map<std::int64_t, std::string> repository_name;
  for (const Repository &repository : serving.list_repositories()) {
    repository_name.emplace(repository.id, repository.name);
    std::optional<std::string> active_clone_path;
    if (repository.active_clone_id) {
      if (const std::optional<Clone> clone =
              serving.get_clone_by_id(*repository.active_clone_id)) {
        active_clone_path = clone->path;
      }
    }
    std::optional<std::string> universe;
    if (repository.semantic_universe_id) {
      if (const auto it = universe_key.find(*repository.semantic_universe_id);
          it != universe_key.end()) {
        universe = it->second;
      }
    }
    inputs.repositories.push_back(
        CleanRebuildRepositoryInput{.name = repository.name,
                                    .kind = repository.kind,
                                    .remote_url = repository.remote_url,
                                    .active_clone_path = active_clone_path,
                                    .semantic_universe_key = universe});
  }

  for (const Clone &clone : serving.list_clones()) {
    const auto it = repository_name.find(clone.repository_id);
    if (it == repository_name.end()) {
      continue;
    }
    inputs.clones.push_back(
        CleanRebuildCloneInput{.repository_name = it->second,
                               .path = clone.path,
                               .label = clone.label});
  }

  for (const Component &component : serving.list_components()) {
    std::optional<std::string> repository;
    if (component.repository_id) {
      if (const auto it = repository_name.find(*component.repository_id);
          it != repository_name.end()) {
        repository = it->second;
      }
    }
    std::optional<std::string> universe;
    if (component.semantic_universe_id) {
      if (const auto it = universe_key.find(*component.semantic_universe_id);
          it != universe_key.end()) {
        universe = it->second;
      }
    }
    const bool portable =
        component.path.contains('$') || component.path.contains('<');
    // component_abs_base resolves the EFFECTIVE root (base + version); the
    // replay needs the absolute BASE, because add_component stores the version
    // in its own column. Resolving a version-less copy yields exactly that.
    Component base_only = component;
    base_only.version = std::nullopt;
    inputs.components.push_back(CleanRebuildComponentInput{
        .name = component.name,
        .path = component.path,
        .absolute_base =
            portable ? component.path : serving.component_abs_base(base_only),
        .portable_path = portable,
        .kind = component.kind,
        .version = component.version,
        .repository_name = repository,
        .semantic_universe_key = universe});
  }

  for (const auto &[file, path] : serving.list_files()) {
    inputs.files.push_back(
        CleanRebuildFileInput{.path = path,
                              .mtime = file.mtime,
                              .md5 = file.md5,
                              .compile_options = file.compile_options,
                              .driver = file.driver,
                              .args_overridden = file.args_overridden});
  }

  for (const auto &[name, path] : serving.list_labels()) {
    inputs.labels.emplace_back(name, path);
  }
  return inputs;
}

void replay_clean_rebuild_inputs(Storage &candidate,
                                 const CleanRebuildInputs &inputs) {
  std::map<std::string, std::int64_t> universe_id;
  for (const SemanticUniverse &universe : inputs.universes) {
    universe_id.emplace(universe.key,
                        candidate.add_semantic_universe(
                            universe.key, universe.name, universe.policy));
  }
  const auto universe_for =
      [&universe_id](const std::optional<std::string> &key)
      -> std::optional<std::int64_t> {
    if (!key) {
      return std::nullopt;
    }
    const auto it = universe_id.find(*key);
    return it == universe_id.end() ? std::nullopt
                                   : std::optional<std::int64_t>(it->second);
  };

  std::map<std::string, std::int64_t> repository_id;
  for (const CleanRebuildRepositoryInput &repository : inputs.repositories) {
    repository_id.emplace(repository.name,
                          candidate.add_repository(
                              repository.name, repository.kind,
                              repository.remote_url,
                              universe_for(repository.semantic_universe_key)));
  }

  std::map<std::string, std::int64_t> clone_id;
  for (const CleanRebuildCloneInput &clone : inputs.clones) {
    const auto it = repository_id.find(clone.repository_name);
    if (it == repository_id.end()) {
      continue;
    }
    clone_id.emplace(clone.path,
                     candidate.add_clone(it->second, clone.path, clone.label));
  }
  // The active clone is restored after every clone exists so a repository can
  // point at any of them, not only the first one replayed.
  for (const CleanRebuildRepositoryInput &repository : inputs.repositories) {
    if (!repository.active_clone_path) {
      continue;
    }
    const auto repo = repository_id.find(repository.name);
    const auto clone = clone_id.find(*repository.active_clone_path);
    if (repo != repository_id.end() && clone != clone_id.end()) {
      candidate.set_active_clone(repo->second, clone->second);
    }
  }

  // Group, then relativize — the same order import uses, so a grouped
  // component ends up with the identical stored (clone-relative) path.
  for (const CleanRebuildComponentInput &component : inputs.components) {
    const std::int64_t id =
        candidate.add_component(component.name, component.absolute_base,
                                component.kind, component.version);
    if (component.repository_name) {
      const std::string &owner = *component.repository_name;
      const auto it = repository_id.find(owner);
      if (it != repository_id.end()) {
        candidate.set_component_repository(id, it->second);
        if (!component.portable_path) {
          const auto repository = std::ranges::find_if(
              inputs.repositories,
              [&owner](const CleanRebuildRepositoryInput &candidate_repo) {
                return candidate_repo.name == owner;
              });
          if (repository != inputs.repositories.end()) {
            if (const std::optional<std::string> &clone_root =
                    repository->active_clone_path) {
              candidate.relativize_component(id, *clone_root);
            }
          }
        }
      }
    }
    const std::optional<std::int64_t> universe =
        universe_for(component.semantic_universe_key);
    if (universe) {
      candidate.set_component_semantic_universe(id, universe);
    }
  }

  for (const auto &[name, path] : inputs.labels) {
    candidate.add_label(name, path);
  }

  for (const CleanRebuildFileInput &file : inputs.files) {
    // mtime/md5 are replayed so staleness detection compares against the same
    // captured source state; the file itself stays pending until it is indexed.
    const std::int64_t id = candidate.add_file_path(
        file.path, file.mtime, file.md5, file.compile_options, file.driver);
    if (file.args_overridden && file.compile_options) {
      candidate.set_file_compile_options(id, *file.compile_options, file.driver,
                                         file.driver.has_value());
    }
  }
}

auto verify_clean_rebuild_candidate(
    const std::string &candidate_path,
    const storage::DatabaseCatalogIdentity &expected, bool require_complete)
    -> CleanRebuildVerification {
  CleanRebuildVerification verification;
  const storage::DatabaseIntegrityReport report =
      storage::inspect_database_integrity(candidate_path);
  verification.opened = report.opened;
  verification.integrity_detail = report.detail;
  if (!report.opened) {
    return verification;
  }
  verification.integrity_ok = report.integrity_ok;
  verification.foreign_keys_ok = report.foreign_keys_ok;
  verification.foreign_key_violations = report.foreign_key_violations;
  verification.schema_version = report.schema_version;
  verification.schema_ok = report.schema_version == kSchemaVersion;
  if (!verification.integrity_ok || !verification.foreign_keys_ok ||
      !verification.schema_ok) {
    // A structurally broken database must not be queried further; the reason
    // is already decisive.
    return verification;
  }
  try {
    verification.catalog =
        storage::read_database_catalog_identity(candidate_path);
    verification.catalog_ok = verification.catalog == expected;
    verification.files_pending =
        storage::read_database_pending_file_count(candidate_path);
    verification.complete =
        !require_complete || verification.files_pending == 0;
    verification.semantic_digest =
        storage::read_database_semantic_digest(candidate_path);
  } catch (const std::exception &error) {
    verification.opened = false;
    verification.integrity_detail = error.what();
  }
  return verification;
}

auto publish_clean_rebuild_candidate(const std::string &candidate_path,
                                     const std::string &index_path,
                                     const std::string &expected_source_digest,
                                     CleanRebuildFailurePoint failure)
    -> std::string {
  std::error_code ec;
  if (!std::filesystem::exists(candidate_path, ec) || ec) {
    return "verified candidate disappeared before publication";
  }

  // Advisory gate: prove the serving database is still exactly the file the
  // rebuild was derived from BEFORE anything replaces it.
  const std::optional<std::string> current = sha256_of(index_path);
  if (expected_source_digest.empty()) {
    if (current) {
      return "an index database appeared at " + index_path +
             " while the clean rebuild was running";
    }
  } else if (!current) {
    return "the index database at " + index_path +
           " disappeared while the clean rebuild was running";
  } else if (*current != expected_source_digest) {
    return "the index database at " + index_path +
           " changed while the clean rebuild was running";
  }

  // Both files must be wholly contained in themselves at the instant of the
  // rename. A sidecar beside either one means the rename would publish a main
  // file while leaving state that belongs to a different database behind.
  for (const std::string &path : {candidate_path, index_path}) {
    for (const std::string &sidecar : storage::database_sidecar_paths(path)) {
      if (std::filesystem::exists(sidecar, ec) && !ec) {
        return "refusing to publish: " +
               std::filesystem::path(sidecar).filename().string() +
               " exists beside " + path +
               ", so replacing the main file alone would not replace the "
               "database";
      }
    }
  }

  if (failure == CleanRebuildFailurePoint::before_publication) {
    return "injected failure at " +
           std::string(clean_rebuild_failure_point_name(failure));
  }

  // fsync the candidate, fsync the directory, then rename. rename(2) is atomic:
  // a reader either sees the whole previous database or the whole published
  // one, never a partial file.
  //
  // The two hooks are the qualification entry into that window. They are the
  // only way to be interrupted at a chosen instant rather than at whatever
  // instant a timer happens to land on, and neither is reachable without the
  // CIDX_CLEAN_REBUILD_FAIL_AT test hook.
  util::PublishHooks hooks;
  if (failure == CleanRebuildFailurePoint::during_publication) {
    hooks.before_rename = [] { util::terminate_by_interrupt(); };
  } else if (failure == CleanRebuildFailurePoint::after_rename) {
    hooks.after_rename = [] { util::terminate_by_interrupt(); };
  }
  return util::publish_file_atomically(candidate_path, index_path, hooks);
}

} // namespace cidx::application
