#include "storage/database_verification.hpp"

#include <array>
#include <exception>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <vector>

#include "storage/sqlite.hpp"
#include "util/hashing.hpp"

namespace cidx::storage {
namespace {

// A read-only handle. `read_only_replay` is the profile that pairs with
// SQLITE_OPEN_READONLY, so the connection is query_only and the file on disk
// is never written.
auto open_read_only(const std::string &path) -> SqliteDb {
  return SqliteDb(path, true, SqliteProfile::read_only_replay);
}

// One digested section: a stable label plus the rows it contributed. Sections
// are folded into the digest in declaration order, each prefixed by its label
// and row count, so an empty family and an absent family cannot collide.
struct DigestSection {
  std::string_view label;
  std::string_view sql;
};

// Portable projections: every column is content, never a database-local row
// id, and every statement carries its own deterministic ORDER BY. `component`
// name plus the component-relative directory path and file name identify a
// file without reconstructing an absolute path (which depends on the active
// clone and therefore on the machine).
constexpr std::string_view kFileSql = R"SQL(
SELECT c.name, d.path, f.name, IFNULL(f.md5, ''), f.indexed,
       IFNULL(f.driver, ''), IFNULL(f.compile_options, ''), f.args_overridden
FROM file f
JOIN directory d ON d.id = f.directory_id
JOIN component c ON c.id = d.component_id
ORDER BY 1, 2, 3
)SQL";

constexpr std::string_view kSymbolSql = R"SQL(
SELECT IFNULL(u.key, ''), s.identity_key, s.usr, s.spelling,
       IFNULL(s.qual_name, ''), IFNULL(s.display_name, ''), s.kind,
       s.is_definition, IFNULL(c.name, ''), IFNULL(d.path, ''),
       IFNULL(f.name, ''), IFNULL(s.line, -1), IFNULL(s.col, -1)
FROM symbol s
LEFT JOIN semantic_universe u ON u.id = s.semantic_universe_id
LEFT JOIN file f ON f.id = s.file_id
LEFT JOIN directory d ON d.id = f.directory_id
LEFT JOIN component c ON c.id = d.component_id
ORDER BY 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13
)SQL";

constexpr std::string_view kEdgeSql = R"SQL(
SELECT src.usr, IFNULL(src.identity_key, ''), dst.usr,
       IFNULL(dst.identity_key, ''), e.kind, e.count,
       IFNULL(e.base_access, -1), IFNULL(e.is_virtual, -1)
FROM edge e
JOIN symbol src ON src.id = e.src_id
JOIN symbol dst ON dst.id = e.dst_id
ORDER BY 1, 2, 3, 4, 5
)SQL";

constexpr std::string_view kDefinitionSql = R"SQL(
SELECT s.usr, IFNULL(s.identity_key, ''), IFNULL(c.name, ''),
       IFNULL(d.path, ''), IFNULL(f.name, ''), IFNULL(dn.line, -1),
       IFNULL(dn.col, -1)
FROM definition dn
JOIN symbol s ON s.id = dn.symbol_id
LEFT JOIN file f ON f.id = dn.file_id
LEFT JOIN directory d ON d.id = f.directory_id
LEFT JOIN component c ON c.id = d.component_id
ORDER BY 1, 2, 3, 4, 5, 6, 7
)SQL";

constexpr std::string_view kDiagnosticSql = R"SQL(
SELECT IFNULL(c.name, ''), IFNULL(d.path, ''), IFNULL(f.name, ''),
       g.severity, g.spelling, IFNULL(g.file_path, ''), IFNULL(g.line, -1),
       IFNULL(g.col, -1)
FROM diagnostic g
LEFT JOIN file f ON f.id = g.file_id
LEFT JOIN directory d ON d.id = f.directory_id
LEFT JOIN component c ON c.id = d.component_id
ORDER BY 1, 2, 3, 4, 5, 6, 7, 8
)SQL";

constexpr std::string_view kIncludeSql = R"SQL(
SELECT IFNULL(sc.name, ''), IFNULL(sd.path, ''), IFNULL(sf.name, ''),
       IFNULL(tc.name, ''), IFNULL(td.path, ''), IFNULL(tf.name, ''),
       ie.dst_path, ie.is_system, ie.is_generated, ie.count
FROM include_edge ie
LEFT JOIN file sf ON sf.id = ie.src_file_id
LEFT JOIN directory sd ON sd.id = sf.directory_id
LEFT JOIN component sc ON sc.id = sd.component_id
LEFT JOIN file tf ON tf.id = ie.dst_file_id
LEFT JOIN directory td ON td.id = tf.directory_id
LEFT JOIN component tc ON tc.id = td.component_id
ORDER BY 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
)SQL";

constexpr std::string_view kCatalogUniverseSql =
    "SELECT key, name, policy FROM semantic_universe ORDER BY 1, 2, 3";
constexpr std::string_view kCatalogRepositorySql = R"SQL(
SELECT r.name, r.kind, IFNULL(r.remote_url, ''), IFNULL(ac.path, ''),
       IFNULL(u.key, '')
FROM repository r
LEFT JOIN clone ac ON ac.id = r.active_clone_id
LEFT JOIN semantic_universe u ON u.id = r.semantic_universe_id
ORDER BY 1, 2, 3, 4, 5
)SQL";
constexpr std::string_view kCatalogCloneSql = R"SQL(
SELECT r.name, cl.path, IFNULL(cl.label, '')
FROM clone cl JOIN repository r ON r.id = cl.repository_id
ORDER BY 1, 2, 3
)SQL";
constexpr std::string_view kCatalogComponentSql = R"SQL(
SELECT c.name, c.path, c.kind, IFNULL(c.version, ''), IFNULL(r.name, ''),
       IFNULL(u.key, '')
FROM component c
LEFT JOIN repository r ON r.id = c.repository_id
LEFT JOIN semantic_universe u ON u.id = c.semantic_universe_id
ORDER BY 1, 2, 3, 4, 5, 6
)SQL";
constexpr std::string_view kCatalogFileSql = R"SQL(
SELECT c.name, d.path, f.name, IFNULL(f.compile_options, ''),
       IFNULL(f.driver, ''), f.args_overridden
FROM file f
JOIN directory d ON d.id = f.directory_id
JOIN component c ON c.id = d.component_id
ORDER BY 1, 2, 3
)SQL";
constexpr std::string_view kCatalogLabelSql =
    "SELECT name, path FROM label ORDER BY 1, 2";

// Append one row's cells to `out` with unambiguous framing: every cell is
// length-prefixed, so no combination of cell contents can imitate a different
// row shape.
void append_row(std::string &out, SqliteStmt &statement) {
  const int columns = statement.column_count();
  for (int column = 0; column < columns; ++column) {
    if (statement.col_is_null(column)) {
      out += "N:";
      continue;
    }
    const std::string cell = statement.col_text(column);
    out += std::to_string(cell.size());
    out += ':';
    out += cell;
    out += '\x1f';
  }
  out += '\x1e';
}

// Fold one section into the accumulating digest input.
void append_section(std::string &out, SqliteDb &db, std::string_view label,
                    std::string_view sql) {
  out += label;
  out += '\x01';
  std::string rows;
  std::int64_t count = 0;
  SqliteStmt statement = db.prepare(sql);
  while (statement.step()) {
    append_row(rows, statement);
    ++count;
  }
  out += std::to_string(count);
  out += '\x02';
  out += rows;
  out += '\x03';
}

auto scalar_count(SqliteDb &db, std::string_view sql) -> std::int64_t {
  SqliteStmt statement = db.prepare(sql);
  return statement.step() ? statement.col_int64(0) : 0;
}

} // namespace

auto semantic_digest_fact_families() -> std::vector<std::string_view> {
  return {"file", "symbol", "edge", "definition", "diagnostic", "include_edge"};
}

auto database_uses_write_ahead_log(const std::string &path) -> bool {
  // The 100-byte SQLite header: the magic string, then the format write and
  // read version bytes at offsets 18 and 19. 2 means WAL in either position.
  static constexpr std::string_view kMagic = "SQLite format 3";
  static constexpr std::streamsize kHeaderBytes = 20;
  static constexpr std::size_t kWriteVersionOffset = 18;
  static constexpr std::size_t kReadVersionOffset = 19;
  static constexpr char kWalVersion = 2;

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  std::array<char, kHeaderBytes> header{};
  file.read(header.data(), kHeaderBytes);
  if (file.gcount() != kHeaderBytes) {
    return false;
  }
  if (std::string_view(header.data(), kMagic.size()) != kMagic) {
    return false;
  }
  return header.at(kWriteVersionOffset) == kWalVersion ||
         header.at(kReadVersionOffset) == kWalVersion;
}

auto database_sidecar_paths(const std::string &path)
    -> std::vector<std::string> {
  return {path + "-wal", path + "-shm", path + "-journal"};
}

auto inspect_database_integrity(const std::string &path)
    -> DatabaseIntegrityReport {
  DatabaseIntegrityReport report;
  try {
    SqliteDb db = open_read_only(path);
    report.opened = true;
    {
      SqliteStmt statement = db.prepare("PRAGMA integrity_check");
      std::string first;
      while (statement.step()) {
        const std::string row = statement.col_text(0);
        if (first.empty()) {
          first = row;
        }
      }
      report.integrity_ok = first == "ok";
      if (!report.integrity_ok) {
        report.detail =
            first.empty() ? "integrity_check returned no rows" : first;
      }
    }
    {
      SqliteStmt statement = db.prepare("PRAGMA foreign_key_check");
      while (statement.step()) {
        ++report.foreign_key_violations;
      }
      report.foreign_keys_ok = report.foreign_key_violations == 0;
    }
    {
      SqliteStmt statement =
          db.prepare("SELECT value FROM meta WHERE key = 'schema_version'");
      if (statement.step() && !statement.col_is_null(0)) {
        const std::string value = statement.col_text(0);
        try {
          report.schema_version = std::stoi(value);
        } catch (const std::exception &) {
          report.schema_version = 0;
        }
      }
    }
  } catch (const std::exception &error) {
    report.opened = false;
    report.detail = error.what();
  }
  return report;
}

auto read_database_catalog_identity(const std::string &path)
    -> DatabaseCatalogIdentity {
  DatabaseCatalogIdentity identity;
  SqliteDb db = open_read_only(path);
  identity.universes =
      scalar_count(db, "SELECT COUNT(*) FROM semantic_universe");
  identity.repositories = scalar_count(db, "SELECT COUNT(*) FROM repository");
  identity.clones = scalar_count(db, "SELECT COUNT(*) FROM clone");
  identity.components = scalar_count(db, "SELECT COUNT(*) FROM component");
  identity.files = scalar_count(db, "SELECT COUNT(*) FROM file");
  identity.labels = scalar_count(db, "SELECT COUNT(*) FROM label");

  static constexpr std::array<DigestSection, 6> kCatalogSections{{
      {.label = "semantic_universe", .sql = kCatalogUniverseSql},
      {.label = "repository", .sql = kCatalogRepositorySql},
      {.label = "clone", .sql = kCatalogCloneSql},
      {.label = "component", .sql = kCatalogComponentSql},
      {.label = "file", .sql = kCatalogFileSql},
      {.label = "label", .sql = kCatalogLabelSql},
  }};
  std::string material = "cidx-catalog-v1\n";
  for (const DigestSection &section : kCatalogSections) {
    append_section(material, db, section.label, section.sql);
  }
  identity.digest = sha256_hex(material);
  return identity;
}

auto read_database_semantic_digest(const std::string &path) -> std::string {
  SqliteDb db = open_read_only(path);
  static constexpr std::array<DigestSection, 6> kSections{{
      {.label = "file", .sql = kFileSql},
      {.label = "symbol", .sql = kSymbolSql},
      {.label = "edge", .sql = kEdgeSql},
      {.label = "definition", .sql = kDefinitionSql},
      {.label = "diagnostic", .sql = kDiagnosticSql},
      {.label = "include_edge", .sql = kIncludeSql},
  }};
  std::string material = "cidx-semantic-v1\n";
  for (const DigestSection &section : kSections) {
    append_section(material, db, section.label, section.sql);
  }
  return sha256_hex(material);
}

auto read_database_pending_file_count(const std::string &path) -> std::int64_t {
  SqliteDb db = open_read_only(path);
  return scalar_count(db, "SELECT COUNT(*) FROM file WHERE indexed = 0");
}

} // namespace cidx::storage
