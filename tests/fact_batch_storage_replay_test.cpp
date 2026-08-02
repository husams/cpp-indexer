#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "application/fact_batch_replay.hpp"
#include "ast/storage_edge_sink.hpp"
#include "ast/storage_symbol_sink.hpp"
#include "storage/sqlite_adapters.hpp"
#include "storage/storage.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cidx;

namespace {

auto partition(std::string file, std::string source) -> ast::FactPartitionKey {
  return {
      .file = {.component_path = "/tmp/cidx-s072-replay",
               .directory_path = {},
               .file_name = std::move(file)},
      .configuration = {.semantic_universe = "workspace",
                        .translation_unit = "/tmp/cidx-s072-replay/main.cpp",
                        .normalized_configuration = "debug",
                        .identity_source = std::move(source)}};
}

auto symbol(const ast::FactPartitionKey &owner, std::string usr,
            std::string display) -> ast::SymbolRecord {
  ast::SymbolRecord result;
  result.file = owner.file.portable_path();
  result.usr = std::move(usr);
  result.spelling = "symbol";
  result.kind = 8;
  result.display_name = std::move(display);
  result.line = 1;
  result.col = 1;
  result.end_line = 1;
  result.end_col = 8;
  result.linkage = "external";
  result.identity_source = owner.configuration.identity_source;
  result.identity_translation_unit = owner.configuration.translation_unit;
  return result;
}

class StorageReplayPort final : public application::FactBatchReplayPort {
public:
  explicit StorageReplayPort(Storage &storage)
      : sqlite_(storage),
        ports_{.workspace = sqlite_.workspace_catalog_read(),
               .source = sqlite_.source_read(),
               .symbols_read = sqlite_.symbol_read(),
               .symbols_write = sqlite_.symbol_write(),
               .types_write = sqlite_.type_write(),
               .facts_write = sqlite_.fact_write(),
               .definitions_write = sqlite_.definition_write(),
               .unit_of_work = sqlite_.unit_of_work()},
        symbols_(ports_), edges_(ports_) {}

  void begin_translation_unit() override {
    transaction_ = ports_.unit_of_work.begin();
  }

  auto apply_file(const ast::FactPartitionKey &owner,
                  std::string_view /*natural_key*/) -> std::int64_t override {
    const auto file = ports_.source.get_file(owner.file.portable_path());
    if (!file) {
      throw std::runtime_error("replay requires a preplanned file row");
    }
    files_[owner] = file->id;
    return file->id;
  }

  auto apply_symbol(const ast::FactPartitionKey &owner,
                    const ast::SymbolRecord &record,
                    std::string_view /*natural_key*/) -> std::int64_t override {
    const std::int64_t file_id = files_.at(owner);
    symbols_.set_current_file_id(file_id);
    const auto translation_unit =
        ports_.source.get_file(owner.configuration.translation_unit);
    symbols_.set_identity_translation_unit_file_id(
        translation_unit ? translation_unit->id : file_id);
    symbols_.emit(record);
    const auto &ids = symbols_.symbol_ids(file_id);
    if (ids.empty()) {
      throw std::logic_error("storage symbol sink returned no applied id");
    }
    return ids.back();
  }

  auto apply_relation(const ast::FactPartitionKey &owner,
                      const ast::EdgeRecord &record) -> std::int64_t override {
    edges_.set_current_file_id(files_.at(owner));
    return edges_.add_edge(record);
  }

  auto apply_definition(const ast::FactPartitionKey &owner,
                        const ast::DefinitionFactRecord &record)
      -> std::int64_t override {
    edges_.set_current_file_id(files_.at(owner));
    return edges_.get_or_create_definition(
        record.symbol_id, record.file_id, record.line, record.col,
        record.end_line, record.end_col, record.init_text);
  }

  void
  apply_other(const ast::FactPartitionKey & /*owner*/, ast::FactFamily family,
              std::size_t /*record_index*/,
              const ast::FactRecords & /*records*/,
              const application::TransientFactApplyMap & /*ids*/) override {
    throw std::runtime_error("unsupported family in focused replay oracle: " +
                             std::to_string(static_cast<unsigned>(family)));
  }

  void commit_translation_unit() override {
    transaction_->commit();
    transaction_.reset();
  }

  void rollback_translation_unit() noexcept override {
    try {
      if (transaction_) {
        transaction_->rollback();
      }
    } catch (...) {
      rollback_failed_ = true;
    }
    transaction_.reset();
  }

private:
  storage::SqliteStoragePorts sqlite_;
  storage::AstStoragePorts ports_;
  ast::StorageSymbolSink symbols_;
  ast::StorageEdgeSink edges_;
  std::unique_ptr<storage::UnitOfWork> transaction_;
  std::map<ast::FactPartitionKey, std::int64_t> files_;
  bool rollback_failed_ = false;
};

struct NormalizedLayerZero {
  std::vector<std::string> symbols;
  std::vector<std::string> relations;
  std::vector<std::string> definitions;

  bool operator==(const NormalizedLayerZero &) const = default;
};

auto normalized(Storage &storage) -> NormalizedLayerZero {
  NormalizedLayerZero result;
  for (const std::string usr : {"usr-shared", "usr-target"}) {
    for (const Symbol &stored : storage.lookup_symbols_by_usr(usr)) {
      result.symbols.push_back(stored.usr + ':' +
                               stored.display_name.value_or("-") + ':' +
                               stored.linkage.value_or("-"));
      for (const DefinitionRow &definition :
           storage.definitions_of(stored.id)) {
        result.definitions.push_back(
            stored.usr + ':' + std::to_string(definition.line.value_or(0)) +
            ':' + std::to_string(definition.col.value_or(0)));
      }
      for (const GraphEdgeRow &edge :
           storage.graph_edges(stored.id, "out", {1}, false, 100)) {
        result.relations.push_back(stored.usr + ':' + edge.sym.usr + ':' +
                                   std::to_string(edge.ecount));
      }
    }
  }
  std::ranges::sort(result.symbols);
  std::ranges::sort(result.relations);
  std::ranges::sort(result.definitions);
  return result;
}

void prepare(Storage &storage) {
  static_cast<void>(
      storage.add_component("replay", "/tmp/cidx-s072-replay", "repo"));
  static_cast<void>(storage.add_file_path("/tmp/cidx-s072-replay/main.cpp"));
  static_cast<void>(storage.add_file_path("/tmp/cidx-s072-replay/header.hpp"));
}

void apply_direct(Storage &storage, const ast::FactPartitionKey &header,
                  const ast::FactPartitionKey &main) {
  storage::SqliteStoragePorts sqlite(storage);
  storage::AstStoragePorts ports{.workspace = sqlite.workspace_catalog_read(),
                                 .source = sqlite.source_read(),
                                 .symbols_read = sqlite.symbol_read(),
                                 .symbols_write = sqlite.symbol_write(),
                                 .types_write = sqlite.type_write(),
                                 .facts_write = sqlite.fact_write(),
                                 .definitions_write = sqlite.definition_write(),
                                 .unit_of_work = sqlite.unit_of_work()};
  ast::StorageSymbolSink symbols(ports);
  ast::StorageEdgeSink edges(ports);
  auto transaction = ports.unit_of_work.begin();
  const auto header_file = ports.source.get_file(header.file.portable_path());
  const auto main_file = ports.source.get_file(main.file.portable_path());
  if (!header_file || !main_file) {
    throw std::logic_error("direct fixture files were not preplanned");
  }
  const std::int64_t header_id = header_file->id;
  const std::int64_t main_id = main_file->id;
  symbols.set_identity_translation_unit_file_id(main_id);
  symbols.set_current_file_id(header_id);
  symbols.emit(symbol(header, "usr-shared", "first"));
  const std::int64_t shared = symbols.symbol_ids(header_id).back();
  symbols.set_current_file_id(main_id);
  symbols.emit(symbol(main, "usr-shared", "last"));
  symbols.emit(symbol(main, "usr-target", "target"));
  const std::int64_t target = symbols.symbol_ids(main_id).back();
  edges.set_current_file_id(main_id);
  static_cast<void>(edges.add_edge({.src_id = shared,
                                    .dst_id = target,
                                    .kind = 1,
                                    .count = 1,
                                    .base_access = std::nullopt,
                                    .is_virtual = std::nullopt}));
  static_cast<void>(edges.get_or_create_definition(target, main_id, 4, 2, 4, 8,
                                                   std::nullopt));
  transaction->commit();
}

auto replay_batch(const ast::FactPartitionKey &header,
                  const ast::FactPartitionKey &main) -> ast::FactBatch {
  ast::FactBatchRecorder recorder("storage-replay-oracle");
  recorder.set_partition(header, 101);
  recorder.emit(symbol(header, "usr-shared", "first"));
  const auto shared = recorder.lookup_symbol_id(
      "usr-shared", header.configuration.identity_source);
  recorder.set_partition(main, 102);
  recorder.emit(symbol(main, "usr-shared", "last"));
  recorder.emit(symbol(main, "usr-target", "target"));
  const auto target = recorder.lookup_symbol_id(
      "usr-target", main.configuration.identity_source);
  if (!shared || !target) {
    throw std::logic_error("replay fixture symbols were not indexed");
  }
  static_cast<void>(recorder.add_edge({.src_id = shared.value(),
                                       .dst_id = target.value(),
                                       .kind = 1,
                                       .count = 1,
                                       .base_access = std::nullopt,
                                       .is_virtual = std::nullopt}));
  static_cast<void>(recorder.get_or_create_definition(target.value(), 102, 4, 2,
                                                      4, 8, std::nullopt));
  return recorder.canonical_batch();
}

} // namespace

TEST_CASE("storage-backed replay matches the direct sink and legacy order") {
  const auto header =
      partition("header.hpp", "/tmp/cidx-s072-replay/header.hpp");
  const auto main = partition("main.cpp", "/tmp/cidx-s072-replay/main.cpp");
  Storage direct(":memory:");
  Storage replayed(":memory:");
  prepare(direct);
  prepare(replayed);
  apply_direct(direct, header, main);

  StorageReplayPort replay_port(replayed);
  const auto replay_result =
      application::replay_fact_batch(replay_batch(header, main), replay_port);
  INFO(replay_result.error.value_or(""));
  REQUIRE(replay_result.committed);
  CHECK(normalized(replayed) == normalized(direct));
}
