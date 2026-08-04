// Thin RAII wrappers over the SQLite3 C API (design D3).
// Smallest possible layer under storage/storage.hpp: no ORM, and no rewriting
// or generation of SQL — every statement is still the caller's own text. The C
// API autocommits each statement unless an explicit BEGIN is open, which is
// exactly the Python `_commit()`-unless-in-txn pattern once Transaction
// (storage.hpp) issues BEGIN/COMMIT/ROLLBACK.
//
// prepare() keeps a bounded per-connection pool of compiled statements keyed by
// the SQL text, because the indexing write path re-issues the same handful of
// long statements once per row and sqlite3_prepare_v2 recompiles the text every
// time. Reuse is transparent: a pooled statement is reset and unbound before it
// is handed out, it is removed from the pool while in use, and the pool is
// finalized before the connection closes.
//
// SQLite floor: >= 3.37 (RETURNING + sqlite3_changes64). Probed on the
// gcc-index-test box (192.168.1.115, Ubuntu 24.04: libsqlite3 3.45.1) — design
// §4.2: the RETURNING path is the ONLY path shipped; the ctor asserts the
// runtime library version.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace cidx {

// One dynamically-typed SQL parameter / cell (update_symbol values, query
// args). Mirrors Python's None/int/float/str binding.
using SqlValue = std::variant<std::nullptr_t, int64_t, double, std::string>;

// Runtime profiles are deliberately narrower than SQLite's tuning surface.
// They name the supported lifecycle contract; benchmark-only WAL and cache
// settings are not silently promoted to a production profile.
enum class SqliteProfile : std::uint8_t {
  indexing,
  interactive_read,
  read_only_replay,
  migration,
  maintenance,
  artifact_staging,
};

struct SqliteProfileSettings {
  int busy_timeout_ms;
  bool foreign_keys;
  bool query_only;
  bool rollback_journal;
  bool full_synchronous;
};

[[nodiscard]] auto sqlite_profile_settings(SqliteProfile profile)
    -> SqliteProfileSettings;
[[nodiscard]] auto sqlite_profile_name(SqliteProfile profile)
    -> std::string_view;

class SqliteDb;

struct SqliteStatementStats {
  std::uint64_t executions = 0;
  std::uint64_t reuses = 0;
  std::uint64_t virtual_machine_steps = 0;
  std::uint64_t fullscan_steps = 0;
  std::uint64_t reprepares = 0;
  double step_seconds = 0.0;
};

// Per-step VM-step / timing sampling costs two steady_clock reads and three
// sqlite3_stmt_status calls, so it is opt-in: SqliteStmt::step() samples only
// while an indexing profiling session is active (profile::active()) or while a
// StatementMeasurementScope is open. Statement execution and reuse counts are
// plain increments and are always maintained.
namespace detail {
extern std::atomic_int statement_measurement_depth;
} // namespace detail

[[nodiscard]] inline auto statement_measurement_active() noexcept -> bool {
  return detail::statement_measurement_depth.load(std::memory_order_relaxed) >
         0;
}

// RAII: turns per-step sampling on for callers that consume a statement report
// without running a full profiling session (FactBatchWriter, benchmarks,
// tests). Nesting is refcounted.
class StatementMeasurementScope {
public:
  StatementMeasurementScope() noexcept {
    detail::statement_measurement_depth.fetch_add(1, std::memory_order_relaxed);
  }
  ~StatementMeasurementScope() {
    detail::statement_measurement_depth.fetch_sub(1, std::memory_order_relaxed);
  }
  StatementMeasurementScope(const StatementMeasurementScope &) = delete;
  StatementMeasurementScope &
  operator=(const StatementMeasurementScope &) = delete;
  StatementMeasurementScope(StatementMeasurementScope &&) = delete;
  StatementMeasurementScope &operator=(StatementMeasurementScope &&) = delete;
};

// Owns a sqlite3_stmt*. Movable, non-copyable. Bind indexes are 1-based,
// column indexes 0-based (SQLite convention).
//
// When it was handed out by SqliteDb::prepare it returns the compiled
// statement to that connection's bounded reuse pool instead of finalizing it,
// so a hot loop compiles its SQL once instead of once per row. The SQL text
// itself is never rewritten.
class SqliteStmt {
public:
  SqliteStmt(sqlite3 *db, std::string_view sql); // throws StorageError
  SqliteStmt(SqliteDb &owner, std::string sql, sqlite3_stmt *compiled,
             bool reused) noexcept;
  ~SqliteStmt();
  SqliteStmt(SqliteStmt &&other) noexcept;
  SqliteStmt &operator=(SqliteStmt &&other) noexcept;
  SqliteStmt(const SqliteStmt &) = delete;
  SqliteStmt &operator=(const SqliteStmt &) = delete;

  void bind_null(int idx);
  void bind(int idx, int64_t value);
  void bind(int idx, double value);
  void bind(int idx, std::string_view value);
  void bind(int idx, const SqlValue &value);

  // true = SQLITE_ROW, false = SQLITE_DONE; throws StorageError otherwise.
  bool step();
  // Runs the statement to completion (e.g. DML with RETURNING).
  void step_done();
  void reset();

  [[nodiscard]] bool readonly() const;
  [[nodiscard]] int column_count() const;

  [[nodiscard]] bool col_is_null(int idx) const;
  [[nodiscard]] int64_t col_int64(int idx) const;
  [[nodiscard]] double col_double(int idx) const;
  [[nodiscard]] std::string col_text(int idx) const; // NULL -> ""
  [[nodiscard]] auto stats() const -> const SqliteStatementStats & {
    return stats_;
  }
  [[nodiscard]] auto reused_compiled_statement() const -> bool {
    return reused_compiled_statement_;
  }

private:
  sqlite3 *db_ = nullptr;
  sqlite3_stmt *stmt_ = nullptr;
  SqliteDb *owner_ = nullptr; // non-null: return to the pool, do not finalize
  std::string sql_;
  SqliteStatementStats stats_;
  bool execution_started_ = false;
  bool reused_compiled_statement_ = false;
};

// Owns a sqlite3*. Non-copyable, non-movable (Storage holds it by value).
class SqliteDb {
public:
  // read_only opens with SQLITE_OPEN_READONLY (no CREATE): the file must
  // already exist; any write statement fails at the SQLite layer.
  explicit SqliteDb(const std::string &path, bool read_only = false,
                    SqliteProfile profile = SqliteProfile::indexing);
  explicit SqliteDb(int source_fd, bool read_only = false,
                    SqliteProfile profile = SqliteProfile::indexing);
  ~SqliteDb();
  SqliteDb(const SqliteDb &) = delete;
  SqliteDb &operator=(const SqliteDb &) = delete;

  SqliteStmt prepare(std::string_view sql);
  void exec(std::string_view sql_script); // multi-statement, throws on error
  [[nodiscard]] int variable_limit() const;
  class VariableLimitOverrideForTesting {
  public:
    VariableLimitOverrideForTesting(SqliteDb &db, int limit);
    ~VariableLimitOverrideForTesting();
    VariableLimitOverrideForTesting(const VariableLimitOverrideForTesting &) =
        delete;
    VariableLimitOverrideForTesting &
    operator=(const VariableLimitOverrideForTesting &) = delete;

  private:
    SqliteDb *db_ = nullptr;
    int previous_ = 0;
  };
  [[nodiscard]] int64_t changes() const; // rows affected by the last DML
  auto backup_to(std::string_view path) const -> void;
  auto backup_to_fd(int destination_fd) const -> void;
  [[nodiscard]] auto profile() const -> SqliteProfile { return profile_; }
  sqlite3 *raw() { return db_; }

private:
  friend class SqliteStmt;

  // Bounded reuse pool. A statement is removed from it while in use, so two
  // live SqliteStmt objects for the same SQL never share one sqlite3_stmt.
  // Both bounds are per connection: at most kMaxCachedStatementTexts distinct
  // SQL texts, each holding at most kMaxCachedStatementsPerText statements.
  static constexpr std::size_t kMaxCachedStatementTexts = 256;
  static constexpr std::size_t kMaxCachedStatementsPerText = 4;

  auto take_cached_statement(std::string_view sql) -> sqlite3_stmt *;
  void release_statement(const std::string &sql, sqlite3_stmt *stmt) noexcept;
  void finalize_cached_statements() noexcept;

  // Transparent so a std::string_view SQL text is looked up without first
  // materialising a std::string key on every prepare().
  struct SqlTextHash {
    using is_transparent = void;
    auto operator()(std::string_view sql) const noexcept -> std::size_t {
      return std::hash<std::string_view>{}(sql);
    }
  };

  sqlite3 *db_ = nullptr;
  SqliteProfile profile_ = SqliteProfile::indexing;
  std::unordered_map<std::string, std::vector<sqlite3_stmt *>, SqlTextHash,
                     std::equal_to<>>
      statement_cache_;
};

} // namespace cidx
