#include "storage/fact_batch_writer.hpp"

#include "storage/sqlite.hpp"
#include "storage/storage.hpp"
#include "util/errors.hpp"
#include "util/json_min.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <map>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace cidx::storage {

namespace {

using Clock = std::chrono::steady_clock;

template <typename T>
void bind_optional(SqliteStmt &statement, int index,
                   const std::optional<T> &value) {
  if (value) {
    if constexpr (std::is_same_v<T, std::string>) {
      statement.bind(index, std::string_view(*value));
    } else {
      statement.bind(index, *value);
    }
  } else {
    statement.bind_null(index);
  }
}

auto seconds_since(Clock::time_point started) -> double {
  return std::chrono::duration<double>(Clock::now() - started).count();
}

void note_statement_stats(FactBatchWriterReport &report,
                          const SqliteStmt &statement) {
  report.virtual_machine_steps += statement.stats().virtual_machine_steps;
  report.virtual_machine_seconds += statement.stats().step_seconds;
}

auto prepare_statement(SqliteDb &database, FactBatchWriterReport &report,
                       std::string_view sql) -> SqliteStmt {
  const auto started = Clock::now();
  auto statement = database.prepare(sql);
  report.prepare_seconds += seconds_since(started);
  if (statement.reused_compiled_statement()) {
    ++report.statements_reused;
  } else {
    ++report.statements_prepared;
  }
  return statement;
}

void inject(FactBatchWriterFailurePoint requested,
            FactBatchWriterFailurePoint actual) {
  if (requested == actual) {
    throw StorageError("injected FactBatchWriter failure");
  }
}

auto execute(SqliteDb &database, FactBatchWriterReport &report,
             std::string_view sql) -> std::int64_t {
  auto statement = prepare_statement(database, report, sql);
  statement.step_done();
  note_statement_stats(report, statement);
  ++report.statement_executions;
  return database.changes();
}

auto universe_id_for(cidx::SqliteStorageService &storage, std::string_view key)
    -> std::int64_t {
  const auto universe = storage.get_semantic_universe_by_key(std::string(key));
  if (!universe) {
    throw StorageError("FactBatch references unknown semantic universe '" +
                       std::string(key) + "'");
  }
  return universe->id;
}

auto local_identity_key(cidx::SqliteStorageService &storage,
                        const ast::FactPartitionKey &partition,
                        const ast::SymbolRecord &record) -> std::string {
  std::string result = partition.configuration.semantic_universe + '\x1f';
  const bool local =
      record.linkage == "internal" || record.linkage == "no-linkage";
  if (local) {
    const std::string translation_unit =
        record.identity_translation_unit.value_or(
            partition.configuration.translation_unit);
    const std::string source = record.identity_source.value_or(record.file);
    result += "local:" + translation_unit + '\x1f' +
              storage.portable_source_identity_for_path(source) + '\x1f';
  }
  result += record.usr;
  return result;
}

struct SymbolLocation {
  ast::FactPartitionKey partition;
  std::size_t index = 0;
};

auto symbol_natural_key(const ast::FactPartitionKey &partition,
                        const ast::SymbolRecord &record) -> std::string {
  return "symbol:" + ast::SymbolNaturalKey{.partition = partition,
                                           .usr = record.usr,
                                           .local_anchor = record.local_anchor,
                                           .linkage = record.linkage}
                         .stable_string();
}

auto inverse(const std::map<std::int64_t, std::string> &values)
    -> std::map<std::string, std::int64_t> {
  std::map<std::string, std::int64_t> result;
  for (const auto &[handle, key] : values) {
    result.emplace(key, handle);
  }
  return result;
}

auto symbol_locations(const ast::FactBatch &batch)
    -> std::map<std::string, std::vector<SymbolLocation>> {
  std::map<std::string, std::vector<SymbolLocation>> result;
  for (const ast::FileFactPartition &partition : batch.partitions()) {
    const auto found = partition.members.find(ast::FactFamily::symbols);
    if (found == partition.members.end()) {
      continue;
    }
    for (const std::size_t index : found->second) {
      const ast::SymbolRecord &record = batch.records().symbols.at(index);
      const std::string key = symbol_natural_key(partition.key, record) + '\n' +
                              ast::stable_symbol_record_key(record);
      result[key].push_back({.partition = partition.key, .index = index});
    }
  }
  return result;
}

auto file_handle_for(const ast::FactBatch &batch,
                     const ast::FactPartitionKey &partition,
                     std::string_view record_family = {}) -> std::int64_t {
  for (const auto &[handle, candidate] : batch.file_keys()) {
    if (candidate == partition) {
      return handle;
    }
  }
  throw StorageError("FactBatch partition has no transient file handle: " +
                     partition.stable_string() +
                     (record_family.empty()
                          ? std::string{}
                          : " (" + std::string(record_family) + ")"));
}

auto partitions_for(const ast::FactBatch &batch, ast::FactFamily family,
                    std::size_t record_count)
    -> std::vector<const ast::FactPartitionKey *> {
  std::vector<const ast::FactPartitionKey *> result(record_count, nullptr);
  for (const ast::FileFactPartition &partition : batch.partitions()) {
    const auto found = partition.members.find(family);
    if (found == partition.members.end()) {
      continue;
    }
    for (const std::size_t index : found->second) {
      if (index >= result.size() || result[index] != nullptr) {
        throw StorageError("FactBatch family membership is invalid");
      }
      result[index] = &partition.key;
    }
  }
  return result;
}

struct AuxiliaryRow {
  ast::FactFamily family = ast::FactFamily::symbols;
  std::int64_t file_handle = 0;
  std::array<std::optional<std::int64_t>, 10> integers;
  std::array<std::optional<std::string>, 6> texts;
};

void create_temporary_schema(SqliteDb &database,
                             FactBatchWriterReport &report) {
  static constexpr std::string_view schema = R"SQL(
CREATE TEMP TABLE IF NOT EXISTS cidx_batch_file_map(
  batch INTEGER NOT NULL, handle INTEGER NOT NULL, file_id INTEGER NOT NULL,
  partition_key TEXT NOT NULL, planned INTEGER NOT NULL,
  generation INTEGER NOT NULL DEFAULT 1,
  PRIMARY KEY(batch, handle));
CREATE TEMP TABLE IF NOT EXISTS cidx_batch_symbol(
  batch INTEGER NOT NULL, ordinal INTEGER NOT NULL, handle INTEGER NOT NULL,
  file_handle INTEGER NOT NULL, universe_id INTEGER NOT NULL,
  identity_key TEXT NOT NULL, usr TEXT NOT NULL, spelling TEXT NOT NULL,
  qual_name TEXT, display_name TEXT, kind INTEGER NOT NULL, type_info TEXT,
  line INTEGER, col INTEGER, end_line INTEGER, end_col INTEGER,
  decl_line INTEGER, decl_col INTEGER, decl_path TEXT,
  is_definition INTEGER NOT NULL,
  is_pure INTEGER NOT NULL, is_static INTEGER NOT NULL,
  is_instantiation INTEGER NOT NULL, is_named_instance INTEGER NOT NULL,
  linkage TEXT, access TEXT,
  parent_usr TEXT, resolved INTEGER NOT NULL, const_value TEXT,
  callable_kind TEXT, template_origin TEXT, template_form TEXT,
  PRIMARY KEY(batch, ordinal));
CREATE TEMP TABLE IF NOT EXISTS cidx_batch_symbol_map(
  batch INTEGER NOT NULL, handle INTEGER NOT NULL, symbol_id INTEGER NOT NULL,
  PRIMARY KEY(batch, handle));
CREATE TEMP TABLE IF NOT EXISTS cidx_batch_type(
  batch INTEGER NOT NULL, ordinal INTEGER NOT NULL, handle INTEGER NOT NULL,
  type_key TEXT NOT NULL, spelling TEXT NOT NULL, kind INTEGER NOT NULL,
  is_const INTEGER NOT NULL, is_volatile INTEGER NOT NULL,
  is_restrict INTEGER NOT NULL, decl_usr TEXT, canonical_handle INTEGER,
  extent TEXT, PRIMARY KEY(batch, ordinal));
CREATE TEMP TABLE IF NOT EXISTS cidx_batch_type_map(
  batch INTEGER NOT NULL, handle INTEGER NOT NULL, type_id INTEGER NOT NULL,
  PRIMARY KEY(batch, handle));
CREATE TEMP TABLE IF NOT EXISTS cidx_batch_relation(
  batch INTEGER NOT NULL, ordinal INTEGER NOT NULL, handle INTEGER NOT NULL,
  file_handle INTEGER NOT NULL, src_handle INTEGER NOT NULL,
  dst_handle INTEGER NOT NULL, kind INTEGER NOT NULL, count INTEGER NOT NULL,
  base_access INTEGER, is_virtual INTEGER, PRIMARY KEY(batch, ordinal));
CREATE TEMP TABLE IF NOT EXISTS cidx_batch_relation_map(
  batch INTEGER NOT NULL, handle INTEGER NOT NULL, relation_id INTEGER NOT NULL,
  PRIMARY KEY(batch, handle));
CREATE TEMP TABLE IF NOT EXISTS cidx_batch_definition(
  batch INTEGER NOT NULL, ordinal INTEGER NOT NULL, handle INTEGER NOT NULL,
  symbol_handle INTEGER NOT NULL, file_handle INTEGER NOT NULL,
  line INTEGER, col INTEGER, end_line INTEGER, end_col INTEGER, init_text TEXT,
  PRIMARY KEY(batch, ordinal));
CREATE TEMP TABLE IF NOT EXISTS cidx_batch_definition_map(
  batch INTEGER NOT NULL, handle INTEGER NOT NULL,
  definition_id INTEGER NOT NULL, PRIMARY KEY(batch, handle));
CREATE TEMP TABLE IF NOT EXISTS cidx_batch_edge_site(
  batch INTEGER NOT NULL, ordinal INTEGER NOT NULL, edge_handle INTEGER NOT NULL,
  file_handle INTEGER NOT NULL, line INTEGER, col INTEGER,
  conditional INTEGER NOT NULL, recv_src_kind TEXT, recv_type_usr TEXT,
  recv_decl_usr TEXT, recv_param_pos INTEGER, recv_type_is_value INTEGER,
  PRIMARY KEY(batch, ordinal));
CREATE TEMP TABLE IF NOT EXISTS cidx_batch_call_arg(
  batch INTEGER NOT NULL, ordinal INTEGER NOT NULL, edge_handle INTEGER NOT NULL,
  file_handle INTEGER NOT NULL, line INTEGER NOT NULL, col INTEGER NOT NULL,
  position INTEGER NOT NULL, src_kind TEXT NOT NULL, type_usr TEXT,
  decl_usr TEXT, callee_usr TEXT, type_is_value INTEGER,
  PRIMARY KEY(batch, ordinal));
CREATE TEMP TABLE IF NOT EXISTS cidx_batch_diagnostic(
  batch INTEGER NOT NULL, ordinal INTEGER NOT NULL, file_handle INTEGER NOT NULL,
  severity INTEGER NOT NULL, spelling TEXT NOT NULL, file_path TEXT,
  line INTEGER, col INTEGER, PRIMARY KEY(batch, ordinal));
CREATE TEMP TABLE IF NOT EXISTS cidx_batch_aux(
  batch INTEGER NOT NULL, family INTEGER NOT NULL, ordinal INTEGER NOT NULL,
  file_handle INTEGER NOT NULL,
  i0 INTEGER,i1 INTEGER,i2 INTEGER,i3 INTEGER,i4 INTEGER,
  i5 INTEGER,i6 INTEGER,i7 INTEGER,i8 INTEGER,i9 INTEGER,
  t0 TEXT,t1 TEXT,t2 TEXT,t3 TEXT,t4 TEXT,t5 TEXT,
  PRIMARY KEY(batch,family,ordinal));
)SQL";
  database.exec(schema);
  report.statements_prepared += 13;
  report.statement_executions += 13;
}

void clear_temporary_rows(SqliteDb &database, FactBatchWriterReport &report,
                          std::int64_t batch_token) {
  for (const std::string_view table :
       {"cidx_batch_file_map", "cidx_batch_symbol", "cidx_batch_symbol_map",
        "cidx_batch_type", "cidx_batch_type_map", "cidx_batch_relation",
        "cidx_batch_relation_map", "cidx_batch_definition",
        "cidx_batch_definition_map", "cidx_batch_edge_site",
        "cidx_batch_call_arg", "cidx_batch_diagnostic", "cidx_batch_aux"}) {
    auto statement = prepare_statement(
        database, report,
        "DELETE FROM temp." + std::string(table) + " WHERE batch = ?");
    statement.bind(1, batch_token);
    statement.step_done();
    note_statement_stats(report, statement);
    ++report.statement_executions;
  }
}

auto batch_token(const ast::FactBatch &batch,
                 const FactBatchPublicationContext &context) -> std::int64_t {
  std::string material = context.route_plan.token();
  material += '\x1f';
  material += batch.producer();
  material += '\x1f';
  material += std::to_string(batch.producer_version());
  return static_cast<std::int64_t>(ast::stable_fact_hash(material) &
                                   0x7fff'ffff'ffff'ffffULL);
}

void load_file_map(SqliteDb &database, FactBatchWriterResult &result,
                   const ast::FactBatch &batch,
                   const FactBatchPublicationContext &context,
                   std::int64_t token) {
  auto insert = prepare_statement(
      database, result.report,
      "INSERT INTO temp.cidx_batch_file_map"
      "(batch,handle,file_id,partition_key,planned) VALUES (?,?,?,?,?)");
  std::uint64_t executions = 0;
  for (const auto &[handle, partition] : batch.file_keys()) {
    const auto file = result.file_ids.find(handle);
    if (file == result.file_ids.end()) {
      throw StorageError("publication plan omitted a FactBatch file handle");
    }
    insert.bind(1, token);
    insert.bind(2, handle);
    insert.bind(3, file->second);
    const std::string partition_key = partition.stable_string();
    insert.bind(4, std::string_view(partition_key));
    const bool planned = std::ranges::any_of(
        context.route_plan.routes(),
        [&partition](const ast::PlannedFileRoute &route) {
          return route.extraction.partition.file == partition.file;
        });
    insert.bind(5, static_cast<std::int64_t>(planned));
    insert.step_done();
    ++executions;
    if (executions != batch.file_keys().size()) {
      insert.reset();
    }
  }
  result.report.statement_executions += executions;
  result.report.statements_reused += executions > 0 ? executions - 1 : 0;
  result.report.statements_eliminated += executions > 0 ? executions - 1 : 0;
  note_statement_stats(result.report, insert);
}

void load_symbols(cidx::SqliteStorageService &storage, SqliteDb &database,
                  FactBatchWriterResult &result, const ast::FactBatch &batch,
                  std::int64_t token) {
  auto insert = prepare_statement(
      database, result.report,
      "INSERT INTO temp.cidx_batch_symbol("
      "batch,ordinal,handle,file_handle,universe_id,identity_key,usr,spelling,"
      "qual_name,display_name,kind,type_info,line,col,end_line,end_col,decl_"
      "line,"
      "decl_col,decl_path,is_definition,is_pure,is_static,is_instantiation,"
      "is_named_instance,linkage,"
      "access,"
      "parent_usr,resolved,const_value,callable_kind,template_origin,template_"
      "form)"
      " VALUES "
      "(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  const auto handles = inverse(batch.symbol_keys());
  auto locations = symbol_locations(batch);
  std::map<std::string, std::size_t> next_location;
  std::uint64_t ordinal = 0;
  for (const ast::SymbolEmissionMetadata &metadata :
       batch.records().symbol_order) {
    const std::string natural = "symbol:" + metadata.symbol.stable_string();
    const std::string location_key = natural + '\n' + metadata.record_key;
    const auto found = locations.find(location_key);
    if (found == locations.end() || found->second.empty()) {
      throw StorageError("symbol apply-order metadata has no staged record");
    }
    std::size_t &cursor = next_location[location_key];
    const SymbolLocation &location =
        found->second[std::min(cursor, found->second.size() - 1)];
    ++cursor;
    const ast::SymbolRecord &record =
        batch.records().symbols.at(location.index);
    const std::int64_t handle = handles.at(natural);
    insert.bind(1, token);
    insert.bind(2, static_cast<std::int64_t>(ordinal));
    insert.bind(3, handle);
    insert.bind(4, file_handle_for(batch, location.partition, "symbols"));
    insert.bind(
        5, universe_id_for(storage,
                           location.partition.configuration.semantic_universe));
    const std::string identity_key =
        local_identity_key(storage, location.partition, record);
    insert.bind(6, std::string_view(identity_key));
    insert.bind(7, std::string_view(record.usr));
    insert.bind(8, std::string_view(record.spelling));
    bind_optional(insert, 9, record.qual_name);
    bind_optional(insert, 10, record.display_name);
    insert.bind(11, static_cast<std::int64_t>(record.kind));
    bind_optional(insert, 12, record.type_info);
    insert.bind(13, record.line);
    insert.bind(14, record.col);
    insert.bind(15, record.end_line);
    insert.bind(16, record.end_col);
    bind_optional(insert, 17, record.decl_line);
    bind_optional(insert, 18, record.decl_col);
    bind_optional(insert, 19, record.decl_path);
    insert.bind(20, static_cast<std::int64_t>(record.is_definition));
    insert.bind(21, static_cast<std::int64_t>(record.is_pure));
    insert.bind(22, static_cast<std::int64_t>(record.is_static));
    insert.bind(23, static_cast<std::int64_t>(record.is_instantiation));
    insert.bind(24, static_cast<std::int64_t>(record.is_named_instance));
    if (record.line > 0) {
      bind_optional(insert, 25, record.linkage);
    } else {
      insert.bind_null(25);
    }
    bind_optional(insert, 26, record.access);
    bind_optional(insert, 27, record.parent_usr);
    insert.bind(28, static_cast<std::int64_t>(record.resolved));
    bind_optional(insert, 29, record.const_value);
    bind_optional(insert, 30, record.callable_kind);
    bind_optional(insert, 31, record.template_origin);
    bind_optional(insert, 32, record.template_form);
    insert.step_done();
    ++ordinal;
    if (ordinal != batch.records().symbol_order.size()) {
      insert.reset();
    }
  }
  auto &rows = result.report.families[ast::FactFamily::symbols];
  rows.staged += ordinal;
  result.report.statement_executions += ordinal;
  result.report.statements_reused += ordinal > 0 ? ordinal - 1 : 0;
  result.report.statements_eliminated += ordinal > 0 ? ordinal - 1 : 0;
  note_statement_stats(result.report, insert);
}

void load_types(SqliteDb &database, FactBatchWriterResult &result,
                const ast::FactBatch &batch, std::int64_t token) {
  auto insert = prepare_statement(
      database, result.report,
      "INSERT INTO temp.cidx_batch_type("
      "batch,ordinal,handle,type_key,spelling,kind,is_const,is_volatile,"
      "is_restrict,decl_usr,canonical_handle,extent) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
  const auto handles = inverse(batch.type_keys());
  const auto partitions = partitions_for(batch, ast::FactFamily::types,
                                         batch.records().type_nodes.size());
  std::uint64_t ordinal = 0;
  for (std::size_t index = 0; index < batch.records().type_nodes.size();
       ++index) {
    const ast::FactPartitionKey *partition = partitions.at(index);
    if (partition == nullptr) {
      throw StorageError("type record has no FactBatch partition");
    }
    const ast::TypeNodeRecord &record = batch.records().type_nodes.at(index);
    const std::string key =
        "type:" + ast::TypeNaturalKey{.partition = *partition,
                                      .type_key = record.type_key}
                      .stable_string();
    insert.bind(1, token);
    insert.bind(2, static_cast<std::int64_t>(ordinal));
    insert.bind(3, handles.at(key));
    insert.bind(4, std::string_view(record.type_key));
    insert.bind(5, std::string_view(record.spelling));
    insert.bind(6, record.kind);
    insert.bind(7, static_cast<std::int64_t>(record.is_const));
    insert.bind(8, static_cast<std::int64_t>(record.is_volatile));
    insert.bind(9, static_cast<std::int64_t>(record.is_restrict));
    bind_optional(insert, 10, record.decl_usr);
    bind_optional(insert, 11, record.canonical_id);
    bind_optional(insert, 12, record.extent);
    insert.step_done();
    ++ordinal;
    if (ordinal != batch.records().type_nodes.size()) {
      insert.reset();
    }
  }
  result.report.families[ast::FactFamily::types].staged += ordinal;
  result.report.statement_executions += ordinal;
  result.report.statements_reused += ordinal > 0 ? ordinal - 1 : 0;
  result.report.statements_eliminated += ordinal > 0 ? ordinal - 1 : 0;
  note_statement_stats(result.report, insert);
}

auto relation_key(const ast::FactPartitionKey &partition,
                  const ast::EdgeRecord &record) -> std::string {
  const auto optional_text = [](const std::optional<std::int64_t> &value) {
    return value ? std::to_string(*value) : std::string("-");
  };
  return "edge:" + partition.stable_string() + std::to_string(record.src_id) +
         ':' + std::to_string(record.dst_id) + ':' +
         std::to_string(record.kind) + ':' + optional_text(record.base_access) +
         ':' + optional_text(record.is_virtual);
}

void load_relations(SqliteDb &database, FactBatchWriterResult &result,
                    const ast::FactBatch &batch, std::int64_t token) {
  auto insert = prepare_statement(
      database, result.report,
      "INSERT INTO temp.cidx_batch_relation("
      "batch,ordinal,handle,file_handle,src_handle,dst_handle,kind,count,"
      "base_access,is_virtual) VALUES(?,?,?,?,?,?,?,?,?,?)");
  const auto handles = inverse(batch.relation_keys());
  const auto partitions = partitions_for(batch, ast::FactFamily::relations,
                                         batch.records().relations.size());
  std::uint64_t ordinal = 0;
  for (std::size_t index = 0; index < batch.records().relations.size();
       ++index) {
    const ast::FactPartitionKey *partition = partitions.at(index);
    if (partition == nullptr) {
      throw StorageError("relation record has no FactBatch partition");
    }
    const ast::EdgeRecord &record = batch.records().relations.at(index);
    if (!batch.symbol_keys().contains(record.src_id) ||
        !batch.symbol_keys().contains(record.dst_id)) {
      throw StorageError("relation endpoint has no batch symbol natural key: " +
                         std::to_string(record.src_id) + " -> " +
                         std::to_string(record.dst_id));
    }
    insert.bind(1, token);
    insert.bind(2, static_cast<std::int64_t>(ordinal));
    insert.bind(3, handles.at(relation_key(*partition, record)));
    insert.bind(4, file_handle_for(batch, *partition, "relations"));
    insert.bind(5, record.src_id);
    insert.bind(6, record.dst_id);
    insert.bind(7, record.kind);
    insert.bind(8, record.count);
    bind_optional(insert, 9, record.base_access);
    bind_optional(insert, 10, record.is_virtual);
    insert.step_done();
    ++ordinal;
    if (ordinal != batch.records().relations.size()) {
      insert.reset();
    }
  }
  result.report.families[ast::FactFamily::relations].staged += ordinal;
  result.report.statement_executions += ordinal;
  result.report.statements_reused += ordinal > 0 ? ordinal - 1 : 0;
  result.report.statements_eliminated += ordinal > 0 ? ordinal - 1 : 0;
  note_statement_stats(result.report, insert);
}

void load_definitions(SqliteDb &database, FactBatchWriterResult &result,
                      const ast::FactBatch &batch, std::int64_t token) {
  auto insert = prepare_statement(database, result.report,
                                  "INSERT INTO temp.cidx_batch_definition("
                                  "batch,ordinal,handle,symbol_handle,file_"
                                  "handle,line,col,end_line,end_col,init_text)"
                                  " VALUES(?,?,?,?,?,?,?,?,?,?)");
  const auto partitions = partitions_for(batch, ast::FactFamily::definitions,
                                         batch.records().definitions.size());
  std::uint64_t ordinal = 0;
  for (std::size_t index = 0; index < batch.records().definitions.size();
       ++index) {
    const ast::DefinitionFactRecord &record =
        batch.records().definitions.at(index);
    const ast::FactPartitionKey *partition = partitions.at(index);
    if (partition == nullptr) {
      throw StorageError("definition record has no FactBatch partition");
    }
    insert.bind(1, token);
    insert.bind(2, static_cast<std::int64_t>(ordinal));
    insert.bind(3, record.id);
    insert.bind(4, record.symbol_id);
    insert.bind(5, file_handle_for(batch, *partition, "definitions"));
    insert.bind(6, record.line);
    insert.bind(7, record.col);
    insert.bind(8, record.end_line);
    insert.bind(9, record.end_col);
    bind_optional(insert, 10, record.init_text);
    insert.step_done();
    ++ordinal;
    if (ordinal != batch.records().definitions.size()) {
      insert.reset();
    }
  }
  result.report.families[ast::FactFamily::definitions].staged += ordinal;
  result.report.statement_executions += ordinal;
  result.report.statements_reused += ordinal > 0 ? ordinal - 1 : 0;
  result.report.statements_eliminated += ordinal > 0 ? ordinal - 1 : 0;
  note_statement_stats(result.report, insert);
}

void load_auxiliary_rows(SqliteDb &database, FactBatchWriterResult &result,
                         const std::vector<AuxiliaryRow> &rows,
                         std::int64_t token) {
  auto insert = prepare_statement(
      database, result.report,
      "INSERT INTO temp.cidx_batch_aux(batch,family,ordinal,file_handle,"
      "i0,i1,i2,i3,i4,i5,i6,i7,i8,i9,t0,t1,t2,t3,t4,t5)"
      " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  for (std::size_t ordinal = 0; ordinal < rows.size(); ++ordinal) {
    const AuxiliaryRow &row = rows[ordinal];
    insert.bind(1, token);
    insert.bind(2, static_cast<std::int64_t>(std::to_underlying(row.family)));
    insert.bind(3, static_cast<std::int64_t>(ordinal));
    insert.bind(4, row.file_handle);
    for (std::size_t index = 0; index < row.integers.size(); ++index) {
      bind_optional(insert, static_cast<int>(5 + index), row.integers[index]);
    }
    for (std::size_t index = 0; index < row.texts.size(); ++index) {
      bind_optional(insert, static_cast<int>(15 + index), row.texts[index]);
    }
    insert.step_done();
    if (ordinal + 1 != rows.size()) {
      insert.reset();
    }
    ++result.report.families[row.family].staged;
  }
  result.report.statement_executions += rows.size();
  result.report.statements_reused += rows.empty() ? 0 : rows.size() - 1;
  result.report.statements_eliminated += rows.empty() ? 0 : rows.size() - 1;
  note_statement_stats(result.report, insert);
}

auto auxiliary_rows(const ast::FactBatch &batch) -> std::vector<AuxiliaryRow> {
  std::vector<AuxiliaryRow> rows;
  const auto symbol_handles = inverse(batch.symbol_keys());
  const auto add_partitioned = [&batch, &rows](
                                   ast::FactFamily family, std::size_t count,
                                   const auto &records, auto transform) {
    const auto partitions = partitions_for(batch, family, count);
    for (std::size_t index = 0; index < count; ++index) {
      const ast::FactPartitionKey *partition = partitions.at(index);
      if (partition == nullptr) {
        throw StorageError("auxiliary record has no FactBatch partition");
      }
      AuxiliaryRow row = transform(records.at(index));
      row.family = family;
      try {
        row.file_handle = file_handle_for(batch, *partition);
      } catch (const StorageError &error) {
        throw StorageError(std::string(error.what()) + " (family " +
                           std::to_string(std::to_underlying(family)) + ")");
      }
      if (family == ast::FactFamily::edge_sites ||
          family == ast::FactFamily::call_arguments) {
        row.integers[1] = row.file_handle;
      }
      rows.push_back(std::move(row));
    }
  };
  add_partitioned(
      ast::FactFamily::declaration_sites,
      batch.records().declaration_sites.size(),
      batch.records().declaration_sites,
      [&symbol_handles](const ast::DeclarationSiteRecord &record) {
        AuxiliaryRow row;
        row.integers = {
            symbol_handles.at("symbol:" + record.symbol.stable_string()),
            record.line,
            record.col,
            record.end_line,
            record.end_col,
            static_cast<std::int64_t>(record.is_definition)};
        return row;
      });
  add_partitioned(ast::FactFamily::edge_sites,
                  batch.records().edge_sites.size(), batch.records().edge_sites,
                  [](const ast::EdgeSiteRecord &record) {
                    AuxiliaryRow row;
                    row.integers = {record.edge_id,
                                    record.file_id,
                                    record.line,
                                    record.col,
                                    record.conditional,
                                    record.recv_param_pos,
                                    record.recv_type_is_value};
                    row.texts = {record.recv_src_kind, record.recv_type_usr,
                                 record.recv_decl_usr};
                    return row;
                  });
  add_partitioned(
      ast::FactFamily::call_arguments, batch.records().call_args.size(),
      batch.records().call_args, [](const ast::CallArgRecord &record) {
        AuxiliaryRow row;
        row.integers = {record.edge_id, record.file_id,  record.line,
                        record.col,     record.position, record.type_is_value};
        row.texts = {record.src_kind, record.type_usr, record.decl_usr,
                     record.callee_usr};
        return row;
      });
  add_partitioned(
      ast::FactFamily::template_parameters,
      batch.records().template_params.size(), batch.records().template_params,
      [](const ast::TemplateParamRecord &record) {
        AuxiliaryRow row;
        row.integers = {record.owner_id,        record.position,
                        record.param_kind,      record.type_id,
                        record.default_type_id, record.default_ref_id};
        row.texts = {record.name, record.default_txt};
        return row;
      });
  add_partitioned(
      ast::FactFamily::template_arguments, batch.records().template_args.size(),
      batch.records().template_args, [](const ast::TemplateArgRecord &record) {
        AuxiliaryRow row;
        row.integers = {record.owner_id, record.position, record.pack_index,
                        record.arg_kind, record.ref_id,   record.type_id};
        row.texts[0] = record.literal;
        return row;
      });
  add_partitioned(ast::FactFamily::type_edges,
                  batch.records().type_edges.size(), batch.records().type_edges,
                  [](const ast::TypeEdgeRecord &record) {
                    AuxiliaryRow row;
                    row.integers = {record.src_id, record.kind, record.position,
                                    record.dst_id};
                    return row;
                  });
  add_partitioned(
      ast::FactFamily::symbol_types, batch.records().symbol_types.size(),
      batch.records().symbol_types, [](const ast::SymbolTypeRecord &record) {
        AuxiliaryRow row;
        row.integers = {record.symbol_id, record.kind, record.type_id};
        return row;
      });
  add_partitioned(
      ast::FactFamily::parameters, batch.records().parameters.size(),
      batch.records().parameters, [](const ast::ParameterFactRecord &record) {
        AuxiliaryRow row;
        row.integers = {record.owner_id,
                        record.parameter.position,
                        record.parameter.pack_index,
                        record.parameter.type_id,
                        record.parameter.declared_type_id,
                        record.parameter.adjusted_type_id,
                        record.parameter.file_id,
                        record.parameter.line,
                        record.parameter.col};
        row.texts = {record.parameter.name, record.parameter.default_text,
                     record.parameter.default_origin,
                     record.parameter.reference_semantics};
        return row;
      });
  add_partitioned(ast::FactFamily::definition_edges,
                  batch.records().definition_edges.size(),
                  batch.records().definition_edges,
                  [](const ast::DefinitionEdgeRecord &record) {
                    AuxiliaryRow row;
                    row.integers = {record.definition_id, record.destination_id,
                                    record.kind};
                    return row;
                  });
  add_partitioned(
      ast::FactFamily::diagnostics, batch.records().diagnostics.size(),
      batch.records().diagnostics, [](const ast::DiagnosticFactRecord &record) {
        AuxiliaryRow row;
        row.integers = {static_cast<std::int64_t>(record.severity) + 1,
                        record.line, record.col};
        row.texts[0] = record.spelling;
        if (record.location_file) {
          row.texts[1] = record.location_file->portable_path();
        }
        return row;
      });
  add_partitioned(ast::FactFamily::includes, batch.records().includes.size(),
                  batch.records().includes,
                  [&batch](const ast::IncludeDirectiveRecord &record) {
                    AuxiliaryRow row;
                    row.integers = {record.line,
                                    record.col,
                                    record.begin_offset,
                                    record.end_offset,
                                    static_cast<std::int64_t>(record.directive),
                                    static_cast<std::int64_t>(record.is_angled),
                                    static_cast<std::int64_t>(record.resolved),
                                    static_cast<std::int64_t>(record.is_system),
                                    static_cast<std::int64_t>(record.guarded)};
                    row.texts = {record.destination_path, record.spelling,
                                 record.conditional_fingerprint};
                    if (record.destination) {
                      const auto destination = std::ranges::find_if(
                          batch.file_keys(), [&record](const auto &candidate) {
                            return candidate.second.file.portable_path() ==
                                   record.destination->portable_path();
                          });
                      if (destination != batch.file_keys().end()) {
                        row.integers[9] = destination->first;
                      }
                      row.texts[3] = record.destination->component_path;
                      row.texts[4] = record.destination->directory_path;
                      row.texts[5] = record.destination->file_name;
                    }
                    return row;
                  });
  add_partitioned(ast::FactFamily::macros, batch.records().macros.size(),
                  batch.records().macros,
                  [](const ast::MacroUseRecord &record) {
                    AuxiliaryRow row;
                    row.integers[0] = record.count;
                    row.texts = {record.definition_path, record.name};
                    return row;
                  });
  add_partitioned(
      ast::FactFamily::evidence, batch.records().evidence.size(),
      batch.records().evidence, [](const ast::EvidenceRecord &record) {
        AuxiliaryRow row;
        row.integers = {record.line, record.col,
                        static_cast<std::int64_t>(record.completeness),
                        static_cast<std::int64_t>(record.trust)};
        row.texts = {record.producer, record.construct, record.file,
                     record.detail};
        return row;
      });
  add_partitioned(ast::FactFamily::presentation_intents,
                  batch.records().presentation_intents.size(),
                  batch.records().presentation_intents,
                  [](const ast::PresentationIntent &record) {
                    AuxiliaryRow row;
                    row.integers[0] = record.symbol_id;
                    row.texts[0] =
                        json_min::encode_string_array(record.display_args);
                    return row;
                  });
  add_partitioned(ast::FactFamily::lifecycle_cleanup,
                  batch.records().lifecycle_cleanup.size(),
                  batch.records().lifecycle_cleanup,
                  [](const ast::LifecycleCleanupIntent &record) {
                    AuxiliaryRow row;
                    row.integers[0] = static_cast<std::int64_t>(record.kind);
                    row.texts = {record.target.portable_path(),
                                 record.prior_generation.token};
                    return row;
                  });
  add_partitioned(ast::FactFamily::applicability,
                  batch.records().applicability.size(),
                  batch.records().applicability,
                  [](const ast::ApplicabilityOwnershipRecord &record) {
                    AuxiliaryRow row;
                    row.integers = {static_cast<std::int64_t>(record.role),
                                    static_cast<std::int64_t>(record.state)};
                    row.texts = {record.reason, record.generation.token};
                    return row;
                  });
  return rows;
}

void apply_symbols(SqliteDb &database, FactBatchWriterResult &result,
                   std::int64_t token) {
  auto existing = prepare_statement(
      database, result.report,
      "SELECT COUNT(*),COALESCE(SUM(EXISTS(SELECT 1 FROM symbol s WHERE "
      "s.semantic_universe_id=b.universe_id AND "
      "s.identity_key=b.identity_key AND s.identity_key<>'')),0) FROM "
      "(SELECT DISTINCT "
      "universe_id,identity_key FROM temp.cidx_batch_symbol WHERE batch=?) b");
  existing.bind(1, token);
  const bool counted = existing.step();
  const std::uint64_t unique =
      counted ? static_cast<std::uint64_t>(existing.col_int64(0)) : 0;
  const std::uint64_t updates =
      counted ? static_cast<std::uint64_t>(existing.col_int64(1)) : 0;
  existing.step_done();
  note_statement_stats(result.report, existing);
  ++result.report.statement_executions;

  auto upsert = prepare_statement(
      database, result.report,
      "INSERT INTO symbol(usr,spelling,qual_name,display_name,kind,type_info,"
      "file_id,line,col,decl_file_id,decl_line,decl_col,is_definition,is_pure,"
      "is_static,is_instantiation,is_named_instance,linkage,access,parent_usr,"
      "resolved,decl_path,end_line,"
      "end_col,const_value,semantic_universe_id,identity_key,callable_kind,"
      "template_origin,template_form) "
      "SELECT b.usr,b.spelling,b.qual_name,b.display_name,b.kind,b.type_info,"
      "CASE WHEN b.line<=0 THEN NULL ELSE fm.file_id END,"
      "CASE WHEN b.line<=0 THEN NULL ELSE b.line END,"
      "CASE WHEN b.line<=0 THEN NULL ELSE b.col END,"
      "CASE WHEN b.decl_line IS NULL OR b.decl_path IS NOT NULL THEN NULL ELSE "
      "fm.file_id END,"
      "b.decl_line,b.decl_col,b.is_definition,b.is_pure,b.is_static,"
      "b.is_instantiation,b.is_named_instance,b.linkage,b.access,b.parent_usr,"
      "b.resolved,b.decl_path,"
      "CASE WHEN b.line<=0 THEN NULL ELSE b.end_line END,"
      "CASE WHEN b.line<=0 THEN NULL ELSE b.end_col END,"
      "b.const_value,b.universe_id,b.identity_key,b.callable_kind,"
      "b.template_origin,b.template_form FROM temp.cidx_batch_symbol b "
      "JOIN temp.cidx_batch_file_map fm ON fm.batch=b.batch AND "
      "fm.handle=b.file_handle "
      "AND fm.planned=1 "
      "WHERE b.batch=? ORDER BY b.ordinal "
      "ON CONFLICT(semantic_universe_id,identity_key) WHERE identity_key<>'' "
      "DO UPDATE SET "
      "spelling=excluded.spelling,qual_name=COALESCE(excluded.qual_name,symbol."
      "qual_name),"
      "display_name=COALESCE(excluded.display_name,symbol.display_name),kind="
      "excluded.kind,"
      "type_info=COALESCE(excluded.type_info,symbol.type_info),"
      "file_id=CASE WHEN excluded.file_id IS NOT NULL AND "
      "excluded.is_definition>=symbol.is_definition THEN excluded.file_id "
      "ELSE symbol.file_id END,"
      "line=CASE WHEN excluded.file_id IS NOT NULL AND "
      "excluded.is_definition>=symbol.is_definition THEN excluded.line ELSE "
      "symbol.line END,"
      "col=CASE WHEN excluded.file_id IS NOT NULL AND "
      "excluded.is_definition>=symbol.is_definition THEN excluded.col ELSE "
      "symbol.col END,"
      "end_line=CASE WHEN excluded.file_id IS NOT NULL AND "
      "excluded.is_definition>=symbol.is_definition THEN excluded.end_line "
      "ELSE symbol.end_line END,"
      "end_col=CASE WHEN excluded.file_id IS NOT NULL AND "
      "excluded.is_definition>=symbol.is_definition THEN excluded.end_col "
      "ELSE symbol.end_col END,"
      "decl_file_id=CASE WHEN excluded.file_id IS NULL THEN "
      "COALESCE(symbol.decl_file_id,excluded.decl_file_id) ELSE "
      "COALESCE(excluded.decl_file_id,symbol.decl_file_id) END,"
      "decl_line=CASE WHEN excluded.file_id IS NULL THEN "
      "COALESCE(symbol.decl_line,excluded.decl_line) ELSE "
      "COALESCE(excluded.decl_line,symbol.decl_line) END,"
      "decl_col=CASE WHEN excluded.file_id IS NULL THEN "
      "COALESCE(symbol.decl_col,excluded.decl_col) ELSE "
      "COALESCE(excluded.decl_col,symbol.decl_col) END,"
      "decl_path=COALESCE(symbol.decl_path,excluded.decl_path),"
      "is_definition=MAX(excluded.is_definition,symbol.is_definition),"
      "is_pure=MAX(excluded.is_pure,symbol.is_pure),"
      "is_static=MAX(excluded.is_static,symbol.is_static),"
      "is_instantiation=excluded.is_instantiation,"
      "is_named_instance=MAX(symbol.is_named_instance,excluded.is_named_"
      "instance),"
      "linkage=COALESCE(excluded.linkage,symbol.linkage),"
      "access=COALESCE(excluded.access,symbol.access),"
      "parent_usr=COALESCE(excluded.parent_usr,symbol.parent_usr),"
      "resolved=MAX(excluded.resolved,symbol.resolved),"
      "const_value=COALESCE(excluded.const_value,symbol.const_value),"
      "callable_kind=COALESCE(excluded.callable_kind,symbol.callable_kind),"
      "template_origin=COALESCE(excluded.template_origin,symbol.template_"
      "origin),"
      "template_form=COALESCE(excluded.template_form,symbol.template_form)");
  upsert.bind(1, token);
  upsert.step_done();
  note_statement_stats(result.report, upsert);
  ++result.report.statement_executions;

  auto map = prepare_statement(
      database, result.report,
      "INSERT INTO temp.cidx_batch_symbol_map(batch,handle,symbol_id) "
      "SELECT ?,b.handle,s.id FROM temp.cidx_batch_symbol b JOIN symbol s "
      "ON s.semantic_universe_id=b.universe_id AND "
      "s.identity_key=b.identity_key AND s.identity_key<>'' "
      "WHERE b.batch=? GROUP BY b.handle");
  map.bind(1, token);
  map.bind(2, token);
  map.step_done();
  note_statement_stats(result.report, map);
  ++result.report.statement_executions;

  auto sites = prepare_statement(
      database, result.report,
      "INSERT OR IGNORE INTO "
      "decl_site(symbol_id,file_id,line,col,end_line,end_col,is_definition) "
      "SELECT "
      "sm.symbol_id,fm.file_id,b.line,b.col,b.end_line,b.end_col,b.is_"
      "definition "
      "FROM temp.cidx_batch_symbol b "
      "JOIN temp.cidx_batch_symbol_map sm ON sm.batch=b.batch AND "
      "sm.handle=b.handle "
      "JOIN temp.cidx_batch_file_map fm ON fm.batch=b.batch AND "
      "fm.handle=b.file_handle "
      "WHERE b.batch=? AND b.line>0");
  sites.bind(1, token);
  sites.step_done();
  note_statement_stats(result.report, sites);
  ++result.report.statement_executions;

  const std::string token_text = std::to_string(token);
  auto needs_declaration_repair = prepare_statement(
      database, result.report,
      "SELECT EXISTS(SELECT 1 FROM temp.cidx_batch_symbol b JOIN "
      "temp.cidx_batch_symbol_map sm ON sm.batch=b.batch AND "
      "sm.handle=b.handle JOIN symbol s ON s.id=sm.symbol_id WHERE "
      "b.batch=? AND b.decl_line IS NOT NULL AND (s.decl_line IS NULL OR "
      "s.decl_col IS NULL OR (b.decl_path IS NULL AND "
      "s.decl_file_id IS NULL)) LIMIT 1)");
  needs_declaration_repair.bind(1, token);
  const bool repair_declarations = needs_declaration_repair.step() &&
                                   needs_declaration_repair.col_int64(0) != 0;
  needs_declaration_repair.step_done();
  note_statement_stats(result.report, needs_declaration_repair);
  ++result.report.statement_executions;
  if (repair_declarations) {
    static_cast<void>(execute(
        database, result.report,
        "WITH ranked AS (SELECT sm.symbol_id,fm.file_id,b.decl_line,b.decl_col,"
        "b.decl_path,ROW_NUMBER() OVER(PARTITION BY sm.symbol_id ORDER BY CASE "
        "WHEN b.line>0 THEN 0 ELSE 1 END,b.ordinal) AS line_rank,ROW_NUMBER() "
        "OVER(PARTITION BY sm.symbol_id ORDER BY CASE WHEN b.decl_path IS NULL "
        "THEN 0 ELSE 1 END,CASE WHEN b.line>0 THEN 0 ELSE 1 END,b.ordinal) AS "
        "file_rank FROM temp.cidx_batch_symbol b JOIN "
        "temp.cidx_batch_symbol_map sm ON sm.batch=b.batch AND "
        "sm.handle=b.handle JOIN temp.cidx_batch_file_map fm ON "
        "fm.batch=b.batch AND fm.handle=b.file_handle WHERE b.batch=" +
            token_text +
            " AND b.decl_line IS NOT NULL), selected AS (SELECT symbol_id,"
            "MAX(CASE WHEN line_rank=1 THEN decl_line END) AS decl_line,"
            "MAX(CASE WHEN line_rank=1 THEN decl_col END) AS decl_col,"
            "MAX(CASE WHEN file_rank=1 AND decl_path IS NULL THEN file_id END) "
            "AS file_id FROM ranked GROUP BY symbol_id) UPDATE symbol AS s SET "
            "decl_file_id=COALESCE(s.decl_file_id,selected.file_id),"
            "decl_line=COALESCE(s.decl_line,selected.decl_line),"
            "decl_col=COALESCE(s.decl_col,selected.decl_col) FROM selected "
            "WHERE s.id=selected.symbol_id"));
  }

  static_cast<void>(execute(
      database, result.report,
      "UPDATE symbol AS child SET parent_id=(SELECT MIN(parent.id) FROM symbol "
      "parent "
      "WHERE parent.semantic_universe_id=child.semantic_universe_id "
      "AND parent.usr=child.parent_usr) WHERE child.parent_usr IS NOT NULL "
      "AND child.id IN (SELECT symbol_id FROM temp.cidx_batch_symbol_map "
      "WHERE batch=" +
          token_text + ")"));

  auto &rows = result.report.families[ast::FactFamily::symbols];
  rows.updated += updates;
  rows.inserted += unique >= updates ? unique - updates : 0;
  rows.ignored += rows.staged >= unique ? rows.staged - unique : 0;
}

void apply_lifecycle_cleanup(SqliteDb &database, FactBatchWriterResult &result,
                             std::int64_t token) {
  const std::string cleanup =
      std::to_string(std::to_underlying(ast::FactFamily::lifecycle_cleanup));
  const std::string token_text = std::to_string(token);
  const std::string relation_kind =
      std::to_string(std::to_underlying(ast::LifecycleCleanupKind::relations));
  const std::string definition_kind = std::to_string(
      std::to_underlying(ast::LifecycleCleanupKind::definitions));
  std::int64_t deleted = execute(
      database, result.report,
      "DELETE FROM edge WHERE kind<>3 AND src_id IN (SELECT s.id FROM symbol "
      "s JOIN temp.cidx_batch_aux a JOIN temp.cidx_batch_file_map fm ON "
      "fm.batch=a.batch AND fm.handle=a.file_handle WHERE a.family=" +
          cleanup + " AND a.i0=" + relation_kind +
          " AND a.batch=" + token_text + " AND s.file_id=fm.file_id)");
  deleted +=
      execute(database, result.report,
              "DELETE FROM definition WHERE file_id IN (SELECT fm.file_id FROM "
              "temp.cidx_batch_aux a JOIN temp.cidx_batch_file_map fm ON "
              "fm.batch=a.batch AND fm.handle=a.file_handle WHERE a.family=" +
                  cleanup + " AND a.i0=" + definition_kind +
                  " AND a.batch=" + token_text + ")");
  result.report.families[ast::FactFamily::lifecycle_cleanup].deleted +=
      static_cast<std::uint64_t>(std::max<std::int64_t>(0, deleted));
}

void apply_types(SqliteDb &database, FactBatchWriterResult &result,
                 std::int64_t token) {
  auto insert = prepare_statement(
      database, result.report,
      "INSERT INTO type_node(type_key,spelling,kind,is_const,is_volatile,"
      "is_restrict,decl_usr,decl_id,canonical_id,extent) "
      "SELECT type_key,spelling,kind,is_const,is_volatile,is_restrict,decl_usr,"
      "(SELECT MIN(id) FROM symbol WHERE usr=t.decl_usr),NULL,extent "
      "FROM temp.cidx_batch_type t WHERE batch=? ORDER BY ordinal "
      "ON CONFLICT(type_key) DO UPDATE SET "
      "decl_id=COALESCE(excluded.decl_id,type_node.decl_id),"
      "extent=COALESCE(excluded.extent,type_node.extent)");
  insert.bind(1, token);
  insert.step_done();
  note_statement_stats(result.report, insert);
  ++result.report.statement_executions;

  auto map = prepare_statement(
      database, result.report,
      "INSERT INTO temp.cidx_batch_type_map(batch,handle,type_id) "
      "SELECT ?,t.handle,n.id FROM temp.cidx_batch_type t JOIN type_node n "
      "ON n.type_key=t.type_key WHERE t.batch=?");
  map.bind(1, token);
  map.bind(2, token);
  map.step_done();
  note_statement_stats(result.report, map);
  ++result.report.statement_executions;

  auto canonical = prepare_statement(
      database, result.report,
      "UPDATE type_node AS n SET canonical_id=(SELECT cm.type_id "
      "FROM temp.cidx_batch_type t JOIN temp.cidx_batch_type_map own "
      "ON own.batch=t.batch AND own.handle=t.handle "
      "JOIN temp.cidx_batch_type_map cm ON cm.batch=t.batch "
      "AND cm.handle=t.canonical_handle WHERE t.batch=? AND own.type_id=n.id) "
      "WHERE n.id IN (SELECT own.type_id FROM temp.cidx_batch_type t "
      "JOIN temp.cidx_batch_type_map own ON own.batch=t.batch AND "
      "own.handle=t.handle "
      "WHERE t.batch=? AND t.canonical_handle IS NOT NULL)");
  canonical.bind(1, token);
  canonical.bind(2, token);
  canonical.step_done();
  note_statement_stats(result.report, canonical);
  ++result.report.statement_executions;

  auto &rows = result.report.families[ast::FactFamily::types];
  rows.inserted += rows.staged;
}

void apply_definitions(SqliteDb &database, FactBatchWriterResult &result,
                       std::int64_t token) {
  auto insert = prepare_statement(
      database, result.report,
      "INSERT INTO "
      "definition(symbol_id,component_id,file_id,line,col,end_line,end_col,"
      "init_text) "
      "SELECT "
      "sm.symbol_id,d.component_id,fm.file_id,b.line,b.col,b.end_line,b.end_"
      "col,b.init_text "
      "FROM temp.cidx_batch_definition b "
      "JOIN temp.cidx_batch_symbol_map sm ON sm.batch=b.batch AND "
      "sm.handle=b.symbol_handle "
      "JOIN temp.cidx_batch_file_map fm ON fm.batch=b.batch AND "
      "fm.handle=b.file_handle "
      "JOIN file f ON f.id=fm.file_id JOIN directory d ON d.id=f.directory_id "
      "WHERE b.batch=? ORDER BY b.ordinal "
      "ON CONFLICT(component_id,file_id,symbol_id) DO UPDATE SET "
      "line=excluded.line,col=excluded.col,end_line=excluded.end_line,"
      "end_col=excluded.end_col,init_text=excluded.init_text");
  insert.bind(1, token);
  insert.step_done();
  note_statement_stats(result.report, insert);
  ++result.report.statement_executions;

  auto map = prepare_statement(
      database, result.report,
      "INSERT INTO temp.cidx_batch_definition_map(batch,handle,definition_id) "
      "SELECT ?,b.handle,d.id FROM temp.cidx_batch_definition b "
      "JOIN temp.cidx_batch_symbol_map sm ON sm.batch=b.batch AND "
      "sm.handle=b.symbol_handle "
      "JOIN temp.cidx_batch_file_map fm ON fm.batch=b.batch AND "
      "fm.handle=b.file_handle "
      "JOIN file f ON f.id=fm.file_id JOIN directory dir ON "
      "dir.id=f.directory_id "
      "JOIN definition d ON d.component_id=dir.component_id AND "
      "d.file_id=fm.file_id "
      "AND d.symbol_id=sm.symbol_id WHERE b.batch=?");
  map.bind(1, token);
  map.bind(2, token);
  map.step_done();
  note_statement_stats(result.report, map);
  ++result.report.statement_executions;
  result.report.families[ast::FactFamily::definitions].inserted +=
      result.report.families[ast::FactFamily::definitions].staged;
}

void apply_relations(SqliteDb &database, FactBatchWriterResult &result,
                     std::int64_t token) {
  auto insert = prepare_statement(
      database, result.report,
      "INSERT INTO "
      "edge(src_id,dst_id,kind,count,base_access,is_virtual,vtable_slot) "
      "SELECT "
      "src.symbol_id,dst.symbol_id,r.kind,r.count,r.base_access,r.is_virtual,"
      "NULL "
      "FROM temp.cidx_batch_relation r "
      "JOIN temp.cidx_batch_symbol_map src ON src.batch=r.batch AND "
      "src.handle=r.src_handle "
      "JOIN temp.cidx_batch_symbol_map dst ON dst.batch=r.batch AND "
      "dst.handle=r.dst_handle "
      "WHERE r.batch=? ORDER BY r.ordinal "
      "ON CONFLICT(src_id,dst_id,kind) DO UPDATE SET "
      "count=CASE WHEN excluded.kind=3 THEN edge.count+excluded.count "
      "ELSE excluded.count END,"
      "base_access=COALESCE(excluded.base_access,edge.base_access),"
      "is_virtual=COALESCE(excluded.is_virtual,edge.is_virtual)");
  insert.bind(1, token);
  insert.step_done();
  note_statement_stats(result.report, insert);
  ++result.report.statement_executions;
  auto map = prepare_statement(
      database, result.report,
      "INSERT INTO temp.cidx_batch_relation_map(batch,handle,relation_id) "
      "SELECT ?,r.handle,e.id FROM temp.cidx_batch_relation r "
      "JOIN temp.cidx_batch_symbol_map src ON src.batch=r.batch AND "
      "src.handle=r.src_handle "
      "JOIN temp.cidx_batch_symbol_map dst ON dst.batch=r.batch AND "
      "dst.handle=r.dst_handle "
      "JOIN edge e ON e.src_id=src.symbol_id AND e.dst_id=dst.symbol_id AND "
      "e.kind=r.kind "
      "WHERE r.batch=?");
  map.bind(1, token);
  map.bind(2, token);
  map.step_done();
  note_statement_stats(result.report, map);
  ++result.report.statement_executions;

  auto unresolved = prepare_statement(
      database, result.report,
      "SELECT r.src_handle,r.dst_handle FROM temp.cidx_batch_relation r "
      "LEFT JOIN temp.cidx_batch_symbol_map src ON src.batch=r.batch AND "
      "src.handle=r.src_handle LEFT JOIN temp.cidx_batch_symbol_map dst ON "
      "dst.batch=r.batch AND dst.handle=r.dst_handle WHERE r.batch=? AND "
      "(src.symbol_id IS NULL OR dst.symbol_id IS NULL) LIMIT 1");
  unresolved.bind(1, token);
  if (unresolved.step()) {
    throw StorageError("relation endpoint did not resolve: " +
                       std::to_string(unresolved.col_int64(0)) + " -> " +
                       std::to_string(unresolved.col_int64(1)));
  }
  note_statement_stats(result.report, unresolved);
  ++result.report.statement_executions;
  result.report.families[ast::FactFamily::relations].inserted +=
      result.report.families[ast::FactFamily::relations].staged;
}

void apply_auxiliary(SqliteDb &database, FactBatchWriterResult &result,
                     std::int64_t token) {
  const auto family = [](ast::FactFamily value) {
    return std::to_string(std::to_underlying(value));
  };
  static_cast<void>(execute(
      database, result.report,
      "INSERT OR REPLACE INTO type_edge(src_id,kind,position,dst_id) "
      "SELECT src.type_id,a.i1,a.i2,dst.type_id FROM temp.cidx_batch_aux a "
      "JOIN temp.cidx_batch_type_map src ON src.batch=a.batch AND "
      "src.handle=a.i0 "
      "JOIN temp.cidx_batch_type_map dst ON dst.batch=a.batch AND "
      "dst.handle=a.i3 "
      "WHERE a.family=" +
          family(ast::FactFamily::type_edges) +
          " AND a.batch=" + std::to_string(token)));
  static_cast<void>(execute(
      database, result.report,
      "INSERT OR REPLACE INTO symbol_type(symbol_id,kind,type_id) "
      "SELECT sm.symbol_id,a.i1,tm.type_id FROM temp.cidx_batch_aux a "
      "JOIN temp.cidx_batch_symbol_map sm ON sm.batch=a.batch AND "
      "sm.handle=a.i0 "
      "JOIN temp.cidx_batch_type_map tm ON tm.batch=a.batch AND tm.handle=a.i2 "
      "WHERE a.family=" +
          family(ast::FactFamily::symbol_types) +
          " AND a.batch=" + std::to_string(token)));
  static_cast<void>(execute(
      database, result.report,
      "INSERT OR REPLACE INTO "
      "decl_site(symbol_id,file_id,line,col,end_line,end_col,is_definition) "
      "SELECT sm.symbol_id,fm.file_id,a.i1,a.i2,a.i3,a.i4,a.i5 "
      "FROM temp.cidx_batch_aux a "
      "JOIN temp.cidx_batch_symbol_map sm ON sm.batch=a.batch AND "
      "sm.handle=a.i0 "
      "JOIN temp.cidx_batch_file_map fm ON fm.batch=a.batch AND "
      "fm.handle=a.file_handle "
      "WHERE a.family=" +
          family(ast::FactFamily::declaration_sites) +
          " AND a.batch=" + std::to_string(token)));
  static_cast<void>(execute(
      database, result.report,
      "INSERT OR REPLACE INTO "
      "template_param(owner_id,position,param_kind,name,default_txt,"
      "type_id,default_type_id,default_ref_id) "
      "SELECT "
      "owner.symbol_id,a.i1,a.i2,a.t0,a.t1,t.type_id,dt.type_id,dr.symbol_id "
      "FROM temp.cidx_batch_aux a "
      "JOIN temp.cidx_batch_symbol_map owner ON owner.batch=a.batch AND "
      "owner.handle=a.i0 "
      "LEFT JOIN temp.cidx_batch_type_map t ON t.batch=a.batch AND "
      "t.handle=a.i3 "
      "LEFT JOIN temp.cidx_batch_type_map dt ON dt.batch=a.batch AND "
      "dt.handle=a.i4 "
      "LEFT JOIN temp.cidx_batch_symbol_map dr ON dr.batch=a.batch AND "
      "dr.handle=a.i5 "
      "WHERE a.family=" +
          family(ast::FactFamily::template_parameters) +
          " AND a.batch=" + std::to_string(token)));
  static_cast<void>(execute(
      database, result.report,
      "INSERT OR REPLACE INTO "
      "template_arg(owner_id,position,pack_index,arg_kind,ref_id,literal,type_"
      "id) "
      "SELECT owner.symbol_id,a.i1,a.i2,a.i3,r.symbol_id,a.t0,t.type_id "
      "FROM temp.cidx_batch_aux a "
      "JOIN temp.cidx_batch_symbol_map owner ON owner.batch=a.batch AND "
      "owner.handle=a.i0 "
      "LEFT JOIN temp.cidx_batch_symbol_map r ON r.batch=a.batch AND "
      "r.handle=a.i4 "
      "LEFT JOIN temp.cidx_batch_type_map t ON t.batch=a.batch AND "
      "t.handle=a.i5 "
      "WHERE a.family=" +
          family(ast::FactFamily::template_arguments) +
          " AND a.batch=" + std::to_string(token)));
  static_cast<void>(execute(
      database, result.report,
      "DELETE FROM parameter WHERE owner_id IN (SELECT DISTINCT sm.symbol_id "
      "FROM temp.cidx_batch_aux a JOIN temp.cidx_batch_symbol_map sm "
      "ON sm.batch=a.batch AND sm.handle=a.i0 WHERE a.family=" +
          family(ast::FactFamily::parameters) +
          " AND a.batch=" + std::to_string(token) + ")"));
  static_cast<void>(execute(
      database, result.report,
      "INSERT INTO "
      "parameter(owner_id,position,pack_index,name,type_id,declared_type_id,"
      "adjusted_type_id,default_text,default_origin,reference_semantics,file_"
      "id,line,col) "
      "SELECT owner.symbol_id,a.i1,a.i2,a.t0,t.type_id,dt.type_id,at.type_id,"
      "a.t1,a.t2,a.t3,pf.file_id,a.i7,a.i8 FROM temp.cidx_batch_aux a "
      "JOIN temp.cidx_batch_symbol_map owner ON owner.batch=a.batch AND "
      "owner.handle=a.i0 "
      "LEFT JOIN temp.cidx_batch_type_map t ON t.batch=a.batch AND "
      "t.handle=a.i3 "
      "LEFT JOIN temp.cidx_batch_type_map dt ON dt.batch=a.batch AND "
      "dt.handle=a.i4 "
      "LEFT JOIN temp.cidx_batch_type_map at ON at.batch=a.batch AND "
      "at.handle=a.i5 "
      "LEFT JOIN temp.cidx_batch_file_map pf ON pf.batch=a.batch AND "
      "pf.handle=a.i6 "
      "WHERE a.family=" +
          family(ast::FactFamily::parameters) +
          " AND a.batch=" + std::to_string(token)));
  static_cast<void>(execute(
      database, result.report,
      "INSERT INTO def_edge(src_def_id,dst_id,kind,count) "
      "SELECT dm.definition_id,sm.symbol_id,a.i2,COALESCE((SELECT MAX(r.count) "
      "FROM temp.cidx_batch_relation r JOIN temp.cidx_batch_symbol_map rsrc "
      "ON rsrc.batch=r.batch AND rsrc.handle=r.src_handle JOIN "
      "temp.cidx_batch_symbol_map rdst ON rdst.batch=r.batch AND "
      "rdst.handle=r.dst_handle JOIN definition d ON d.id=dm.definition_id "
      "WHERE r.batch=a.batch AND rsrc.symbol_id=d.symbol_id AND "
      "rdst.symbol_id=sm.symbol_id AND r.kind=a.i2),1) "
      "FROM temp.cidx_batch_aux a "
      "JOIN temp.cidx_batch_definition_map dm ON dm.batch=a.batch AND "
      "dm.handle=a.i0 "
      "JOIN temp.cidx_batch_symbol_map sm ON sm.batch=a.batch AND "
      "sm.handle=a.i1 "
      "WHERE a.family=" +
          family(ast::FactFamily::definition_edges) +
          " AND a.batch=" + std::to_string(token) +
          " ON CONFLICT(src_def_id,dst_id,kind) DO UPDATE SET "
          "count=excluded.count"));
  static_cast<void>(execute(database, result.report,
                            "DELETE FROM diagnostic AS d WHERE d.file_id IN "
                            "(SELECT file_id FROM temp.cidx_batch_file_map "
                            "WHERE batch=" +
                                std::to_string(token) +
                                ") AND NOT EXISTS(SELECT 1 FROM "
                                "temp.cidx_batch_aux a JOIN "
                                "temp.cidx_batch_file_map fm ON "
                                "fm.batch=a.batch AND fm.handle=a.file_handle "
                                "WHERE a.family=" +
                                family(ast::FactFamily::diagnostics) +
                                " AND a.batch=" + std::to_string(token) +
                                " AND fm.file_id=d.file_id AND a.i0=d.severity "
                                "AND a.t0=d.spelling AND a.t1 IS d.file_path "
                                "AND a.i1 IS d.line AND a.i2 IS d.col)"));
  static_cast<void>(execute(
      database, result.report,
      "INSERT INTO diagnostic(file_id,severity,spelling,file_path,line,col) "
      "SELECT fm.file_id,a.i0,a.t0,a.t1,a.i1,a.i2 FROM temp.cidx_batch_aux a "
      "JOIN temp.cidx_batch_file_map fm ON fm.batch=a.batch AND "
      "fm.handle=a.file_handle "
      "WHERE a.family=" +
          family(ast::FactFamily::diagnostics) +
          " AND a.batch=" + std::to_string(token) +
          " AND NOT EXISTS(SELECT 1 FROM diagnostic d WHERE "
          "d.file_id=fm.file_id AND d.severity=a.i0 AND d.spelling=a.t0 AND "
          "d.file_path IS a.t1 AND d.line IS a.i1 AND d.col IS a.i2)"));

  const std::string edge_sites = family(ast::FactFamily::edge_sites);
  const std::string call_args = family(ast::FactFamily::call_arguments);
  const std::string token_text = std::to_string(token);
  static_cast<void>(execute(
      database, result.report,
      "INSERT INTO "
      "external_identity(identity_kind,identity_text,resolution_status,symbol_"
      "id,type_id) "
      "SELECT DISTINCT 1,x.text,0,NULL,NULL FROM ("
      "SELECT t1 AS text FROM temp.cidx_batch_aux WHERE family=" +
          edge_sites + " AND batch=" + token_text +
          " UNION SELECT t1 FROM temp.cidx_batch_aux WHERE family=" +
          call_args + " AND batch=" + token_text +
          ") x WHERE x.text IS NOT NULL AND x.text<>'' "
          "AND NOT EXISTS(SELECT 1 FROM type_node n WHERE n.decl_usr=x.text) "
          "ON CONFLICT(identity_kind,identity_text) DO UPDATE SET "
          "resolution_status=0"));
  static_cast<void>(
      execute(database, result.report,
              "INSERT INTO "
              "external_identity(identity_kind,identity_text,resolution_status,"
              "symbol_id,type_id) "
              "SELECT DISTINCT 2,x.text,0,NULL,NULL FROM ("
              "SELECT t2 AS text FROM temp.cidx_batch_aux WHERE family=" +
                  edge_sites + " AND batch=" + token_text +
                  " UNION SELECT t2 FROM temp.cidx_batch_aux WHERE family=" +
                  call_args + " AND batch=" + token_text +
                  " UNION SELECT t3 FROM temp.cidx_batch_aux WHERE family=" +
                  call_args + " AND batch=" + token_text +
                  ") x WHERE x.text IS NOT NULL AND x.text<>'' "
                  "AND NOT EXISTS(SELECT 1 FROM symbol s WHERE s.usr=x.text) "
                  "ON CONFLICT(identity_kind,identity_text) DO UPDATE SET "
                  "resolution_status=0"));
  static_cast<void>(execute(
      database, result.report,
      "INSERT OR IGNORE INTO "
      "edge_site(edge_id,file_id,line,col,conditional,args_sig,"
      "recv_src_kind,recv_type_usr,recv_decl_usr,recv_src_kind_id,recv_type_id,"
      "recv_decl_id,recv_type_identity_id,recv_decl_identity_id,recv_param_pos,"
      "recv_type_is_value) "
      "SELECT rm.relation_id,fm.file_id,a.i2,a.i3,a.i4,NULL,NULL,NULL,NULL,"
      "CASE a.t0 WHEN 'literal' THEN 1 WHEN 'local' THEN 2 WHEN 'construct' "
      "THEN 3 "
      "WHEN 'member' THEN 4 WHEN 'global' THEN 5 WHEN 'call_result' THEN 6 "
      "WHEN 'this' THEN 7 WHEN 'unknown' THEN 8 END,"
      "(SELECT MIN(id) FROM type_node WHERE decl_usr=a.t1),"
      "(SELECT MIN(id) FROM symbol WHERE usr=a.t2),"
      "CASE WHEN EXISTS(SELECT 1 FROM type_node WHERE decl_usr=a.t1) THEN NULL "
      "ELSE (SELECT id FROM external_identity WHERE identity_kind=1 AND "
      "identity_text=a.t1) END,"
      "CASE WHEN EXISTS(SELECT 1 FROM symbol WHERE usr=a.t2) THEN NULL "
      "ELSE (SELECT id FROM external_identity WHERE identity_kind=2 AND "
      "identity_text=a.t2) END,"
      "a.i5,a.i6 FROM temp.cidx_batch_aux a "
      "JOIN temp.cidx_batch_relation_map rm ON rm.batch=a.batch AND "
      "rm.handle=a.i0 "
      "JOIN temp.cidx_batch_file_map fm ON fm.batch=a.batch AND fm.handle=a.i1 "
      "WHERE a.family=" +
          edge_sites + " AND a.batch=" + token_text));
  static_cast<void>(execute(
      database, result.report,
      "INSERT OR IGNORE INTO "
      "call_arg(edge_id,file_id,line,col,position,src_kind,"
      "type_usr,decl_usr,callee_usr,src_kind_id,type_id,decl_id,callee_id,"
      "type_identity_id,decl_identity_id,callee_identity_id,type_is_value) "
      "SELECT rm.relation_id,fm.file_id,a.i2,a.i3,a.i4,NULL,NULL,NULL,NULL,"
      "CASE a.t0 WHEN 'literal' THEN 1 WHEN 'local' THEN 2 WHEN 'construct' "
      "THEN 3 "
      "WHEN 'member' THEN 4 WHEN 'global' THEN 5 WHEN 'call_result' THEN 6 "
      "WHEN 'this' THEN 7 WHEN 'unknown' THEN 8 END,"
      "(SELECT MIN(id) FROM type_node WHERE decl_usr=a.t1),"
      "(SELECT MIN(id) FROM symbol WHERE usr=a.t2),"
      "(SELECT MIN(id) FROM symbol WHERE usr=a.t3),"
      "CASE WHEN EXISTS(SELECT 1 FROM type_node WHERE decl_usr=a.t1) THEN NULL "
      "ELSE (SELECT id FROM external_identity WHERE identity_kind=1 AND "
      "identity_text=a.t1) END,"
      "CASE WHEN EXISTS(SELECT 1 FROM symbol WHERE usr=a.t2) THEN NULL "
      "ELSE (SELECT id FROM external_identity WHERE identity_kind=2 AND "
      "identity_text=a.t2) END,"
      "CASE WHEN EXISTS(SELECT 1 FROM symbol WHERE usr=a.t3) THEN NULL "
      "ELSE (SELECT id FROM external_identity WHERE identity_kind=2 AND "
      "identity_text=a.t3) END,"
      "a.i5 FROM temp.cidx_batch_aux a "
      "JOIN temp.cidx_batch_relation_map rm ON rm.batch=a.batch AND "
      "rm.handle=a.i0 "
      "JOIN temp.cidx_batch_file_map fm ON fm.batch=a.batch AND fm.handle=a.i1 "
      "WHERE a.family=" +
          call_args + " AND a.batch=" + token_text));

  for (const ast::FactFamily applied :
       {ast::FactFamily::declaration_sites, ast::FactFamily::edge_sites,
        ast::FactFamily::call_arguments, ast::FactFamily::template_parameters,
        ast::FactFamily::template_arguments, ast::FactFamily::type_edges,
        ast::FactFamily::parameters, ast::FactFamily::symbol_types,
        ast::FactFamily::definition_edges, ast::FactFamily::diagnostics}) {
    result.report.families[applied].inserted +=
        result.report.families[applied].staged;
  }
}

void publish_includes(SqliteDb &database, FactBatchWriterResult &result,
                      const FactBatchPublicationContext &context,
                      std::int64_t token) {
  const auto main_route = std::ranges::find_if(
      context.route_plan.routes(),
      [&context](const ast::PlannedFileRoute &route) {
        return route.translation_unit == context.translation_unit &&
               route.role == ast::PlannedFileRole::translation_unit &&
               route.extraction.transient_file_handle.has_value();
      });
  if (main_route == context.route_plan.routes().end()) {
    throw StorageError("include publication has no translation-unit route");
  }
  const auto main_handle = main_route->extraction.transient_file_handle;
  if (!main_handle) {
    throw StorageError("include publication has no translation-unit handle");
  }
  const auto &configuration = main_route->extraction.partition.configuration;
  const std::int64_t main_file_id = result.file_ids.at(*main_handle);
  auto config = prepare_statement(
      database, result.report,
      "INSERT INTO "
      "include_config(tu_file_id,digest,driver,working_dir,arguments,"
      "lang_mode,resource_dir,translation_unit_config_id) "
      "VALUES(?,?,?,?,?,?,?,?) "
      "ON CONFLICT(tu_file_id,digest) DO UPDATE SET driver=excluded.driver,"
      "working_dir=excluded.working_dir,arguments=excluded.arguments,"
      "lang_mode=excluded.lang_mode,resource_dir=excluded.resource_dir,"
      "translation_unit_config_id=excluded.translation_unit_config_id "
      "RETURNING id");
  config.bind(1, main_file_id);
  config.bind(2, std::string_view(configuration.normalized_configuration));
  bind_optional(config, 3, configuration.content.driver);
  bind_optional(config, 4, configuration.content.working_dir);
  const std::string arguments =
      json_min::encode_string_array(configuration.content.arguments);
  config.bind(5, std::string_view(arguments));
  bind_optional(config, 6, configuration.content.lang_mode);
  bind_optional(config, 7, configuration.content.resource_dir);
  if (context.configuration_id >= 0) {
    config.bind(8, context.configuration_id);
  } else {
    config.bind_null(8);
  }
  if (!config.step()) {
    throw StorageError("include configuration publication returned no id");
  }
  const std::int64_t include_config_id = config.col_int64(0);
  config.step_done();
  note_statement_stats(result.report, config);
  ++result.report.statement_executions;

  const std::string include_family =
      std::to_string(std::to_underlying(ast::FactFamily::includes));
  const std::string macro_family =
      std::to_string(std::to_underlying(ast::FactFamily::macros));
  const std::string batch_token_text = std::to_string(token);
  const std::string include_config_text = std::to_string(include_config_id);
  static_cast<void>(execute(
      database, result.report,
      "UPDATE temp.cidx_batch_aux AS a SET t3=(SELECT CAST(e.dst_file_id AS "
      "TEXT) FROM temp.cidx_batch_file_map src JOIN include_edge e ON "
      "e.src_file_id=src.file_id AND e.dst_path=a.t0 AND e.config_id=" +
          include_config_text +
          " WHERE src.batch=a.batch AND src.handle=a.file_handle LIMIT 1) "
          "WHERE a.family=" +
          include_family + " AND a.batch=" + batch_token_text +
          " AND a.i9 IS NULL AND EXISTS(SELECT 1 FROM "
          "temp.cidx_batch_file_map src JOIN include_edge e ON "
          "e.src_file_id=src.file_id AND e.dst_path=a.t0 AND e.config_id=" +
          include_config_text +
          " WHERE src.batch=a.batch AND src.handle=a.file_handle AND "
          "e.dst_file_id IS NOT NULL)"));
  static_cast<void>(execute(
      database, result.report,
      "DELETE FROM include_edge WHERE config_id=" + include_config_text +
          " AND src_file_id IN (SELECT file_id FROM temp.cidx_batch_file_map "
          "WHERE batch=" +
          batch_token_text + ")"));
  static_cast<void>(execute(
      database, result.report,
      "DELETE FROM include_macro_use WHERE config_id=" + include_config_text +
          " AND src_file_id IN (SELECT file_id FROM temp.cidx_batch_file_map "
          "WHERE batch=" +
          batch_token_text + ")"));
  static_cast<void>(execute(
      database, result.report,
      "INSERT INTO include_edge(src_file_id,dst_file_id,dst_path,config_id,"
      "is_system,is_generated,count) "
      "SELECT src.file_id,COALESCE(dst.file_id,CASE WHEN a.t3 GLOB '[0-9]*' "
      "THEN CAST(a.t3 AS INTEGER) END,persisted.id,(SELECT pf.id FROM file pf "
      "JOIN directory pd ON pd.id=pf.directory_id JOIN component pc ON "
      "pc.id=pd.component_id LEFT JOIN repository pr ON "
      "pr.id=pc.repository_id LEFT JOIN clone pcl ON "
      "pcl.id=pr.active_clone_id WHERE pf.name=a.t5 AND pd.path=a.t4 AND "
      "((pc.repository_id IS NULL AND pc.path=a.t3) OR "
      "(pc.repository_id IS NOT NULL AND "
      "CASE WHEN pc.path='.' THEN pcl.path ELSE rtrim(pcl.path,'/')||'/'||"
      "pc.path END=a.t3)) LIMIT 1)),a.t0," +
          include_config_text +
          ",MAX(a.i7),0,COUNT(*) FROM temp.cidx_batch_aux a "
          "JOIN temp.cidx_batch_file_map src ON src.batch=a.batch "
          "AND src.handle=a.file_handle "
          "LEFT JOIN temp.cidx_batch_file_map dst ON dst.batch=a.batch "
          "AND dst.handle=a.i9 LEFT JOIN component dc ON dc.path=a.t3 LEFT "
          "JOIN "
          "directory dd ON dd.component_id=dc.id AND dd.path=a.t4 LEFT JOIN "
          "file "
          "persisted ON persisted.directory_id=dd.id AND persisted.name=a.t5 "
          "WHERE a.family=" +
          include_family + " AND a.batch=" + batch_token_text +
          " GROUP BY src.file_id,a.t0"));
  static_cast<void>(execute(
      database, result.report,
      "INSERT INTO "
      "include_site(edge_id,line,col,begin_offset,end_offset,spelling,"
      "is_angled,directive,cond_fingerprint,resolved,guarded) "
      "SELECT e.id,a.i0,a.i1,a.i2,a.i3,a.t1,a.i5,a.i4,a.t2,a.i6,a.i8 "
      "FROM temp.cidx_batch_aux a JOIN temp.cidx_batch_file_map src "
      "ON src.batch=a.batch AND src.handle=a.file_handle JOIN include_edge e "
      "ON e.src_file_id=src.file_id AND e.dst_path=a.t0 AND e.config_id=" +
          include_config_text + " WHERE a.family=" + include_family +
          " AND a.batch=" + batch_token_text));
  static_cast<void>(
      execute(database, result.report,
              "INSERT INTO "
              "include_macro_use(src_file_id,def_path,name,config_id,count) "
              "SELECT src.file_id,a.t0,a.t1," +
                  include_config_text +
                  ",SUM(a.i0) FROM temp.cidx_batch_aux a "
                  "JOIN temp.cidx_batch_file_map src ON src.batch=a.batch "
                  "AND src.handle=a.file_handle WHERE a.family=" +
                  macro_family + " AND a.batch=" + batch_token_text +
                  " GROUP BY src.file_id,a.t0,a.t1"));
  result.report.families[ast::FactFamily::includes].inserted +=
      result.report.families[ast::FactFamily::includes].staged;
  result.report.families[ast::FactFamily::macros].inserted +=
      result.report.families[ast::FactFamily::macros].staged;
}

void publish_applicability(SqliteDb &database, FactBatchWriterResult &result,
                           const FactBatchPublicationContext &context,
                           std::int64_t token) {
  if (context.configuration_id < 0) {
    return;
  }
  const auto family = [](ast::FactFamily value) {
    return std::to_string(std::to_underlying(value));
  };
  const std::string token_text = std::to_string(token);
  const std::string config_text = std::to_string(context.configuration_id);
  const std::string applicability = family(ast::FactFamily::applicability);
  static_cast<void>(execute(
      database, result.report,
      "UPDATE temp.cidx_batch_file_map AS fm SET generation=COALESCE((SELECT "
      "MAX(fa.generation)+1 FROM fact_applicability fa WHERE "
      "fa.file_id=fm.file_id AND fa.config_id=" +
          config_text + "),1) WHERE fm.batch=" + token_text +
          " AND fm.planned=1"));
  static_cast<void>(execute(
      database, result.report,
      "INSERT INTO file_config(file_id,config_id,role,state,reason) "
      "SELECT fm.file_id," +
          config_text +
          ",CASE a.i0 WHEN 0 THEN 'translation_unit' ELSE 'header' END,"
          "CASE a.i1 WHEN 0 THEN 'registered' WHEN 1 THEN 'unregistered' "
          "WHEN 2 THEN 'ambiguous' WHEN 3 THEN 'stale' ELSE 'unavailable' END,"
          "a.t0 FROM temp.cidx_batch_aux a JOIN temp.cidx_batch_file_map fm "
          "ON fm.batch=a.batch AND fm.handle=a.file_handle WHERE a.family=" +
          applicability + " AND a.batch=" + token_text + " AND a.i0=0" +
          " ON CONFLICT(file_id,config_id,role) DO UPDATE SET "
          "state=excluded.state,reason=excluded.reason"));
  static_cast<void>(execute(
      database, result.report,
      "INSERT INTO file_config(file_id,config_id,role,state,reason) "
      "SELECT DISTINCT selected.file_id," +
          config_text +
          ",'header','registered',NULL FROM (SELECT src.file_id FROM "
          "temp.cidx_batch_aux include JOIN temp.cidx_batch_file_map src ON "
          "src.batch=include.batch AND src.handle=include.file_handle WHERE "
          "include.family=" +
          family(ast::FactFamily::includes) +
          " AND include.batch=" + token_text +
          " UNION SELECT COALESCE(dst.file_id,CASE WHEN include.t3 GLOB "
          "'[0-9]*' THEN CAST(include.t3 AS INTEGER) END,persisted.id,(SELECT "
          "pf.id FROM file pf JOIN directory pd ON pd.id=pf.directory_id "
          "JOIN component pc ON pc.id=pd.component_id LEFT JOIN repository pr "
          "ON pr.id=pc.repository_id LEFT JOIN clone pcl ON "
          "pcl.id=pr.active_clone_id WHERE pf.name=include.t5 AND "
          "pd.path=include.t4 AND ((pc.repository_id IS NULL AND "
          "pc.path=include.t3) OR (pc.repository_id IS NOT NULL AND CASE WHEN "
          "pc.path='.' THEN pcl.path ELSE rtrim(pcl.path,'/')||'/'||pc.path "
          "END=include.t3)) LIMIT 1)) "
          "FROM temp.cidx_batch_aux include LEFT JOIN "
          "temp.cidx_batch_file_map dst ON dst.batch=include.batch AND "
          "dst.handle=include.i9 LEFT JOIN component dc ON "
          "dc.path=include.t3 LEFT JOIN directory dd ON "
          "dd.component_id=dc.id AND dd.path=include.t4 LEFT JOIN file "
          "persisted ON persisted.directory_id=dd.id AND "
          "persisted.name=include.t5 WHERE "
          "(include.i9 IS NOT NULL OR include.t5 IS NOT NULL) AND "
          "include.family=" +
          family(ast::FactFamily::includes) +
          " AND include.batch=" + token_text +
          ") selected WHERE selected.file_id IS NOT NULL "
          "ON CONFLICT(file_id,config_id,role) DO UPDATE "
          "SET state=excluded.state,reason=excluded.reason"));

  const auto publish = [&](std::string_view kind, std::string_view rows) {
    static_cast<void>(execute(
        database, result.report,
        "INSERT INTO "
        "fact_applicability(fact_kind,fact_id,file_id,config_id,generation) "
        "SELECT '" +
            std::string(kind) + "'," + std::string(rows) +
            " ON CONFLICT(fact_kind,fact_id,file_id,config_id) DO UPDATE SET "
            "generation=excluded.generation"));
  };
  publish("symbol", "sm.symbol_id,fm.file_id," + config_text + "," +
                        "fm.generation" +
                        " FROM temp.cidx_batch_symbol b "
                        "JOIN temp.cidx_batch_symbol_map sm ON "
                        "sm.batch=b.batch AND sm.handle=b.handle "
                        "JOIN temp.cidx_batch_file_map fm ON fm.batch=b.batch "
                        "AND fm.handle=b.file_handle AND fm.planned=1 "
                        "WHERE b.batch=" +
                        token_text + " AND b.line>0");
  publish("edge", "rm.relation_id,fm.file_id," + config_text + "," +
                      "fm.generation" +
                      " FROM temp.cidx_batch_relation r "
                      "JOIN temp.cidx_batch_relation_map rm ON "
                      "rm.batch=r.batch AND rm.handle=r.handle "
                      "JOIN temp.cidx_batch_file_map fm ON fm.batch=r.batch "
                      "AND fm.handle=r.file_handle "
                      "AND fm.planned=1 "
                      "WHERE r.batch=" +
                      token_text);
  publish("definition", "dm.definition_id,fm.file_id," + config_text + "," +
                            "fm.generation" +
                            " FROM temp.cidx_batch_definition d "
                            "JOIN temp.cidx_batch_definition_map dm ON "
                            "dm.batch=d.batch AND dm.handle=d.handle "
                            "JOIN temp.cidx_batch_file_map fm ON "
                            "fm.batch=d.batch AND fm.handle=d.file_handle "
                            "AND fm.planned=1 "
                            "WHERE d.batch=" +
                            token_text);
  const bool publish_diagnostics = context.route_plan.routes().size() == 1;
  if (publish_diagnostics) {
    publish("diagnostic",
            "d.id,fm.file_id," + config_text + ",fm.generation" +
                " FROM diagnostic d JOIN temp.cidx_batch_file_map fm "
                "ON fm.file_id=d.file_id WHERE fm.batch=" +
                token_text + " AND fm.planned=1 AND fm.generation>1");
  }

  const auto derive = [&](std::string_view kind, std::string_view rows) {
    publish(kind, std::string(rows) +
                      " JOIN temp.cidx_batch_file_map current ON "
                      "current.batch=" +
                      token_text +
                      " AND current.planned=1 AND current.file_id=fa.file_id "
                      "AND current.generation=fa.generation WHERE "
                      "fa.config_id=" +
                      config_text);
  };
  derive("def_edge",
         "de.rowid,fa.file_id,fa.config_id,fa.generation FROM def_edge de "
         "JOIN fact_applicability fa ON fa.fact_kind='definition' AND "
         "fa.fact_id=de.src_def_id");
  derive("decl_site",
         "ds.rowid,fa.file_id,fa.config_id,fa.generation FROM decl_site ds "
         "JOIN fact_applicability fa ON fa.fact_kind='symbol' AND "
         "fa.fact_id=ds.symbol_id AND ds.file_id=fa.file_id");
  derive("parameter",
         "p.owner_id,fa.file_id,fa.config_id,fa.generation FROM parameter p "
         "JOIN fact_applicability fa ON fa.fact_kind='symbol' AND "
         "fa.fact_id=p.owner_id");
  derive("symbol_type",
         "st.symbol_id,fa.file_id,fa.config_id,fa.generation FROM symbol_type "
         "st JOIN fact_applicability fa ON fa.fact_kind='symbol' AND "
         "fa.fact_id=st.symbol_id");
  derive("type_node",
         "st.type_id,fa.file_id,fa.config_id,fa.generation FROM symbol_type st "
         "JOIN fact_applicability fa ON fa.fact_kind='symbol' AND "
         "fa.fact_id=st.symbol_id AND st.type_id IS NOT NULL");
  derive("type_edge",
         "te.src_id,fa.file_id,fa.config_id,fa.generation FROM type_edge te "
         "JOIN fact_applicability fa ON fa.fact_kind='type_node' AND "
         "fa.fact_id=te.src_id");
  derive("entity_node",
         "en.id,fa.file_id,fa.config_id,fa.generation FROM entity_node en "
         "JOIN fact_applicability fa ON fa.fact_kind='symbol' AND "
         "fa.fact_id=en.id");
  derive("entity_edge",
         "ee.rowid,fa.file_id,fa.config_id,fa.generation FROM entity_edge ee "
         "JOIN fact_applicability fa ON fa.fact_kind='symbol' AND "
         "fa.fact_id=ee.src_id");
  derive("template_param",
         "tp.owner_id,fa.file_id,fa.config_id,fa.generation FROM "
         "template_param tp JOIN fact_applicability fa ON "
         "fa.fact_kind='symbol' AND fa.fact_id=tp.owner_id");
  derive("template_arg",
         "ta.owner_id,fa.file_id,fa.config_id,fa.generation FROM template_arg "
         "ta JOIN fact_applicability fa ON fa.fact_kind='symbol' AND "
         "fa.fact_id=ta.owner_id");
  derive("call_arg",
         "ca.edge_id,fa.file_id,fa.config_id,fa.generation FROM call_arg ca "
         "JOIN fact_applicability fa ON fa.fact_kind='edge' AND "
         "fa.fact_id=ca.edge_id");
  derive("possible_call",
         "pc.src_def_id,fa.file_id,fa.config_id,fa.generation FROM "
         "possible_call pc JOIN fact_applicability fa ON "
         "fa.fact_kind='definition' AND fa.fact_id=pc.src_def_id");

  const auto stale = [&](std::string_view alias) {
    return std::string(alias) + ".config_id=" + config_text + " AND " +
           std::string(alias) +
           ".file_id IN (SELECT current.file_id FROM "
           "temp.cidx_batch_file_map current WHERE current.batch=" +
           token_text + " AND current.planned=1) AND " + std::string(alias) +
           ".generation<>(SELECT current.generation FROM "
           "temp.cidx_batch_file_map current WHERE current.batch=" +
           token_text +
           " AND current.planned=1 AND current.file_id=" + std::string(alias) +
           ".file_id LIMIT 1)";
  };
  const std::string scoped = stale("fact_applicability");
  auto has_stale = prepare_statement(
      database, result.report,
      "SELECT EXISTS(SELECT 1 FROM fact_applicability WHERE fact_kind<>"
      "'diagnostic' AND " +
          scoped + " LIMIT 1)");
  const bool remove_stale = has_stale.step() && has_stale.col_int64(0) != 0;
  has_stale.step_done();
  note_statement_stats(result.report, has_stale);
  ++result.report.statement_executions;
  std::int64_t deleted = 0;
  if (remove_stale) {
    static_cast<void>(execute(
        database, result.report,
        "DELETE FROM edge WHERE id IN (SELECT fact_id FROM fact_applicability "
        "WHERE fact_kind='edge' AND " +
            scoped +
            ") AND NOT EXISTS(SELECT 1 FROM fact_applicability other "
            "WHERE other.fact_kind='edge' AND other.fact_id=edge.id AND NOT(" +
            stale("other") + "))"));
    static_cast<void>(
        execute(database, result.report,
                "DELETE FROM definition WHERE id IN (SELECT fact_id FROM "
                "fact_applicability "
                "WHERE fact_kind='definition' AND " +
                    scoped +
                    ") AND NOT EXISTS(SELECT 1 FROM fact_applicability other "
                    "WHERE other.fact_kind='definition' AND "
                    "other.fact_id=definition.id AND NOT(" +
                    stale("other") + "))"));
    static_cast<void>(execute(
        database, result.report,
        "DELETE FROM symbol WHERE id IN (SELECT fact_id FROM "
        "fact_applicability WHERE fact_kind='symbol' AND " +
            scoped +
            ") AND NOT EXISTS(SELECT 1 FROM fact_applicability other "
            "WHERE other.fact_kind='symbol' AND other.fact_id=symbol.id AND "
            "NOT(" +
            stale("other") + "))"));
    deleted = execute(database, result.report,
                      "DELETE FROM fact_applicability WHERE fact_kind<>"
                      "'diagnostic' AND " +
                          scoped);
  }
  if (publish_diagnostics) {
    static_cast<void>(execute(
        database, result.report,
        "INSERT OR REPLACE INTO fact_applicability(fact_kind,fact_id,file_id,"
        "config_id,generation) SELECT 'diagnostic',d.id,fm.file_id," +
            config_text +
            ",fm.generation FROM diagnostic d JOIN "
            "temp.cidx_batch_file_map fm ON fm.file_id=d.file_id WHERE "
            "fm.batch=" +
            token_text + " AND fm.planned=1 AND fm.generation>1"));
  }
  static_cast<void>(execute(
      database, result.report,
      "UPDATE fact_applicability SET generation=1 WHERE config_id=" +
          config_text +
          " AND generation<>1 "
          " AND file_id IN (SELECT file_id FROM temp.cidx_batch_file_map "
          "WHERE batch=" +
          token_text +
          " AND planned=1) AND EXISTS(SELECT 1 FROM file_config fc WHERE "
          "fc.file_id=fact_applicability.file_id AND fc.config_id=" +
          config_text + ")"));
  result.report.families[ast::FactFamily::applicability].inserted +=
      result.report.families[ast::FactFamily::applicability].staged;
  result.report.families[ast::FactFamily::lifecycle_cleanup].deleted +=
      static_cast<std::uint64_t>(std::max<std::int64_t>(0, deleted));
}

void collect_maps(SqliteDb &database, FactBatchWriterResult &result,
                  std::int64_t token) {
  auto symbols = prepare_statement(
      database, result.report,
      "SELECT handle,symbol_id FROM temp.cidx_batch_symbol_map WHERE batch=? "
      "ORDER BY handle");
  symbols.bind(1, token);
  while (symbols.step()) {
    result.symbol_ids.emplace(symbols.col_int64(0), symbols.col_int64(1));
  }
  note_statement_stats(result.report, symbols);
  auto files =
      prepare_statement(database, result.report,
                        "SELECT handle,file_id FROM temp.cidx_batch_file_map "
                        "WHERE batch=? ORDER BY handle");
  files.bind(1, token);
  while (files.step()) {
    result.file_ids.insert_or_assign(files.col_int64(0), files.col_int64(1));
  }
  note_statement_stats(result.report, files);
  auto relations = prepare_statement(
      database, result.report,
      "SELECT handle,relation_id FROM temp.cidx_batch_relation_map WHERE "
      "batch=? ORDER BY handle");
  relations.bind(1, token);
  while (relations.step()) {
    result.relation_ids.emplace(relations.col_int64(0), relations.col_int64(1));
  }
  note_statement_stats(result.report, relations);
  auto types =
      prepare_statement(database, result.report,
                        "SELECT handle,type_id FROM temp.cidx_batch_type_map "
                        "WHERE batch=? ORDER BY handle");
  types.bind(1, token);
  while (types.step()) {
    result.type_ids.emplace(types.col_int64(0), types.col_int64(1));
  }
  note_statement_stats(result.report, types);
  auto definitions = prepare_statement(
      database, result.report,
      "SELECT handle,definition_id FROM temp.cidx_batch_definition_map WHERE "
      "batch=? ORDER BY handle");
  definitions.bind(1, token);
  while (definitions.step()) {
    result.definition_ids.emplace(definitions.col_int64(0),
                                  definitions.col_int64(1));
  }
  note_statement_stats(result.report, definitions);
  result.report.statement_executions += 5;
}

} // namespace

auto fact_batch_writer_phase_name(FactBatchWriterPhase phase)
    -> std::string_view {
  switch (phase) {
  case FactBatchWriterPhase::validate_plan:
    return "validate-plan";
  case FactBatchWriterPhase::resolve_file_rows:
    return "resolve-file-rows";
  case FactBatchWriterPhase::load_temporary_staging:
    return "load-temporary-staging";
  case FactBatchWriterPhase::resolve_natural_keys:
    return "resolve-natural-keys";
  case FactBatchWriterPhase::apply_entities:
    return "apply-entities";
  case FactBatchWriterPhase::apply_annotations:
    return "apply-annotations";
  case FactBatchWriterPhase::apply_relations:
    return "apply-relations";
  case FactBatchWriterPhase::apply_sites_and_external_identities:
    return "apply-sites-and-external-identities";
  case FactBatchWriterPhase::publish_includes_and_applicability:
    return "publish-includes-and-applicability";
  case FactBatchWriterPhase::cleanup_stale_facts:
    return "cleanup-stale-facts";
  case FactBatchWriterPhase::revalidate_sources:
    return "revalidate-sources";
  case FactBatchWriterPhase::mark_current:
    return "mark-current";
  case FactBatchWriterPhase::commit:
    return "commit";
  }
  throw std::logic_error("unknown FactBatchWriter phase");
}

FactBatchWriter::FactBatchWriter(cidx::SqliteStorageService &storage)
    : storage_(storage) {}

auto FactBatchWriter::apply(const ast::FactBatch &batch,
                            const FactBatchPublicationContext &context)
    -> FactBatchWriterResult {
  FactBatchWriterResult result;
  const std::int64_t token = batch_token(batch, context);
  auto transaction = storage_.transaction();
  try {
    FactBatchPublicationContext publication = context;
    if (publication.configuration) {
      publication.configuration_id =
          storage_.add_translation_unit_config(*publication.configuration);
    }
    result.configuration_id = publication.configuration_id;
    if (context.route_plan.generation() != context.expected_generation) {
      throw StorageError("FactBatch publication plan generation is stale");
    }
    if (context.translation_unit.empty()) {
      throw StorageError("FactBatch publication requires a translation unit");
    }
    for (const ast::PlannedFileRoute &route : context.route_plan.routes()) {
      if (route.translation_unit != context.translation_unit) {
        continue;
      }
      if (context.source_is_current &&
          !context.source_is_current(route.path, route.snapshot)) {
        throw StorageError(route.path +
                           ": source changed before FactBatch publication");
      }
      if (route.role == ast::PlannedFileRole::translation_unit &&
          !route.existing_file_id) {
        throw StorageError("translation-unit route has no existing file row");
      }
      const std::int64_t file_id =
          route.existing_file_id
              ? *route.existing_file_id
              : storage_.add_file_path(route.path, route.snapshot.mtime,
                                       route.snapshot.md5,
                                       route.compile_options, route.driver);
      if (!route.extraction.transient_file_handle) {
        throw StorageError("publication route has no transient file handle");
      }
      result.file_ids.emplace(*route.extraction.transient_file_handle, file_id);
    }

    for (const auto &[batch_handle, partition] : batch.file_keys()) {
      const auto route = std::ranges::find_if(
          context.route_plan.routes(),
          [&partition, &context](const ast::PlannedFileRoute &candidate) {
            return candidate.translation_unit == context.translation_unit &&
                   candidate.extraction.partition == partition;
          });
      if (route == context.route_plan.routes().end()) {
        const auto existing = storage_.get_file(partition.file.portable_path());
        if (!existing) {
          throw StorageError(
              "FactBatch partition is absent from publication plan: " +
              partition.file.portable_path());
        }
        result.file_ids.emplace(batch_handle, existing->id);
        continue;
      }
      if (!route->extraction.transient_file_handle) {
        throw StorageError("FactBatch publication route has no file handle");
      }
      result.file_ids.emplace(
          batch_handle,
          result.file_ids.at(*route->extraction.transient_file_handle));
    }

    SqliteDb &database = storage_.raw_db();
    create_temporary_schema(database, result.report);
    clear_temporary_rows(database, result.report, token);
    load_file_map(database, result, batch, context, token);
    load_symbols(storage_, database, result, batch, token);
    load_types(database, result, batch, token);
    load_definitions(database, result, batch, token);
    load_relations(database, result, batch, token);
    load_auxiliary_rows(database, result, auxiliary_rows(batch), token);
    inject(context.failure, FactBatchWriterFailurePoint::temporary_load);

    inject(context.failure,
           FactBatchWriterFailurePoint::natural_key_resolution);
    apply_lifecycle_cleanup(database, result, token);
    apply_symbols(database, result, token);
    apply_types(database, result, token);
    apply_definitions(database, result, token);
    inject(context.failure, FactBatchWriterFailurePoint::entity_apply);

    inject(context.failure, FactBatchWriterFailurePoint::annotation_apply);
    apply_relations(database, result, token);
    inject(context.failure, FactBatchWriterFailurePoint::relation_apply);
    apply_auxiliary(database, result, token);
    collect_maps(database, result, token);
    inject(context.failure, FactBatchWriterFailurePoint::site_apply);
    publish_includes(database, result, publication, token);
    publish_applicability(database, result, publication, token);
    inject(context.failure, FactBatchWriterFailurePoint::publication);

    for (const ast::PlannedFileRoute &route : context.route_plan.routes()) {
      if (route.translation_unit != context.translation_unit ||
          !route.extraction.transient_file_handle) {
        continue;
      }
      if (context.source_is_current &&
          !context.source_is_current(route.path, route.snapshot)) {
        throw StorageError(route.path +
                           ": source changed before FactBatch commit");
      }
    }
    inject(context.failure, FactBatchWriterFailurePoint::cleanup);

    for (const ast::PlannedFileRoute &route : context.route_plan.routes()) {
      if (route.translation_unit != context.translation_unit ||
          !route.extraction.transient_file_handle) {
        continue;
      }
      const std::int64_t file_id =
          result.file_ids.at(*route.extraction.transient_file_handle);
      storage_.mark_file_indexed(file_id, route.snapshot.mtime,
                                 route.snapshot.md5);
    }
    inject(context.failure, FactBatchWriterFailurePoint::before_commit);
    result.report.commit_attempted = true;
    const auto commit_started = Clock::now();
    if (context.failure == FactBatchWriterFailurePoint::commit) {
      throw StorageError("injected FactBatchWriter commit failure");
    }
    clear_temporary_rows(database, result.report, token);
    transaction.commit();
    result.report.commit_seconds = seconds_since(commit_started);
    result.report.committed = true;
  } catch (const std::exception &error) {
    std::string failure = error.what();
    try {
      transaction.rollback();
    } catch (const std::exception &rollback_error) {
      failure += "; rollback failed: ";
      failure += rollback_error.what();
    }
    result.report.committed = false;
    result.error = std::move(failure);
  }
  return result;
}

} // namespace cidx::storage
