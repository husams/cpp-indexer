// SQLite-backed implementation of the query read capability.
#pragma once

#include "storage/sqlite.hpp"

#include <stdexcept>
#include <string_view>

namespace cidx::storage {

// Read-only SQL capability. The returned statement is prepared through
// SQLite's readonly classification; callers cannot obtain the owning
// connection, execute scripts, or prepare DML through this interface.
class SqliteReadDb final {
public:
  explicit SqliteReadDb(SqliteDb &db) : db_(&db) {}

  [[nodiscard]] SqliteStmt prepare(std::string_view sql) const {
    SqliteStmt statement = db_->prepare(sql);
    if (!statement.readonly()) {
      throw std::logic_error("non-readonly SQL through SqliteReadDb");
    }
    return statement;
  }

private:
  SqliteDb *db_;
};

} // namespace cidx::storage
