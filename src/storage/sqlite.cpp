#include "storage/sqlite.hpp"
#include "profile/index_profile.hpp"

#include <sqlite3.h>

#include <chrono>
#include <string>

#include "util/errors.hpp"

namespace cidx {

namespace {

[[noreturn]] void throw_db_error(sqlite3 *db, const std::string &context) {
  const char *msg = (db != nullptr) ? sqlite3_errmsg(db) : "out of memory";
  throw StorageError(context + ": " +
                     ((msg != nullptr) ? msg : "unknown SQLite error"));
}

} // namespace

auto sqlite_profile_settings(SqliteProfile profile) -> SqliteProfileSettings {
  switch (profile) {
  case SqliteProfile::indexing:
    return {.busy_timeout_ms = 5000,
            .foreign_keys = true,
            .query_only = false,
            .rollback_journal = true,
            .full_synchronous = true};
  case SqliteProfile::interactive_read:
  case SqliteProfile::read_only_replay:
    return {.busy_timeout_ms = 5000,
            .foreign_keys = true,
            .query_only = true,
            .rollback_journal = false,
            .full_synchronous = false};
  case SqliteProfile::migration:
  case SqliteProfile::maintenance:
    return {.busy_timeout_ms = 5000,
            .foreign_keys = true,
            .query_only = false,
            .rollback_journal = true,
            .full_synchronous = true};
  case SqliteProfile::artifact_staging:
    return {.busy_timeout_ms = 5000,
            .foreign_keys = true,
            .query_only = false,
            .rollback_journal = false,
            .full_synchronous = false};
  }
  throw StorageError("unknown SQLite profile");
}

auto sqlite_profile_name(SqliteProfile profile) -> std::string_view {
  switch (profile) {
  case SqliteProfile::indexing:
    return "indexing";
  case SqliteProfile::interactive_read:
    return "interactive_read";
  case SqliteProfile::read_only_replay:
    return "read_only_replay";
  case SqliteProfile::migration:
    return "migration";
  case SqliteProfile::maintenance:
    return "maintenance";
  case SqliteProfile::artifact_staging:
    return "artifact_staging";
  }
  throw StorageError("unknown SQLite profile");
}

// -- SqliteStmt --------------------------------------------------------------

SqliteStmt::SqliteStmt(sqlite3 *db, std::string_view sql) : db_(db) {
  const bool profiling = profile::active();
  const auto started = profiling ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
  const int rc = sqlite3_prepare_v2(
      db_, sql.data(), static_cast<int>(sql.size()), &stmt_, nullptr);
  if (profiling) {
    profile::note_sqlite_prepare(std::chrono::duration<double>(
                                     std::chrono::steady_clock::now() - started)
                                     .count(),
                                 sql.size());
  }
  if (rc != SQLITE_OK) {
    throw_db_error(db_, "prepare failed for \"" + std::string(sql) + "\"");
  }
}

SqliteStmt::SqliteStmt(SqliteDb &owner, std::string sql,
                       sqlite3_stmt *compiled) noexcept
    : db_(owner.db_), stmt_(compiled), owner_(&owner), sql_(std::move(sql)) {}

SqliteStmt::~SqliteStmt() {
  if (owner_ != nullptr && stmt_ != nullptr) {
    owner_->release_statement(sql_, stmt_);
    return;
  }
  sqlite3_finalize(stmt_);
}

SqliteStmt::SqliteStmt(SqliteStmt &&other) noexcept
    : db_(other.db_), stmt_(other.stmt_), owner_(other.owner_),
      sql_(std::move(other.sql_)) {
  other.db_ = nullptr;
  other.stmt_ = nullptr;
  other.owner_ = nullptr;
}

SqliteStmt &SqliteStmt::operator=(SqliteStmt &&other) noexcept {
  if (this != &other) {
    if (owner_ != nullptr && stmt_ != nullptr) {
      owner_->release_statement(sql_, stmt_);
    } else {
      sqlite3_finalize(stmt_);
    }
    db_ = other.db_;
    stmt_ = other.stmt_;
    owner_ = other.owner_;
    sql_ = std::move(other.sql_);
    other.db_ = nullptr;
    other.stmt_ = nullptr;
    other.owner_ = nullptr;
  }
  return *this;
}

void SqliteStmt::bind_null(int idx) {
  if (sqlite3_bind_null(stmt_, idx) != SQLITE_OK) {
    throw_db_error(db_, "bind_null");
  }
}

void SqliteStmt::bind(int idx, int64_t value) {
  if (sqlite3_bind_int64(stmt_, idx, value) != SQLITE_OK) {
    throw_db_error(db_, "bind int64");
  }
}

void SqliteStmt::bind(int idx, double value) {
  if (sqlite3_bind_double(stmt_, idx, value) != SQLITE_OK) {
    throw_db_error(db_, "bind double");
  }
}

void SqliteStmt::bind(int idx, std::string_view value) {
  if (sqlite3_bind_text(stmt_, idx, value.data(),
                        static_cast<int>(value.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK) {
    throw_db_error(db_, "bind text");
  }
}

void SqliteStmt::bind(int idx, const SqlValue &value) {
  if (std::holds_alternative<std::nullptr_t>(value)) {
    bind_null(idx);
  } else if (const auto *i = std::get_if<int64_t>(&value)) {
    bind(idx, *i);
  } else if (const auto *d = std::get_if<double>(&value)) {
    bind(idx, *d);
  } else {
    bind(idx, std::string_view(std::get<std::string>(value)));
  }
}

bool SqliteStmt::step() {
  const bool profiling = profile::active();
  const auto started = profiling ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
  const int rc = sqlite3_step(stmt_);
  if (profiling) {
    profile::note_sqlite_step(std::chrono::duration<double>(
                                  std::chrono::steady_clock::now() - started)
                                  .count(),
                              static_cast<std::uint64_t>(sqlite3_stmt_status(
                                  stmt_, SQLITE_STMTSTATUS_VM_STEP, 1)),
                              static_cast<std::uint64_t>(sqlite3_stmt_status(
                                  stmt_, SQLITE_STMTSTATUS_FULLSCAN_STEP, 1)),
                              static_cast<std::uint64_t>(sqlite3_stmt_status(
                                  stmt_, SQLITE_STMTSTATUS_REPREPARE, 1)));
  }
  if (rc == SQLITE_ROW) {
    return true;
  }
  if (rc == SQLITE_DONE) {
    return false;
  }
  throw_db_error(db_, "step");
}

void SqliteStmt::step_done() {
  while (step()) {
  }
}

void SqliteStmt::reset() {
  if (sqlite3_reset(stmt_) != SQLITE_OK) {
    throw_db_error(db_, "reset");
  }
  sqlite3_clear_bindings(stmt_);
}

bool SqliteStmt::readonly() const { return sqlite3_stmt_readonly(stmt_) != 0; }

int SqliteStmt::column_count() const { return sqlite3_column_count(stmt_); }

bool SqliteStmt::col_is_null(int idx) const {
  return sqlite3_column_type(stmt_, idx) == SQLITE_NULL;
}

int64_t SqliteStmt::col_int64(int idx) const {
  return sqlite3_column_int64(stmt_, idx);
}

double SqliteStmt::col_double(int idx) const {
  return sqlite3_column_double(stmt_, idx);
}

std::string SqliteStmt::col_text(int idx) const {
  const unsigned char *text = sqlite3_column_text(stmt_, idx);
  if (text == nullptr) {
    return "";
  }
  const int len = sqlite3_column_bytes(stmt_, idx);
  return {reinterpret_cast<const char *>(text), static_cast<std::size_t>(len)};
}

// -- SqliteDb ----------------------------------------------------------------

SqliteDb::SqliteDb(const std::string &path, bool read_only,
                   SqliteProfile profile)
    : profile_(profile) {
  const bool profile_is_read_only =
      profile == SqliteProfile::interactive_read ||
      profile == SqliteProfile::read_only_replay;
  if (profile_is_read_only != read_only) {
    throw StorageError("SQLite profile/open-mode mismatch for " + path);
  }
  // Design §4.2: the RETURNING upserts are the only path shipped; refuse to
  // run against a pre-3.35 runtime with a clear message instead of a SQL
  // syntax error later.
  if (sqlite3_libversion_number() < 3035000) {
    throw StorageError(std::string("cidx requires SQLite >= 3.35 (RETURNING "
                                   "support); found ") +
                       sqlite3_libversion());
  }
  const int flags = read_only ? SQLITE_OPEN_READONLY
                              : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
  const int rc = sqlite3_open_v2(path.c_str(), &db_, flags, nullptr);
  if (rc != SQLITE_OK) {
    std::string msg = (db_ != nullptr) ? sqlite3_errmsg(db_) : "out of memory";
    sqlite3_close(db_);
    db_ = nullptr;
    throw StorageError("cannot open database " + path + ": " + msg);
  }
  try {
    const auto settings = sqlite_profile_settings(profile_);
    if (sqlite3_busy_timeout(db_, settings.busy_timeout_ms) != SQLITE_OK) {
      throw_db_error(db_, "cannot configure SQLite busy timeout");
    }
    if (settings.foreign_keys) {
      exec("PRAGMA foreign_keys = ON");
    }
    if (settings.query_only) {
      exec("PRAGMA query_only = ON");
    }
    if (read_only || settings.query_only) {
      return;
    }
    // CIDX ships rollback journaling with FULL synchronous durability. WAL is
    // a benchmark candidate only; it is not enabled without measured
    // operational and atomicity evidence for the active workload.
    if (settings.rollback_journal) {
      exec("PRAGMA journal_mode = DELETE");
    }
    if (settings.full_synchronous) {
      exec("PRAGMA synchronous = FULL");
    }
    profile::apply_sqlite_experiment(db_);
  } catch (...) {
    sqlite3_close(db_);
    db_ = nullptr;
    throw;
  }
}

SqliteDb::~SqliteDb() {
  finalize_cached_statements();
  sqlite3_close(db_);
}

// A pooled statement is reset and unbound before it is handed out again, so a
// reused statement starts in exactly the state a freshly compiled one would.
sqlite3_stmt *SqliteDb::take_cached_statement(std::string_view sql) {
  const auto pooled = statement_cache_.find(sql);
  if (pooled == statement_cache_.end() || pooled->second.empty()) {
    return nullptr;
  }
  sqlite3_stmt *stmt = pooled->second.back();
  pooled->second.pop_back();
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  return stmt;
}

void SqliteDb::release_statement(const std::string &sql,
                                 sqlite3_stmt *stmt) noexcept {
  // sqlite3_reset reports the last step's error, which the caller has already
  // seen; the reset itself still releases the statement's locks and rows.
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  // Pooling is an optimization: if the pool is full, or growing it would
  // allocate and cannot, finalize the statement and carry on.
  bool pooled_for_reuse = false;
  try {
    const auto pooled = statement_cache_.find(sql);
    if (pooled != statement_cache_.end()) {
      if (pooled->second.size() < kMaxCachedStatementsPerText) {
        pooled->second.push_back(stmt);
        pooled_for_reuse = true;
      }
    } else if (statement_cache_.size() < kMaxCachedStatementTexts) {
      statement_cache_.emplace(sql, std::vector<sqlite3_stmt *>{stmt});
      pooled_for_reuse = true;
    }
  } catch (...) {
    pooled_for_reuse = false;
  }
  if (!pooled_for_reuse) {
    sqlite3_finalize(stmt);
  }
}

void SqliteDb::finalize_cached_statements() noexcept {
  for (auto &[sql, pooled] : statement_cache_) {
    for (sqlite3_stmt *stmt : pooled) {
      sqlite3_finalize(stmt);
    }
  }
  statement_cache_.clear();
}

SqliteStmt SqliteDb::prepare(std::string_view sql) {
  const bool profiling = profile::active();
  const auto started = profiling ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
  sqlite3_stmt *stmt = take_cached_statement(sql);
  if (stmt == nullptr) {
    const int rc = sqlite3_prepare_v2(
        db_, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr);
    if (rc != SQLITE_OK) {
      throw_db_error(db_, "prepare failed for \"" + std::string(sql) + "\"");
    }
  }
  if (profiling) {
    profile::note_sqlite_prepare(std::chrono::duration<double>(
                                     std::chrono::steady_clock::now() - started)
                                     .count(),
                                 sql.size());
  }
  return {*this, std::string(sql), stmt};
}

void SqliteDb::exec(std::string_view sql_script) {
  char *err = nullptr;
  const int rc = sqlite3_exec(db_, std::string(sql_script).c_str(), nullptr,
                              nullptr, &err);
  if (rc != SQLITE_OK) {
    std::string msg = (err != nullptr) ? err : "unknown SQLite error";
    sqlite3_free(err);
    throw StorageError("exec failed: " + msg);
  }
}

int64_t SqliteDb::changes() const {
#if SQLITE_VERSION_NUMBER >= 3037000
  return sqlite3_changes64(const_cast<sqlite3 *>(db_));
#else
  return static_cast<int64_t>(sqlite3_changes(const_cast<sqlite3 *>(db_)));
#endif
}

int SqliteDb::variable_limit() const {
  return sqlite3_limit(const_cast<sqlite3 *>(db_), SQLITE_LIMIT_VARIABLE_NUMBER,
                       -1);
}

SqliteDb::VariableLimitOverrideForTesting::VariableLimitOverrideForTesting(
    SqliteDb &db, int limit)
    : db_(&db),
      previous_(sqlite3_limit(db.db_, SQLITE_LIMIT_VARIABLE_NUMBER, limit)) {}

SqliteDb::VariableLimitOverrideForTesting::~VariableLimitOverrideForTesting() {
  if (db_ != nullptr) {
    sqlite3_limit(db_->db_, SQLITE_LIMIT_VARIABLE_NUMBER, previous_);
  }
}

auto SqliteDb::backup_to(std::string_view path) const -> void {
  if (path.empty()) {
    throw StorageError("cannot back up SQLite database to an empty path");
  }
  sqlite3 *destination = nullptr;
  const std::string destination_path(path);
  const int open_rc =
      sqlite3_open_v2(destination_path.c_str(), &destination,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
  if (open_rc != SQLITE_OK) {
    std::string message = (destination != nullptr) ? sqlite3_errmsg(destination)
                                                   : "out of memory";
    sqlite3_close(destination);
    throw StorageError("cannot open backup database " + destination_path +
                       ": " + message);
  }

  sqlite3_backup *backup =
      sqlite3_backup_init(destination, "main", db_, "main");
  if (backup == nullptr) {
    const std::string message = sqlite3_errmsg(destination);
    sqlite3_close(destination);
    throw StorageError("cannot initialize SQLite backup: " + message);
  }
  const int step_rc = sqlite3_backup_step(backup, -1);
  const int finish_rc = sqlite3_backup_finish(backup);
  if (step_rc != SQLITE_DONE || finish_rc != SQLITE_OK) {
    const std::string message = sqlite3_errmsg(destination);
    sqlite3_close(destination);
    throw StorageError("SQLite backup failed: " + message);
  }
  if (sqlite3_close(destination) != SQLITE_OK) {
    throw StorageError("cannot close backup database " + destination_path);
  }
}

} // namespace cidx
