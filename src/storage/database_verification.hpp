// Read-only qualification of an index database file (S-101).
//
// Every check here opens its own READ-ONLY connection to a database file and
// writes nothing: no directory creation, no migration, no schema script, no
// backfill. That is what makes it safe to run against a candidate database
// before publication and against the database currently in service.
//
// Raw SQL belongs to the persistence module, so the clean-rebuild orchestration
// consumes these results rather than reaching for a connection of its own.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cidx::storage {

// `PRAGMA integrity_check` / `PRAGMA foreign_key_check` / schema_version for
// one database file.
struct DatabaseIntegrityReport {
  bool opened = false;
  bool integrity_ok = false;
  bool foreign_keys_ok = false;
  int schema_version = 0;
  std::int64_t foreign_key_violations = 0;
  // First integrity_check row when it is not "ok", or the open error.
  std::string detail;
};

// Identity of the input catalog: the rows a rebuild must reproduce exactly.
// Counts are cheap assertions; the digest is the authoritative comparison and
// is computed over an ordered projection that excludes database-local ids.
struct DatabaseCatalogIdentity {
  std::int64_t universes = 0;
  std::int64_t repositories = 0;
  std::int64_t clones = 0;
  std::int64_t components = 0;
  std::int64_t files = 0;
  std::int64_t labels = 0;
  std::string digest;

  [[nodiscard]] auto operator==(const DatabaseCatalogIdentity &) const
      -> bool = default;
};

// Opens `path` read-only and runs the integrity pragmas. A database that
// cannot be opened reports `opened == false` with the reason in `detail`.
[[nodiscard]] auto inspect_database_integrity(const std::string &path)
    -> DatabaseIntegrityReport;

// Catalog identity of `path`, read-only.
[[nodiscard]] auto read_database_catalog_identity(const std::string &path)
    -> DatabaseCatalogIdentity;

// Canonical semantic digest of `path`, read-only: a sha256 over the ordered,
// id-free projection of the published fact families (files, symbols, edges,
// definitions, diagnostics, include edges). Two independently built databases
// holding the same semantic content produce the same digest.
[[nodiscard]] auto read_database_semantic_digest(const std::string &path)
    -> std::string;

// Number of registered files still pending (never indexed) in `path`.
[[nodiscard]] auto read_database_pending_file_count(const std::string &path)
    -> std::int64_t;

// The fact families covered by read_database_semantic_digest, in digest order.
// Exposed so the qualification suites can assert the coverage list rather than
// re-deriving it.
[[nodiscard]] auto semantic_digest_fact_families()
    -> std::vector<std::string_view>;

} // namespace cidx::storage
