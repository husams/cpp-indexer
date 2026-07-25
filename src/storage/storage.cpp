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

#include "storage/sqlite_adapters.hpp"
#include "storage/storage_detail.hpp"
#include "storage/storage_schema.hpp"

namespace cidx {

using namespace detail;

constexpr std::string_view kPreviousCatalogHash =
    "eed1f38ccdc779776c637d8e8ffbc015c7616a94fecabd5e4302f0587c1bab93";
constexpr int kPreviousSchemaVersion = kSchemaVersion - 1;

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

Transaction::Transaction(SqliteStorageService &db)
    : db_(db), uncaught_on_entry_(std::uncaught_exceptions()) {
  if (db_.in_txn_) {
    throw StorageError(
        "nested SqliteStorageService::transaction() is not supported");
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

SqliteStorageService::SqliteStorageService(const std::string &path,
                                           OpenMode mode)
    : db_(mode == OpenMode::read_only ? path : prepare_db_path(path),
          mode == OpenMode::read_only,
          mode == OpenMode::read_only ? SqliteProfile::read_only_replay
                                      : SqliteProfile::indexing),
      ports_(std::make_unique<storage::SqliteStoragePorts>(*this)) {
  if (mode == OpenMode::read_only) {
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
    return;
  }
  // Reject an incompatible existing catalog before migrations or schema
  // seeding can mutate the database. Fresh and legacy databases without a
  // catalog hash are allowed to receive the current seed below.
  std::string existing_schema_version;
  try {
    auto schema_stmt =
        db_.prepare("SELECT value FROM meta WHERE key = 'schema_version'");
    if (schema_stmt.step()) {
      existing_schema_version = schema_stmt.col_text(0);
    }
  } catch (const StorageError &e) {
    if (!std::string(e.what()).contains("no such table")) {
      throw;
    }
  }
  std::string existing_catalog_hash;
  try {
    auto catalog_stmt =
        db_.prepare("SELECT value FROM meta WHERE key = 'catalog_hash'");
    if (catalog_stmt.step()) {
      existing_catalog_hash = catalog_stmt.col_text(0);
    }
  } catch (const StorageError &e) {
    if (!std::string(e.what()).contains("no such table")) {
      throw;
    }
  }
  const bool predecessor_catalog =
      existing_catalog_hash == kPreviousCatalogHash &&
      existing_schema_version == std::to_string(kPreviousSchemaVersion);
  if (existing_catalog_hash == kPreviousCatalogHash && !predecessor_catalog) {
    throw CidxError("predecessor catalog hash requires schema_version " +
                    std::to_string(kPreviousSchemaVersion) + " -> " +
                    std::to_string(kSchemaVersion) + ", found " +
                    (existing_schema_version.empty()
                         ? std::string("missing")
                         : existing_schema_version));
  }
  if (!existing_catalog_hash.empty() &&
      existing_catalog_hash != catalog::kCatalogHash && !predecessor_catalog) {
    throw CidxError(
        "catalog_hash " + existing_catalog_hash +
        " does not match the required " + std::string(catalog::kCatalogHash) +
        " (regenerate the database with the matching semantic catalogs)");
  }
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
      stored_catalog_hash != catalog::kCatalogHash && !predecessor_catalog) {
    throw CidxError(
        "catalog_hash " + stored_catalog_hash +
        " does not match the required " + std::string(catalog::kCatalogHash) +
        " (regenerate the database with the matching semantic catalogs)");
  }
  if (predecessor_catalog) {
    db_.exec("UPDATE meta SET value = '" + std::string(catalog::kCatalogHash) +
             "' WHERE key = 'catalog_hash'");
  }
  db_.exec(catalog::kSeedSql);
  // v34 -> v35: preserve every legacy include configuration as one canonical
  // descriptor and attach its compatibility row to that descriptor.
  {
    std::vector<int64_t> legacy_ids;
    auto pending = db_.prepare("SELECT id FROM include_config WHERE "
                               "translation_unit_config_id IS NULL "
                               "ORDER BY id");
    while (pending.step()) {
      legacy_ids.push_back(pending.col_int64(0));
    }
    for (const int64_t id : legacy_ids) {
      if (const auto config = include_config_by_id(id)) {
        add_include_config(*config);
      }
    }
  }
  // v21 -> v22 one-time backfill: entity_node is a pure-DB classification of
  // existing symbols (no re-parse), so an upgraded index gets its design types
  // filled in immediately on open -- no re-index/resolve. (entity_node did not
  // exist during migrate(); it does now, after kSchema.) Mirrors storage.py.
  if (needs_entity_node_backfill_) {
    auto txn = transaction();
    cpp_materialise_entity_nodes(db_);
    txn.commit();
  }
  reconcile_external_identities();
}

SqliteStorageService::~SqliteStorageService() = default;

storage::WorkspaceCatalogReadPort &
SqliteStorageService::workspace_catalog_read() {
  return ports_->workspace_catalog_read();
}

storage::WorkspaceCatalogWritePort &
SqliteStorageService::workspace_catalog_write() {
  return ports_->workspace_catalog_write();
}

storage::SourceStoreReadPort &SqliteStorageService::source_read() {
  return ports_->source_read();
}

storage::SourceStoreWritePort &SqliteStorageService::source_write() {
  return ports_->source_write();
}

storage::SymbolReadPort &SqliteStorageService::symbol_read() {
  return ports_->symbol_read();
}

storage::SymbolWritePort &SqliteStorageService::symbol_write() {
  return ports_->symbol_write();
}

storage::TypeReadPort &SqliteStorageService::type_read() {
  return ports_->type_read();
}

storage::TypeWritePort &SqliteStorageService::type_write() {
  return ports_->type_write();
}

storage::FactReadPort &SqliteStorageService::fact_read() {
  return ports_->fact_read();
}

storage::FactWritePort &SqliteStorageService::fact_write() {
  return ports_->fact_write();
}

storage::DefinitionReadPort &SqliteStorageService::definition_read() {
  return ports_->definition_read();
}

storage::DefinitionWritePort &SqliteStorageService::definition_write() {
  return ports_->definition_write();
}

storage::IncludeReadPort &SqliteStorageService::include_read() {
  return ports_->include_read();
}

storage::IncludeWritePort &SqliteStorageService::include_write() {
  return ports_->include_write();
}

storage::SchemaCatalogReadPort &SqliteStorageService::schema_read() {
  return ports_->schema_read();
}

storage::UnitOfWorkFactory &SqliteStorageService::unit_of_work() {
  return ports_->unit_of_work();
}

void SqliteStorageService::reconcile_external_identities() {
  for (const auto &entry : catalog::kSourceKinds) {
    db_.exec(
        "UPDATE edge_site SET recv_src_kind_id = " + std::to_string(entry.id) +
        " WHERE recv_src_kind = '" + std::string(entry.name) + "'");
    db_.exec("UPDATE call_arg SET src_kind_id = " + std::to_string(entry.id) +
             " WHERE src_kind = '" + std::string(entry.name) + "'");
  }
  auto unknown_edge = db_.prepare(
      "SELECT recv_src_kind FROM edge_site WHERE recv_src_kind IS NOT NULL "
      "AND recv_src_kind_id IS NULL LIMIT 1");
  if (unknown_edge.step()) {
    throw StorageError("unknown source kind '" + unknown_edge.col_text(0) +
                       "'");
  }
  auto unknown_arg =
      db_.prepare("SELECT src_kind FROM call_arg WHERE src_kind IS NOT NULL "
                  "AND src_kind_id IS NULL LIMIT 1");
  if (unknown_arg.step()) {
    throw StorageError("unknown source kind '" + unknown_arg.col_text(0) + "'");
  }
  db_.exec("UPDATE edge_site SET recv_src_kind = NULL WHERE recv_src_kind_id "
           "IS NOT NULL");
  db_.exec("UPDATE call_arg SET src_kind = NULL WHERE src_kind_id IS NOT NULL");
  db_.exec(
      "UPDATE external_identity SET symbol_id = CASE WHEN identity_kind = 2 "
      "THEN (SELECT id FROM symbol WHERE usr = identity_text LIMIT 1) ELSE "
      "NULL END, "
      "type_id = CASE WHEN identity_kind = 1 THEN (SELECT id FROM type_node "
      "WHERE decl_usr = identity_text ORDER BY id LIMIT 1) ELSE NULL END, "
      "resolution_status = CASE WHEN (identity_kind = 2 AND EXISTS "
      "(SELECT 1 FROM symbol WHERE usr = identity_text)) OR (identity_kind = 1 "
      "AND EXISTS (SELECT 1 FROM type_node WHERE decl_usr = identity_text)) "
      "THEN 1 ELSE 0 END");
  db_.exec("UPDATE type_node SET decl_id = (SELECT id FROM symbol s WHERE "
           "s.usr = type_node.decl_usr LIMIT 1) WHERE decl_usr IS NOT NULL");
  db_.exec("UPDATE symbol SET parent_id = (SELECT id FROM symbol p WHERE "
           "p.usr = symbol.parent_usr) WHERE parent_usr IS NOT NULL");
  db_.exec(
      "UPDATE edge_site SET recv_decl_id = COALESCE(recv_decl_id, (SELECT id "
      "FROM symbol s "
      "WHERE s.usr = edge_site.recv_decl_usr LIMIT 1), (SELECT symbol_id FROM "
      "external_identity i WHERE i.id = edge_site.recv_decl_identity_id)), "
      "recv_type_id = COALESCE(recv_type_id, (SELECT id FROM type_node t WHERE "
      "t.decl_usr = "
      "edge_site.recv_type_usr ORDER BY id LIMIT 1), (SELECT type_id FROM "
      "external_identity i WHERE i.id = edge_site.recv_type_identity_id))");
  db_.exec("UPDATE edge_site SET recv_decl_identity_id = NULL WHERE "
           "recv_decl_id IS NOT NULL");
  db_.exec("UPDATE edge_site SET recv_type_identity_id = NULL WHERE "
           "recv_type_id IS NOT NULL");
  db_.exec("UPDATE call_arg SET decl_id = COALESCE(decl_id, (SELECT id FROM "
           "symbol s WHERE "
           "s.usr = call_arg.decl_usr LIMIT 1), (SELECT symbol_id FROM "
           "external_identity i "
           "WHERE i.id = call_arg.decl_identity_id)), "
           "callee_id = COALESCE(callee_id, (SELECT id FROM symbol s WHERE "
           "s.usr = call_arg.callee_usr LIMIT 1), "
           "(SELECT symbol_id FROM external_identity i WHERE i.id = "
           "call_arg.callee_identity_id)), "
           "type_id = COALESCE(type_id, (SELECT id FROM type_node t WHERE "
           "t.decl_usr = call_arg.type_usr "
           "ORDER BY id LIMIT 1), (SELECT type_id FROM external_identity i "
           "WHERE i.id = call_arg.type_identity_id))");
  db_.exec(
      "UPDATE call_arg SET decl_identity_id = NULL WHERE decl_id IS NOT NULL");
  db_.exec("UPDATE call_arg SET callee_identity_id = NULL WHERE callee_id IS "
           "NOT NULL");
  db_.exec(
      "UPDATE call_arg SET type_identity_id = NULL WHERE type_id IS NOT NULL");
}

void SqliteStorageService::reconcile_symbol_identity(int64_t symbol_id,
                                                     std::string_view usr) {
  auto identity = db_.prepare(
      "UPDATE external_identity SET symbol_id = ?, resolution_status = 1 "
      "WHERE identity_kind = 2 AND identity_text = ?");
  identity.bind(1, symbol_id);
  identity.bind(2, usr);
  identity.step_done();

  auto type_decl =
      db_.prepare("UPDATE type_node SET decl_id = ? WHERE decl_usr = ?");
  type_decl.bind(1, symbol_id);
  type_decl.bind(2, usr);
  type_decl.step_done();

  auto edge_decl = db_.prepare(
      "UPDATE edge_site SET recv_decl_id = ? WHERE recv_decl_id IS NULL "
      "AND recv_decl_identity_id IN (SELECT id FROM external_identity WHERE "
      "identity_kind = 2 AND identity_text = ?)");
  edge_decl.bind(1, symbol_id);
  edge_decl.bind(2, usr);
  edge_decl.step_done();

  auto call_decl =
      db_.prepare("UPDATE call_arg SET decl_id = ? WHERE decl_id IS NULL AND "
                  "decl_identity_id IN (SELECT id FROM external_identity WHERE "
                  "identity_kind = 2 AND identity_text = ?)");
  call_decl.bind(1, symbol_id);
  call_decl.bind(2, usr);
  call_decl.step_done();

  auto call_callee = db_.prepare(
      "UPDATE call_arg SET callee_id = ? WHERE callee_id IS NULL AND "
      "callee_identity_id IN (SELECT id FROM external_identity WHERE "
      "identity_kind = 2 AND identity_text = ?)");
  call_callee.bind(1, symbol_id);
  call_callee.bind(2, usr);
  call_callee.step_done();

  auto edge_clear = db_.prepare(
      "UPDATE edge_site SET recv_decl_identity_id = NULL WHERE "
      "recv_decl_id IS NOT NULL AND recv_decl_identity_id IN (SELECT id FROM "
      "external_identity WHERE identity_kind = 2 AND identity_text = ?)");
  edge_clear.bind(1, usr);
  edge_clear.step_done();
  auto call_decl_clear = db_.prepare(
      "UPDATE call_arg SET decl_identity_id = NULL WHERE decl_id IS NOT NULL "
      "AND decl_identity_id IN (SELECT id FROM external_identity WHERE "
      "identity_kind = 2 AND identity_text = ?)");
  call_decl_clear.bind(1, usr);
  call_decl_clear.step_done();
  auto call_callee_clear = db_.prepare(
      "UPDATE call_arg SET callee_identity_id = NULL WHERE callee_id IS NOT "
      "NULL "
      "AND callee_identity_id IN (SELECT id FROM external_identity WHERE "
      "identity_kind = 2 AND identity_text = ?)");
  call_callee_clear.bind(1, usr);
  call_callee_clear.step_done();
}

void SqliteStorageService::reconcile_type_identity(int64_t type_id,
                                                   std::string_view decl_usr) {
  auto identity = db_.prepare(
      "UPDATE external_identity SET type_id = ?, resolution_status = 1 "
      "WHERE identity_kind = 1 AND identity_text = ?");
  identity.bind(1, type_id);
  identity.bind(2, decl_usr);
  identity.step_done();

  auto type_decl = db_.prepare(
      "UPDATE type_node SET decl_id = (SELECT id FROM symbol WHERE usr = ?) "
      "WHERE decl_usr = ?");
  type_decl.bind(1, decl_usr);
  type_decl.bind(2, decl_usr);
  type_decl.step_done();

  auto edge_type = db_.prepare(
      "UPDATE edge_site SET recv_type_id = ? WHERE recv_type_id IS NULL AND "
      "recv_type_identity_id IN (SELECT id FROM external_identity WHERE "
      "identity_kind = 1 AND identity_text = ?)");
  edge_type.bind(1, type_id);
  edge_type.bind(2, decl_usr);
  edge_type.step_done();

  auto call_type =
      db_.prepare("UPDATE call_arg SET type_id = ? WHERE type_id IS NULL AND "
                  "type_identity_id IN (SELECT id FROM external_identity WHERE "
                  "identity_kind = 1 AND identity_text = ?)");
  call_type.bind(1, type_id);
  call_type.bind(2, decl_usr);
  call_type.step_done();

  auto edge_clear = db_.prepare(
      "UPDATE edge_site SET recv_type_identity_id = NULL WHERE "
      "recv_type_id IS NOT NULL AND recv_type_identity_id IN (SELECT id FROM "
      "external_identity WHERE identity_kind = 1 AND identity_text = ?)");
  edge_clear.bind(1, decl_usr);
  edge_clear.step_done();
  auto call_clear = db_.prepare(
      "UPDATE call_arg SET type_identity_id = NULL WHERE type_id IS NOT NULL "
      "AND type_identity_id IN (SELECT id FROM external_identity WHERE "
      "identity_kind = 1 AND identity_text = ?)");
  call_clear.bind(1, decl_usr);
  call_clear.step_done();
}

} // namespace cidx
