#include "analysis/facts.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <tuple>
#include <utility>

#include "astgraph/schema.hpp"
#include "catalogs/generated_catalog.hpp"
#include "storage/sqlite.hpp"
#include "storage/storage.hpp"
#include "util/errors.hpp"
#include "util/hashing.hpp"

namespace cidx::analysis {

namespace {

namespace fs = std::filesystem;

struct SqlRelationSpec {
  RelationDescriptor descriptor;
  const char *sql;
};

RelationDescriptor descriptor(std::string name,
                              std::initializer_list<FactColumn> columns) {
  return RelationDescriptor{.name = std::move(name),
                            .version = 1,
                            .catalog_version = catalog::kCatalogVersion,
                            .columns = columns};
}

std::array<SqlRelationSpec, 10> semantic_relations() {
  return {
      SqlRelationSpec{
          .descriptor = descriptor(
              "symbol", {{.name = "id", .type = FactType::integer},
                         {.name = "usr", .type = FactType::string},
                         {.name = "spelling", .type = FactType::string},
                         {.name = "qual_name", .type = FactType::string},
                         {.name = "kind", .type = FactType::integer},
                         {.name = "is_definition", .type = FactType::integer},
                         {.name = "file_id", .type = FactType::integer},
                         {.name = "line", .type = FactType::integer}}),
          .sql = "SELECT id, usr, spelling, COALESCE(qual_name, ''), kind, "
                 "is_definition, COALESCE(file_id, 0), COALESCE(line, 0) "
                 "FROM symbol ORDER BY id"},
      SqlRelationSpec{
          .descriptor = descriptor(
              "symbol_kind", {{.name = "id", .type = FactType::integer},
                              {.name = "name", .type = FactType::string}}),
          .sql = "SELECT id, name FROM symbol_kind ORDER BY id"},
      SqlRelationSpec{
          .descriptor = descriptor(
              "edge", {{.name = "id", .type = FactType::integer},
                       {.name = "src_id", .type = FactType::integer},
                       {.name = "dst_id", .type = FactType::integer},
                       {.name = "kind", .type = FactType::integer},
                       {.name = "count", .type = FactType::integer}}),
          .sql =
              "SELECT id, src_id, dst_id, kind, count FROM edge ORDER BY id"},
      SqlRelationSpec{
          .descriptor = descriptor(
              "edge_kind", {{.name = "id", .type = FactType::integer},
                            {.name = "name", .type = FactType::string}}),
          .sql = "SELECT id, name FROM edge_kind ORDER BY id"},
      SqlRelationSpec{
          .descriptor = descriptor(
              "edge_site", {{.name = "edge_id", .type = FactType::integer},
                            {.name = "file_id", .type = FactType::integer},
                            {.name = "line", .type = FactType::integer},
                            {.name = "col", .type = FactType::integer}}),
          .sql = "SELECT edge_id, file_id, COALESCE(line, 0), COALESCE(col, 0) "
                 "FROM edge_site ORDER BY edge_id, file_id, line, col"},
      SqlRelationSpec{
          .descriptor = descriptor(
              "entity_node", {{.name = "id", .type = FactType::integer},
                              {.name = "kind", .type = FactType::string}}),
          .sql = "SELECT id, kind FROM entity_node ORDER BY id"},
      SqlRelationSpec{
          .descriptor = descriptor(
              "entity_kind", {{.name = "id", .type = FactType::integer},
                              {.name = "name", .type = FactType::string}}),
          .sql = "SELECT id, name FROM entity_kind ORDER BY id"},
      SqlRelationSpec{
          .descriptor =
              descriptor("entity_edge",
                         {{.name = "src_id", .type = FactType::integer},
                          {.name = "dst_id", .type = FactType::integer},
                          {.name = "kind", .type = FactType::integer},
                          {.name = "via_member_id", .type = FactType::integer},
                          {.name = "access", .type = FactType::integer},
                          {.name = "is_virtual", .type = FactType::integer}}),
          .sql = "SELECT src_id, dst_id, kind, COALESCE(via_member_id, 0), "
                 "access, "
                 "is_virtual FROM entity_edge ORDER BY src_id, dst_id, kind, "
                 "COALESCE(via_member_id, 0)"},
      SqlRelationSpec{
          .descriptor = descriptor(
              "entity_edge_kind", {{.name = "id", .type = FactType::integer},
                                   {.name = "name", .type = FactType::string}}),
          .sql = "SELECT id, name FROM entity_edge_kind ORDER BY id"},
      SqlRelationSpec{
          .descriptor =
              descriptor("file", {{.name = "id", .type = FactType::integer},
                                  {.name = "path", .type = FactType::string}}),
          .sql =
              "SELECT f.id, c.path || CASE WHEN d.path = '' THEN '' ELSE '/' "
              "|| "
              "d.path END || '/' || f.name FROM file f JOIN directory d ON "
              "f.directory_id = d.id JOIN component c ON d.component_id = c.id "
              "ORDER BY f.id"}};
}

std::array<SqlRelationSpec, 6> astgraph_relations() {
  return {
      SqlRelationSpec{
          .descriptor = descriptor(
              "ast_file", {{.name = "id", .type = FactType::integer},
                           {.name = "path", .type = FactType::string},
                           {.name = "is_main", .type = FactType::integer}}),
          .sql = "SELECT id, path, is_main FROM file ORDER BY id"},
      SqlRelationSpec{
          .descriptor =
              descriptor("ast_node_kind",
                         {{.name = "id", .type = FactType::integer},
                          {.name = "name", .type = FactType::string},
                          {.name = "category", .type = FactType::string}}),
          .sql = "SELECT id, name, category FROM node_kind ORDER BY id"},
      SqlRelationSpec{.descriptor = descriptor(
                          "ast_relation_kind",
                          {{.name = "id", .type = FactType::integer},
                           {.name = "name", .type = FactType::string}}),
                      .sql = "SELECT id, name FROM relation_kind ORDER BY id"},
      SqlRelationSpec{
          .descriptor = descriptor(
              "ast_symbol", {{.name = "id", .type = FactType::integer},
                             {.name = "usr", .type = FactType::string},
                             {.name = "name", .type = FactType::string},
                             {.name = "kind_id", .type = FactType::integer},
                             {.name = "linkage", .type = FactType::integer}}),
          .sql =
              "SELECT id, usr, name, kind_id, linkage FROM symbol ORDER BY id"},
      SqlRelationSpec{
          .descriptor = descriptor(
              "ast_node",
              {{.name = "id", .type = FactType::integer},
               {.name = "kind_id", .type = FactType::integer},
               {.name = "symbol_id", .type = FactType::integer},
               {.name = "type_id", .type = FactType::integer},
               {.name = "spelling", .type = FactType::string},
               {.name = "file_id", .type = FactType::integer},
               {.name = "line", .type = FactType::integer},
               {.name = "col", .type = FactType::integer},
               {.name = "end_line", .type = FactType::integer},
               {.name = "end_col", .type = FactType::integer},
               {.name = "is_definition", .type = FactType::integer}}),
          .sql = "SELECT id, kind_id, symbol_id, type_id, spelling, file_id, "
                 "line, "
                 "col, end_line, end_col, is_definition FROM node ORDER BY id"},
      SqlRelationSpec{
          .descriptor = descriptor(
              "ast_edge", {{.name = "src_id", .type = FactType::integer},
                           {.name = "dst_id", .type = FactType::integer},
                           {.name = "rel_id", .type = FactType::integer},
                           {.name = "ord", .type = FactType::integer}}),
          .sql =
              "SELECT src_id, dst_id, rel_id, ord FROM edge ORDER BY src_id, "
              "dst_id, rel_id, ord"}};
}

std::optional<std::string> meta_value(SqliteDb &db, std::string_view key) {
  SqliteStmt stmt = db.prepare("SELECT value FROM meta WHERE key = ?");
  stmt.bind(1, key);
  if (!stmt.step()) {
    return std::nullopt;
  }
  return stmt.col_text(0);
}

int meta_int(SqliteDb &db, std::string_view key, int fallback) {
  const auto value = meta_value(db, key);
  if (!value) {
    return fallback;
  }
  try {
    return std::stoi(*value);
  } catch (...) {
    throw CidxError("invalid " + std::string(key) + " metadata");
  }
}

FactCompleteness meta_completeness(SqliteDb &db) {
  const auto status = meta_value(db, "status");
  if (!status || *status == "complete") {
    return FactCompleteness::complete;
  }
  if (*status == "partial") {
    return FactCompleteness::partial;
  }
  if (*status == "stale") {
    return FactCompleteness::stale;
  }
  return FactCompleteness::unknown;
}

FactRelation load_relation(SqliteDb &db, const SqlRelationSpec &spec) {
  FactRelation relation{.descriptor = spec.descriptor, .rows = {}};
  SqliteStmt stmt = db.prepare(spec.sql);
  while (stmt.step()) {
    FactRow row;
    row.reserve(spec.descriptor.columns.size());
    for (std::size_t index = 0; index < spec.descriptor.columns.size();
         ++index) {
      if (spec.descriptor.columns[index].type == FactType::integer) {
        row.emplace_back(stmt.col_int64(static_cast<int>(index)));
      } else {
        row.emplace_back(stmt.col_text(static_cast<int>(index)));
      }
    }
    relation.rows.push_back(std::move(row));
  }
  return relation;
}

template <typename Range>
FactSnapshot load_sqlite_snapshot(const std::string &path,
                                  const FactRequest &request,
                                  const Range &specs, std::string provider,
                                  std::optional<std::string> artifact_path) {
  SqliteDb db(path, true, SqliteProfile::read_only_replay);
  FactSnapshot snapshot{
      .provider = std::move(provider),
      .workspace_identity =
          meta_value(db, "workspace_identity").value_or("unknown"),
      .tu_identity = meta_value(db, "tu_identity"),
      .applicability = meta_value(db, "applicability").value_or("workspace"),
      .schema_version = meta_int(db, "schema_version", 1),
      .catalog_version =
          meta_int(db, "catalog_version", catalog::kCatalogVersion),
      .catalog_hash = meta_value(db, "catalog_hash").value_or(""),
      .completeness = meta_completeness(db),
      .truncated = meta_value(db, "truncated").value_or("0") == "1",
      .evidence_references = {},
      .input_hashes = {},
      .artifact_path = std::move(artifact_path),
      .relations = {}};
  snapshot.input_hashes.push_back(
      sha256_of(path).value_or("unreadable:" + path));
  snapshot.evidence_references.push_back(snapshot.provider + ":" +
                                         snapshot.input_hashes.back());

  for (const auto &spec : specs) {
    if (request.relations.empty() ||
        std::ranges::find(request.relations, spec.descriptor.name) !=
            request.relations.end()) {
      snapshot.add_relation(load_relation(db, spec));
    }
  }
  for (const std::string &name : request.relations) {
    if (!snapshot.find_relation(name)) {
      throw CidxError("unsupported fact relation: " + name);
    }
  }
  if (request.workspace_identity &&
      snapshot.workspace_identity != *request.workspace_identity) {
    snapshot.completeness = FactCompleteness::unknown;
  }
  if (request.tu_identity && snapshot.tu_identity != request.tu_identity) {
    snapshot.completeness = FactCompleteness::unknown;
  }
  snapshot.validate();
  return snapshot;
}

std::string canonical_value(const FactValue &value) {
  return std::visit(
      [](const auto &item) -> std::string {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::same_as<T, std::int64_t>) {
          return "i:" + std::to_string(item);
        } else if constexpr (std::same_as<T, bool>) {
          return item ? "b:1" : "b:0";
        } else {
          return "s:" + std::to_string(item.size()) + ":" + item;
        }
      },
      value);
}

FactValue default_value(FactType type) {
  switch (type) {
  case FactType::integer:
    return std::int64_t{0};
  case FactType::boolean:
    return false;
  case FactType::string:
    return std::string{};
  }
  throw CidxError("unsupported fact type");
}

std::string join_key(const FactValue &value) { return canonical_value(value); }

const FactColumn &key_column(const FactRelation &relation,
                             std::string_view name) {
  const auto it =
      std::ranges::find(relation.descriptor.columns, name, &FactColumn::name);
  if (it == relation.descriptor.columns.end()) {
    throw CidxError("join key not found: " + std::string(name));
  }
  return *it;
}

std::size_t key_index(const FactRelation &relation, std::string_view name) {
  key_column(relation, name);
  return static_cast<std::size_t>(std::ranges::distance(
      relation.descriptor.columns.begin(),
      std::ranges::find(relation.descriptor.columns, name, &FactColumn::name)));
}

bool compatible_value(const FactValue &value, FactType type) {
  return (type == FactType::integer &&
          std::holds_alternative<std::int64_t>(value)) ||
         (type == FactType::boolean && std::holds_alternative<bool>(value)) ||
         (type == FactType::string &&
          std::holds_alternative<std::string>(value));
}

} // namespace

std::string fact_type_name(FactType type) {
  switch (type) {
  case FactType::integer:
    return "integer";
  case FactType::boolean:
    return "boolean";
  case FactType::string:
    return "string";
  }
  throw CidxError("unsupported fact type");
}

std::string fact_value_text(const FactValue &value) {
  return std::visit(
      [](const auto &item) -> std::string {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::same_as<T, bool>) {
          return item ? "1" : "0";
        } else if constexpr (std::same_as<T, std::int64_t>) {
          return std::to_string(item);
        } else {
          std::string text = item;
          for (char &character : text) {
            if (character == '\t' || character == '\n' || character == '\r') {
              character = ' ';
            }
          }
          return text;
        }
      },
      value);
}

void FactSnapshot::add_relation(FactRelation relation) {
  if (relation.descriptor.name.empty()) {
    throw CidxError("fact relation name cannot be empty");
  }
  relations.insert_or_assign(relation.descriptor.name, std::move(relation));
}

const FactRelation *FactSnapshot::find_relation(std::string_view name) const {
  const auto it = relations.find(std::string(name));
  return it == relations.end() ? nullptr : &it->second;
}

const FactRelation &
FactSnapshot::require_relation(std::string_view name) const {
  const FactRelation *relation = find_relation(name);
  if (relation == nullptr) {
    throw CidxError("missing fact relation: " + std::string(name));
  }
  return *relation;
}

void FactSnapshot::validate() const {
  if (provider.empty() || workspace_identity.empty() || applicability.empty() ||
      schema_version < 1 || catalog_version < 1) {
    throw CidxError("fact snapshot metadata is incomplete");
  }
  for (const auto &[name, relation] : relations) {
    if (name != relation.descriptor.name || relation.descriptor.version < 1 ||
        relation.descriptor.catalog_version < 1) {
      throw CidxError("fact relation descriptor is invalid: " + name);
    }
    for (const FactRow &row : relation.rows) {
      if (row.size() != relation.descriptor.columns.size()) {
        throw CidxError("fact row width mismatch: " + name);
      }
      for (std::size_t index = 0; index < row.size(); ++index) {
        if (!compatible_value(row[index],
                              relation.descriptor.columns[index].type)) {
          throw CidxError("fact row type mismatch: " + name);
        }
      }
    }
  }
}

std::string FactSnapshot::canonical() const {
  validate();
  std::ostringstream out;
  out << "provider=" << provider << '\n'
      << "workspace=" << workspace_identity << '\n'
      << "tu=" << tu_identity.value_or("") << '\n'
      << "applicability=" << applicability << '\n'
      << "schema=" << schema_version << '\n'
      << "catalog=" << catalog_version << ':' << catalog_hash << '\n'
      << "completeness=" << static_cast<int>(completeness) << '\n'
      << "truncated=" << (truncated ? 1 : 0) << '\n';
  for (const auto &hash : input_hashes) {
    out << "input=" << hash << '\n';
  }
  for (const auto &[name, relation] : relations) {
    out << "relation=" << name << ':' << relation.descriptor.version << ':'
        << relation.descriptor.catalog_version << '\n';
    for (const auto &column : relation.descriptor.columns) {
      out << "column=" << column.name << ':' << fact_type_name(column.type)
          << '\n';
    }
    for (const auto &row : relation.rows) {
      out << "row=";
      for (const auto &value : row) {
        out << canonical_value(value) << '\x1f';
      }
      out << '\n';
    }
  }
  return out.str();
}

std::string FactSnapshot::stable_hash() const {
  return sha256_hex(canonical());
}

StaticFactProvider::StaticFactProvider(FactSnapshot snapshot)
    : snapshot_(std::move(snapshot)) {
  snapshot_.validate();
}

FactSnapshot StaticFactProvider::snapshot(const FactRequest &request) const {
  if (request.workspace_identity &&
      snapshot_.workspace_identity != *request.workspace_identity) {
    FactSnapshot result = snapshot_;
    result.completeness = FactCompleteness::unknown;
    return result;
  }
  if (request.tu_identity && snapshot_.tu_identity != request.tu_identity) {
    FactSnapshot result = snapshot_;
    result.completeness = FactCompleteness::unknown;
    return result;
  }
  for (const auto &name : request.relations) {
    if (snapshot_.find_relation(name) == nullptr) {
      throw CidxError("unsupported fact relation: " + name);
    }
  }
  return snapshot_;
}

SqliteFactProvider::SqliteFactProvider(std::string path)
    : path_(std::move(path)) {}

FactSnapshot SqliteFactProvider::snapshot(const FactRequest &request) const {
  return load_sqlite_snapshot(path_, request, semantic_relations(),
                              "semantic-index", path_);
}

AstgraphFactProvider::AstgraphFactProvider(std::string path)
    : path_(std::move(path)) {}

FactSnapshot AstgraphFactProvider::snapshot(const FactRequest &request) const {
  FactSnapshot result = load_sqlite_snapshot(
      path_, request, astgraph_relations(), "astgraph", path_);
  if (result.schema_version != astgraph::kSchemaVersion) {
    throw CidxError("unsupported astgraph fact schema version: " +
                    std::to_string(result.schema_version));
  }
  return result;
}

FactSnapshot compose_snapshots(const FactSnapshot &left,
                               const FactSnapshot &right,
                               const std::vector<JoinSpec> &joins) {
  left.validate();
  right.validate();
  FactSnapshot result = left;
  result.provider = left.provider + "+" + right.provider;
  result.input_hashes.insert(result.input_hashes.end(),
                             right.input_hashes.begin(),
                             right.input_hashes.end());
  result.evidence_references.insert(result.evidence_references.end(),
                                    right.evidence_references.begin(),
                                    right.evidence_references.end());
  result.artifact_path.reset();
  if (left.workspace_identity != right.workspace_identity) {
    result.completeness = FactCompleteness::unknown;
  }
  for (const auto &[name, relation] : right.relations) {
    const std::string target = result.find_relation(name) != nullptr
                                   ? right.provider + "/" + name
                                   : name;
    FactRelation copy = relation;
    copy.descriptor.name = target;
    result.add_relation(std::move(copy));
  }

  FactRelation unresolved{
      .descriptor =
          descriptor("analysis/unresolved_join",
                     {{.name = "source_relation", .type = FactType::string},
                      {.name = "source_key", .type = FactType::string},
                      {.name = "target_relation", .type = FactType::string},
                      {.name = "target_key", .type = FactType::string},
                      {.name = "reason", .type = FactType::string}}),
      .rows = {}};
  for (const JoinSpec &join : joins) {
    const FactRelation &source = left.require_relation(join.left_relation);
    const FactRelation &target = right.require_relation(join.right_relation);
    const std::size_t source_index = key_index(source, join.left_key);
    const std::size_t target_index = key_index(target, join.right_key);
    if (source.descriptor.columns[source_index].type !=
        target.descriptor.columns[target_index].type) {
      throw CidxError("join key type mismatch: " + join.output_relation);
    }

    std::map<std::string, std::vector<const FactRow *>> matches;
    for (const FactRow &row : target.rows) {
      matches[join_key(row[target_index])].push_back(&row);
    }
    FactRelation output{.descriptor = descriptor(join.output_relation, {}),
                        .rows = {}};
    for (const auto &column : source.descriptor.columns) {
      output.descriptor.columns.push_back(
          {.name = "left_" + column.name, .type = column.type});
    }
    for (const auto &column : target.descriptor.columns) {
      output.descriptor.columns.push_back(
          {.name = "right_" + column.name, .type = column.type});
    }
    output.descriptor.columns.push_back(
        {.name = "join_status", .type = FactType::string});
    for (const FactRow &source_row : source.rows) {
      const std::string key = join_key(source_row[source_index]);
      const auto found = matches.find(key);
      if (found == matches.end()) {
        FactRow row = source_row;
        row.insert(row.end(), target.descriptor.columns.size(),
                   FactValue{std::int64_t{0}});
        for (std::size_t index = 0; index < target.descriptor.columns.size();
             ++index) {
          row[source.descriptor.columns.size() + index] =
              default_value(target.descriptor.columns[index].type);
        }
        row.emplace_back("unresolved");
        output.rows.push_back(std::move(row));
        unresolved.rows.push_back(
            {join.left_relation, fact_value_text(source_row[source_index]),
             join.right_relation, fact_value_text(source_row[source_index]),
             "missing_identity"});
        continue;
      }
      for (const FactRow *target_row : found->second) {
        FactRow row = source_row;
        row.insert(row.end(), target_row->begin(), target_row->end());
        row.emplace_back("resolved");
        output.rows.push_back(std::move(row));
      }
    }
    result.add_relation(std::move(output));
  }
  if (!unresolved.rows.empty()) {
    result.add_relation(std::move(unresolved));
    if (result.completeness == FactCompleteness::complete) {
      result.completeness = FactCompleteness::partial;
    }
  }
  result.validate();
  return result;
}

FactExportStats write_fact_files(const FactSnapshot &snapshot,
                                 const std::string &out_dir,
                                 std::string_view prelude) {
  snapshot.validate();
  std::error_code ec;
  fs::create_directories(out_dir, ec);
  if (ec) {
    throw CidxError("cannot create " + out_dir + ": " + ec.message());
  }
  FactExportStats stats;
  for (const auto &[name, relation] : snapshot.relations) {
    const std::string path = (fs::path(out_dir) / (name + ".facts")).string();
    std::ofstream out(path, std::ios::binary);
    if (!out) {
      throw CidxError("cannot write " + path);
    }
    for (const FactRow &row : relation.rows) {
      for (std::size_t index = 0; index < row.size(); ++index) {
        if (index > 0) {
          out << '\t';
        }
        out << fact_value_text(row[index]);
      }
      out << '\n';
      ++stats.rows;
    }
    ++stats.files;
  }
  const std::string prelude_path =
      (fs::path(out_dir) / "cidx_facts.dl").string();
  std::ofstream prelude_file(prelude_path, std::ios::binary);
  if (!prelude_file) {
    throw CidxError("cannot write " + prelude_path);
  }
  prelude_file << prelude;
  return stats;
}

} // namespace cidx::analysis
