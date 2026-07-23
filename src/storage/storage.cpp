// Port of indexer/storage.py. The schema text and the three upsert statements
// are copied character-for-character (design §4/§4.2); read queries keep the
// same WHERE/ORDER BY text but select explicit column lists so that rows from
// MIGRATED databases (where ALTER TABLE appended columns at the end) decode
// correctly without name-based row factories.
#include "storage/storage.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "catalogs/generated_catalog.hpp"
#include "catalogs/generated_catalog_sql.hpp"
#include "compiledb/compiledb.hpp"
#include "util/errors.hpp"
#include "util/json_min.hpp"
#include "util/logger.hpp"
#include "util/pathutil.hpp"

#include "storage/storage_detail.hpp"
#include "storage/storage_schema.hpp"

namespace cidx {

using namespace detail;

bool is_symbol_kind(std::string_view kind) {
  return std::ranges::find(kSymbolKinds, kind) != kSymbolKinds.end();
}

int64_t symbol_kind_id(std::string_view name) {
  const auto &m = symbol_kind_ids_map();
  const auto it = m.find(name);
  return it != m.end() ? it->second
                       : -1; // unknown -> matches nothing (filters)
}

std::string symbol_kind_name(int64_t id) {
  const auto &m = symbol_kind_names_map();
  const auto it = m.find(id);
  return it != m.end() ? it->second : std::to_string(id);
}

// -- Transaction --------------------------------------------------------------

Transaction::Transaction(Storage &db)
    : db_(db), uncaught_on_entry_(std::uncaught_exceptions()) {
  if (db_.in_txn_) {
    throw StorageError("nested Storage::transaction() is not supported");
  }
  db_.db_.exec("BEGIN");
  db_.in_txn_ = true;
}

Transaction::~Transaction() {
  if (done_) {
    return;
  }
  // Destructor is ROLLBACK-only. Successful paths must call txn.commit()
  // explicitly so a failed COMMIT is not silently swallowed here (R2).
  // We only reach this branch during exception unwind (or forgotten commit).
  try {
    db_.db_.exec("ROLLBACK");
  } catch (...) {
    // Destructor must not throw; the connection rolls back on close anyway.
  }
  db_.in_txn_ = false;
}

void Transaction::commit() {
  if (done_) {
    return;
  }
  db_.db_.exec("COMMIT");
  db_.in_txn_ = false;
  done_ = true;
}

void Transaction::rollback() {
  if (done_) {
    return;
  }
  db_.db_.exec("ROLLBACK");
  db_.in_txn_ = false;
  done_ = true;
}

// -- Storage lifecycle
// ---------------------------------------------------------

// Defined below (materialise pass); forward-declared so the constructor can run
// the v21->v22 entity_node backfill right after the schema is created.

Storage::Storage(const std::string &path, OpenMode mode)
    : db_(mode == OpenMode::read_only ? path : prepare_db_path(path),
          mode == OpenMode::read_only) {
  if (mode == OpenMode::read_only) {
    // A concurrent writer must produce BUSY-with-retry, not an instant error.
    db_.exec("PRAGMA busy_timeout = 5000");
    // Version gate before anything else: a read-only connection cannot
    // migrate, so any other stored version is unusable.
    std::string stored;
    try {
      auto st =
          db_.prepare("SELECT value FROM meta WHERE key = 'schema_version'");
      if (st.step()) {
        stored = st.col_text(0);
      }
    } catch (const StorageError &e) {
      // Only a missing meta table means "not a cidx index"; any other
      // SQLite failure (not a database, BUSY, I/O) keeps its real message.
      if (!std::string(e.what()).contains("no such table")) {
        throw;
      }
    }
    if (stored != std::to_string(kSchemaVersion)) {
      throw CidxError("cannot open " + path + " read-only: schema_version " +
                      (stored.empty() ? std::string("missing") : stored) +
                      " does not match the required " +
                      std::to_string(kSchemaVersion) +
                      " (a read-only open cannot migrate)");
    }
    std::string catalog_hash;
    auto catalog_stmt =
        db_.prepare("SELECT value FROM meta WHERE key = 'catalog_hash'");
    if (catalog_stmt.step()) {
      catalog_hash = catalog_stmt.col_text(0);
    }
    if (catalog_hash != catalog::kCatalogHash) {
      throw CidxError(
          "cannot open " + path + " read-only: catalog_hash " +
          (catalog_hash.empty() ? std::string("missing") : catalog_hash) +
          " does not match the required " + std::string(catalog::kCatalogHash) +
          " (regenerate with the matching semantic catalogs)");
    }
    db_.exec("PRAGMA foreign_keys = ON");
    return;
  }
  db_.exec("PRAGMA foreign_keys = ON");
  migrate(); // BEFORE the schema script: its indexes need migrated columns
             // (G19)
  db_.exec(kSchema);
  std::string stored_catalog_hash;
  auto catalog_stmt =
      db_.prepare("SELECT value FROM meta WHERE key = 'catalog_hash'");
  if (catalog_stmt.step()) {
    stored_catalog_hash = catalog_stmt.col_text(0);
  }
  if (!stored_catalog_hash.empty() &&
      stored_catalog_hash != catalog::kCatalogHash) {
    throw CidxError(
        "catalog_hash " + stored_catalog_hash +
        " does not match the required " + std::string(catalog::kCatalogHash) +
        " (regenerate the database with the matching semantic catalogs)");
  }
  db_.exec(catalog::kSeedSql);
  // v21 -> v22 one-time backfill: entity_node is a pure-DB classification of
  // existing symbols (no re-parse), so an upgraded index gets its design types
  // filled in immediately on open -- no re-index/resolve. (entity_node did not
  // exist during migrate(); it does now, after kSchema.) Mirrors storage.py.
  if (needs_entity_node_backfill_) {
    auto txn = transaction();
    cpp_materialise_entity_nodes(db_);
    txn.commit();
  }
}

} // namespace cidx
