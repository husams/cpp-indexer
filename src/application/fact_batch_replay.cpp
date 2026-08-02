#include "application/fact_batch_replay.hpp"

#include <stdexcept>

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

} // namespace

auto replay_fact_batch(const ast::FactBatch &batch, FactBatchReplayPort &port,
                       FactReplayFailurePoint failure) -> FactReplayResult {
  FactReplayResult result;
  const auto symbol_handles = inverse(batch.symbol_keys());
  const auto relation_handles = inverse(batch.relation_keys());
  try {
    port.begin_translation_unit();
    inject(FactReplayFailurePoint::before_apply, failure);
    const ast::FactRecords &records = batch.records();
    for (const ast::FileFactPartition &partition : batch.partitions()) {
      const auto symbols = partition.members.find(ast::FactFamily::symbols);
      if (symbols != partition.members.end()) {
        for (const std::size_t index : symbols->second) {
          const ast::SymbolRecord &record = records.symbols.at(index);
          const std::string natural = symbol_natural_key(partition.key, record);
          const std::int64_t batch_id = symbol_handles.at(natural);
          result.ids.symbols[batch_id] =
              port.apply_symbol(partition.key, record, natural);
        }
      }
    }
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
