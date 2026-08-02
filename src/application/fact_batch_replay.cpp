#include "application/fact_batch_replay.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cidx::application {

namespace {

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
  for (const auto &[id, value] : values) {
    result.emplace(value, id);
  }
  return result;
}

void inject(FactReplayFailurePoint actual, FactReplayFailurePoint requested) {
  if (actual == requested) {
    throw std::runtime_error("injected fact replay failure");
  }
}

struct SymbolLocation {
  ast::FactPartitionKey partition;
  std::size_t index = 0;
};

auto symbol_locations(const ast::FactBatch &batch)
    -> std::map<std::string, std::vector<SymbolLocation>> {
  std::map<std::string, std::vector<SymbolLocation>> result;
  const ast::FactRecords &records = batch.records();
  for (const ast::FileFactPartition &partition : batch.partitions()) {
    const auto found = partition.members.find(ast::FactFamily::symbols);
    if (found == partition.members.end()) {
      continue;
    }
    for (const std::size_t index : found->second) {
      const ast::SymbolRecord &record = records.symbols.at(index);
      const std::string key = symbol_natural_key(partition.key, record) + '\n' +
                              ast::stable_symbol_record_key(record);
      result[key].push_back({.partition = partition.key, .index = index});
    }
  }
  return result;
}

void apply_files(const ast::FactBatch &batch, FactBatchReplayPort &port,
                 TransientFactApplyMap &ids) {
  std::map<ast::FactPartitionKey, std::vector<std::int64_t>> handles;
  for (const auto &[handle, partition] : batch.file_keys()) {
    handles[partition].push_back(handle);
  }
  for (const auto &[partition, partition_handles] : handles) {
    const std::int64_t applied =
        port.apply_file(partition, partition.stable_string());
    for (const std::int64_t handle : partition_handles) {
      ids.files[handle] = applied;
    }
  }
}

void apply_symbols(const ast::FactBatch &batch, FactBatchReplayPort &port,
                   const std::map<std::string, std::int64_t> &symbol_handles,
                   TransientFactApplyMap &ids) {
  const ast::FactRecords &records = batch.records();
  auto locations = symbol_locations(batch);
  std::map<std::string, std::size_t> next_location;
  for (const ast::SymbolEmissionMetadata &metadata : records.symbol_order) {
    const std::string natural = "symbol:" + metadata.symbol.stable_string();
    const std::string key = natural + '\n' + metadata.record_key;
    const auto found = locations.find(key);
    if (found == locations.end() || found->second.empty()) {
      throw std::logic_error("symbol apply-order metadata has no record");
    }
    std::size_t &cursor = next_location[key];
    const SymbolLocation &location =
        found->second[std::min(cursor, found->second.size() - 1)];
    ++cursor;
    const ast::SymbolRecord &record = records.symbols.at(location.index);
    const std::int64_t batch_id = symbol_handles.at(natural);
    ids.symbols[batch_id] =
        port.apply_symbol(location.partition, record, natural);
  }
}

} // namespace

auto replay_fact_batch(const ast::FactBatch &batch, FactBatchReplayPort &port,
                       FactReplayFailurePoint failure) -> FactReplayResult {
  FactReplayResult result;
  const auto symbol_handles = inverse(batch.symbol_keys());
  const auto relation_handles = inverse(batch.relation_keys());
  try {
    port.begin_translation_unit();
    inject(FactReplayFailurePoint::before_apply, failure);
    apply_files(batch, port, result.ids);
    const ast::FactRecords &records = batch.records();
    apply_symbols(batch, port, symbol_handles, result.ids);
    inject(FactReplayFailurePoint::after_symbols, failure);

    for (const ast::FileFactPartition &partition : batch.partitions()) {
      for (const auto &[family, indexes] : partition.members) {
        for (const std::size_t index : indexes) {
          if (family == ast::FactFamily::symbols) {
            continue;
          }
          if (family == ast::FactFamily::relations) {
            ast::EdgeRecord relation = records.relations.at(index);
            const std::int64_t batch_relation = relation_handles.at(
                "edge:" + partition.key.stable_string() +
                std::to_string(relation.src_id) + ':' +
                std::to_string(relation.dst_id) + ':' +
                std::to_string(relation.kind) + ':' +
                (relation.base_access ? std::to_string(*relation.base_access)
                                      : "-") +
                ':' +
                (relation.is_virtual ? std::to_string(*relation.is_virtual)
                                     : "-"));
            relation.src_id = result.ids.symbols.at(relation.src_id);
            relation.dst_id = result.ids.symbols.at(relation.dst_id);
            result.ids.relations[batch_relation] =
                port.apply_relation(partition.key, relation);
            continue;
          }
          if (family == ast::FactFamily::definitions) {
            ast::DefinitionFactRecord definition =
                records.definitions.at(index);
            const std::int64_t batch_definition = definition.id;
            definition.symbol_id = result.ids.symbols.at(definition.symbol_id);
            const auto file = result.ids.files.find(definition.file_id);
            if (file == result.ids.files.end()) {
              throw std::runtime_error("unresolved transient file handle " +
                                       std::to_string(definition.file_id) +
                                       " for definition " +
                                       std::to_string(batch_definition));
            }
            definition.file_id = file->second;
            result.ids.definitions[batch_definition] =
                port.apply_definition(partition.key, definition);
            continue;
          }
          port.apply_other(partition.key, family, index, records, result.ids);
        }
      }
    }
    inject(FactReplayFailurePoint::before_commit, failure);
    port.commit_translation_unit();
    result.committed = true;
  } catch (const std::exception &error) {
    port.rollback_translation_unit();
    result.ids = {};
    result.error = error.what();
  }
  return result;
}

} // namespace cidx::application
