// Opt-in clean rebuild with verified atomic publication (S-101).
//
// A clean rebuild never edits the database that is currently in service. It
//   1. captures the rebuild inputs from the serving database under a READ-ONLY
//      handle (no directory creation, no migration, no schema script, no
//      backfill — the file on disk is not written at all);
//   2. builds a private candidate database beside the target so that the final
//      publication is a same-filesystem rename;
//   3. replays the captured inputs into the candidate and indexes it through
//      the SAME lifecycle the in-place index path runs (graph / deferred
//      transforms / no-graph are not forked here — see
//      StorageApplicationOperations::execute(IndexRequest));
//   4. verifies the candidate in full (integrity, foreign keys, schema and
//      catalog identity, completeness, canonical semantic digest);
//   5. re-checks that the serving database is still byte-for-byte the file that
//      was captured, and only then publishes it with one atomic rename.
//
// Any failure, cancellation, interruption, or verification refusal before the
// rename leaves the serving database untouched and removes the candidate. The
// rename itself is atomic in the kernel, so there is no observable torn state.
//
// Per-translation-unit writer rollback is NOT re-implemented here; it stays
// owned by the controlled set-based writer. This module owns only the
// temporary database, its verification, and the atomic file replacement.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "storage/database_verification.hpp"
#include "storage/records.hpp"

namespace cidx {
class Storage;
} // namespace cidx

namespace cidx::application {

// Fault points of the clean-rebuild sequence, in execution order. A test
// injects a failure at one of them to prove the serving database survives a
// failure at every step. The ordering is the contract; the values are internal.
//
// Two kinds appear here and they are NOT interchangeable:
//   * boundary points (`after_*`, `before_publication`) abort BETWEEN phases;
//   * in-phase points (`during_*`) abort INSIDE a phase, mid-flight, which is
//     the only way to exercise a partially applied translation unit, a
//     half-written unit of work, or an interrupted rename.
enum class CleanRebuildFailurePoint : std::uint8_t {
  none,
  after_inputs_captured,
  after_candidate_created,
  after_inputs_replayed,
  // In-phase: throws inside the translation-unit pipeline while facts are being
  // handed to the storage adapter, i.e. with the extraction of that unit
  // already under way. Routed to ast::IndexFailurePoint::adapter.
  during_extraction,
  // In-phase: throws inside the controlled writer as the unit of work commits,
  // after its facts were staged. Routed to ast::IndexFailurePoint::commit.
  during_writer_commit,
  after_candidate_indexed,
  after_verification,
  before_publication,
  // In-phase: delivers SIGINT with the default disposition INSIDE the
  // publication window — after the candidate and the directory are on stable
  // storage, before rename(2). The process dies with the serving database still
  // in place.
  during_publication,
  // In-phase: delivers the same SIGINT immediately after rename(2) returns. The
  // serving path now holds the candidate; the assertion is that what a reader
  // finds there is a whole, healthy database rather than a torn file.
  after_rename,
};

// Stable spelling used by diagnostics and by the CIDX_CLEAN_REBUILD_FAIL_AT
// test hook. Unknown values map to "none"/nullopt.
[[nodiscard]] auto
clean_rebuild_failure_point_name(CleanRebuildFailurePoint point)
    -> std::string_view;
[[nodiscard]] auto clean_rebuild_failure_point_from_name(std::string_view name)
    -> std::optional<CleanRebuildFailurePoint>;
// Reads CIDX_CLEAN_REBUILD_FAIL_AT. Unset or unrecognised spellings yield
// `none`, so a stray environment value can never silently skip publication.
[[nodiscard]] auto clean_rebuild_failure_point_from_environment()
    -> CleanRebuildFailurePoint;

// One registered translation-unit / header input, addressed by absolute path
// rather than by a database-local row id so it can be replayed into a database
// that mints its own ids.
struct CleanRebuildFileInput {
  std::string path;
  std::optional<double> mtime;
  std::optional<std::string> md5;
  std::optional<std::vector<std::string>> compile_options;
  std::optional<std::string> driver;
  bool args_overridden = false;
};

struct CleanRebuildComponentInput {
  std::string name;
  // The stored path is clone-relative for a grouped component, so replay uses
  // `absolute_base` (add_component absolutizes a plain path) and then
  // re-relativizes against the same clone root, exactly as import does. A
  // portable `$VAR` / `<label>` path is stored verbatim and replayed verbatim.
  std::string path;
  std::string absolute_base;
  bool portable_path = false;
  std::string kind;
  std::optional<std::string> version;
  std::optional<std::string> repository_name;
  std::optional<std::string> semantic_universe_key;
};

struct CleanRebuildRepositoryInput {
  std::string name;
  std::string kind;
  std::optional<std::string> remote_url;
  std::optional<std::string> active_clone_path;
  std::optional<std::string> semantic_universe_key;
};

struct CleanRebuildCloneInput {
  std::string repository_name;
  std::string path;
  std::optional<std::string> label;
};

// Everything a clean rebuild must reproduce, free of database-local ids.
struct CleanRebuildInputs {
  std::vector<SemanticUniverse> universes;
  std::vector<CleanRebuildRepositoryInput> repositories;
  std::vector<CleanRebuildCloneInput> clones;
  std::vector<CleanRebuildComponentInput> components;
  std::vector<CleanRebuildFileInput> files;
  std::vector<std::pair<std::string, std::string>> labels;
  int schema_version = 0;
  // sha256 of the serving database file at capture time; empty when no
  // database was in service yet.
  std::string source_digest;
};

struct CleanRebuildVerification {
  bool opened = false;
  bool integrity_ok = false;
  bool foreign_keys_ok = false;
  bool schema_ok = false;
  bool catalog_ok = false;
  bool complete = false;
  int schema_version = 0;
  std::int64_t foreign_key_violations = 0;
  std::int64_t files_pending = 0;
  std::string integrity_detail;
  storage::DatabaseCatalogIdentity catalog;
  // Canonical semantic digest over the portable projection of the published
  // fact families (files, symbols, edges, definitions, diagnostics, includes).
  std::string semantic_digest;

  [[nodiscard]] auto passed() const -> bool {
    return opened && integrity_ok && foreign_keys_ok && schema_ok &&
           catalog_ok && complete;
  }
  // Human-readable reason for the first failed check, or "" when passed().
  [[nodiscard]] auto failure_reason() const -> std::string;
};

// Verify a candidate database that is already closed for writing.
// `expected` is the catalog captured from the serving database.
// `require_complete` refuses a candidate that still has pending files, which is
// what makes an interrupted or partially indexed rebuild unpublishable.
[[nodiscard]] auto
verify_clean_rebuild_candidate(const std::string &candidate_path,
                               const storage::DatabaseCatalogIdentity &expected,
                               bool require_complete)
    -> CleanRebuildVerification;

// Whether a serving database may be the source of a clean rebuild at all.
// Returns an empty string when it may, or the refusal reason.
//
// Two properties are checked, both read-only, both BEFORE anything is built:
//
//   * schema identity — capture reads the serving database through the ordinary
//     read-only handle, which by design does not migrate. Reading an older (or
//     newer) schema through today's queries would silently mis-capture the
//     catalog, so an out-of-date database is refused with an actionable message
//     rather than rebuilt from a misread input set;
//   * whole-file containment — publication replaces the main database file with
//     one rename. That is only equivalent to replacing the database when the
//     database IS the main file. A WAL-mode database keeps committed content in
//     its `-wal` sidecar, and a leftover sidecar of any kind would survive the
//     rename and be interpreted against the newly published file. Refused.
//
// A path with no database in service is accepted: there is nothing to misread
// and nothing to strand.
[[nodiscard]] auto clean_rebuild_source_refusal(const std::string &index_path)
    -> std::string;

// Capture the rebuild inputs from a serving database WITHOUT writing to it.
// Throws CidxError when the database cannot be opened read-only.
//
// `schema_version` reports the version the SERVING database actually carries,
// not the version this binary was built for; the two are compared by
// clean_rebuild_source_refusal.
[[nodiscard]] auto capture_clean_rebuild_inputs(const std::string &index_path)
    -> CleanRebuildInputs;

// Replay captured inputs into an open, empty candidate database.
void replay_clean_rebuild_inputs(Storage &candidate,
                                 const CleanRebuildInputs &inputs);

// The candidate path used for `index_path`. Deterministic given the pid and
// the sequence number, and always a sibling of the target so that publication
// is a same-filesystem rename.
[[nodiscard]] auto clean_rebuild_candidate_path(const std::string &index_path)
    -> std::string;

// Publish a verified candidate over `index_path`.
//
// `expected_source_digest` is the sha256 captured before the rebuild started;
// publication is refused when the serving database no longer matches it, so a
// concurrent writer can never be silently overwritten. An empty expected digest
// means "there was no database in service" and requires the target to still be
// absent.
//
// Returns an empty string on success, or the refusal reason. On refusal the
// candidate is left on disk for the caller to remove and the target is
// untouched.
[[nodiscard]] auto publish_clean_rebuild_candidate(
    const std::string &candidate_path, const std::string &index_path,
    const std::string &expected_source_digest, CleanRebuildFailurePoint failure)
    -> std::string;

// sha256 of a file, or nullopt when it does not exist / cannot be read.
[[nodiscard]] auto clean_rebuild_file_digest(const std::string &path)
    -> std::optional<std::string>;

} // namespace cidx::application
