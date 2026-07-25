#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ast/edge_records.hpp"
#include "ast/storage_edge_sink.hpp"
#include "ast/storage_symbol_sink.hpp"
#include "ast/symbol_record.hpp"
#include "storage/ports.hpp"
#include "storage/sqlite_adapters.hpp"
#include "storage/storage.hpp"

namespace {

#define CIDX_CHECK_BOOL(expression)                                            \
  do {                                                                         \
    const bool result = (expression);                                          \
    CHECK(result);                                                             \
  } while (false)

#define CIDX_REQUIRE_BOOL(expression)                                          \
  do {                                                                         \
    const bool result = (expression);                                          \
    REQUIRE(result);                                                           \
  } while (false)

class CountingSymbolReadPort final : public cidx::storage::SymbolReadPort {
public:
  explicit CountingSymbolReadPort(cidx::storage::SymbolReadPort &delegate)
      : delegate_(delegate) {}

  std::optional<cidx::Symbol> lookup_symbol(
      const std::string &usr,
      const std::optional<int64_t> &semantic_universe_id = std::nullopt,
      const std::optional<std::string> &identity_source = std::nullopt,
      const std::optional<std::string> &identity_translation_unit =
          std::nullopt) override {
    ++lookup_calls;
    return delegate_.lookup_symbol(usr, semantic_universe_id, identity_source,
                                   identity_translation_unit);
  }

  std::optional<cidx::Symbol> lookup_symbol_by_id(int64_t id) override {
    return delegate_.lookup_symbol_by_id(id);
  }

  std::vector<cidx::Symbol>
  lookup_symbols_by_usr(const std::string &usr,
                        const std::optional<int64_t> &semantic_universe_id =
                            std::nullopt) override {
    return delegate_.lookup_symbols_by_usr(usr, semantic_universe_id);
  }

  std::vector<cidx::Symbol>
  lookup_symbols_by_name(const std::string &name,
                         const std::optional<std::string> &kind = std::nullopt,
                         const std::optional<int64_t> &semantic_universe_id =
                             std::nullopt) override {
    return delegate_.lookup_symbols_by_name(name, kind, semantic_universe_id);
  }

  std::vector<cidx::Symbol> lookup_symbols_by_qual_name(
      const std::string &name,
      const std::optional<std::string> &kind = std::nullopt,
      const std::optional<int64_t> &semantic_universe_id =
          std::nullopt) override {
    return delegate_.lookup_symbols_by_qual_name(name, kind,
                                                 semantic_universe_id);
  }

  std::vector<cidx::Symbol> symbols_in_file(int64_t file_id) override {
    return delegate_.symbols_in_file(file_id);
  }

  int lookup_calls = 0;

private:
  cidx::storage::SymbolReadPort &delegate_;
};

struct SinkFixture {
  cidx::Storage db{":memory:"};
  cidx::storage::SqliteStoragePorts sqlite_ports{db};
  CountingSymbolReadPort symbol_read{sqlite_ports.symbol_read()};
  cidx::storage::AstStoragePorts ports{
      .workspace = sqlite_ports.workspace_catalog_read(),
      .source = sqlite_ports.source_read(),
      .symbols_read = symbol_read,
      .symbols_write = sqlite_ports.symbol_write(),
      .types_write = sqlite_ports.type_write(),
      .facts_write = sqlite_ports.fact_write(),
      .definitions_write = sqlite_ports.definition_write(),
      .unit_of_work = sqlite_ports.unit_of_work()};

  const int64_t component = db.add_component("sink-test", "/tmp/hse95-sink");
  const int64_t first_file = db.add_file_path("/tmp/hse95-sink/first.cpp");
  const int64_t second_file = db.add_file_path("/tmp/hse95-sink/second.cpp");

  [[nodiscard]] cidx::ast::SymbolRecord symbol(std::string usr, bool resolved,
                                               int64_t file_id) const {
    cidx::ast::SymbolRecord result;
    result.file = file_id == first_file ? "/tmp/hse95-sink/first.cpp"
                                        : "/tmp/hse95-sink/second.cpp";
    result.usr = std::move(usr);
    result.spelling = "sink_symbol";
    result.kind = 8; // CXCursor_FunctionDecl
    result.line = 1;
    result.col = 1;
    result.end_line = 1;
    result.end_col = 20;
    result.is_definition = true;
    result.resolved = resolved;
    return result;
  }

  [[nodiscard]] static cidx::Symbol stored_symbol(const std::string &usr,
                                                  int64_t file_id) {
    cidx::Symbol result;
    result.usr = usr;
    result.spelling = "edge_symbol";
    result.kind = "function";
    result.file_id = file_id;
    result.is_definition = true;
    result.resolved = true;
    return result;
  }
};

} // namespace

TEST_CASE("symbol sink caches resolved identities and invalidates state") {
  SinkFixture fixture;
  cidx::ast::StorageSymbolSink sink(fixture.ports);
  sink.set_current_file_id(fixture.first_file);

  const auto resolved =
      fixture.symbol("sink:@F@resolved", true, fixture.first_file);
  sink.emit(resolved);
  sink.emit(resolved);
  sink.emit(resolved);
  CIDX_CHECK_BOOL(fixture.symbol_read.lookup_calls == 2);
  CIDX_CHECK_BOOL(sink.stored_count() == 1);
  CIDX_REQUIRE_BOOL(sink.symbol_ids().size() == 1);

  sink.set_current_file_id(fixture.first_file);
  sink.emit(resolved);
  CIDX_CHECK_BOOL(fixture.symbol_read.lookup_calls == 3);
  CIDX_CHECK_BOOL(sink.stored_count() == 1);

  sink.reset_counters();
  sink.emit(resolved);
  CIDX_CHECK_BOOL(fixture.symbol_read.lookup_calls == 4);
  CIDX_CHECK_BOOL(sink.stored_count() == 0);

  cidx::TranslationUnitConfig first_config;
  first_config.driver = "clang++";
  first_config.language = "c++";
  first_config.arguments = {"-std=c++23", "-DFIRST=1"};
  cidx::TranslationUnitConfig second_config = first_config;
  second_config.arguments = {"-std=c++23", "-DSECOND=1"};
  const int64_t first_config_id =
      fixture.db.add_translation_unit_config(first_config);
  const int64_t second_config_id =
      fixture.db.add_translation_unit_config(second_config);
  sink.set_identity_translation_unit_config_id(first_config_id,
                                               fixture.first_file);
  sink.emit(resolved);
  sink.emit(resolved);
  CIDX_CHECK_BOOL(fixture.symbol_read.lookup_calls == 5);
  sink.set_identity_translation_unit_config_id(second_config_id,
                                               fixture.first_file);
  sink.emit(resolved);
  CIDX_CHECK_BOOL(fixture.symbol_read.lookup_calls == 6);

  const auto unresolved =
      fixture.symbol("sink:@F@unresolved", false, fixture.first_file);
  sink.emit(unresolved);
  sink.emit(unresolved);
  CIDX_CHECK_BOOL(fixture.symbol_read.lookup_calls == 8);
  CIDX_CHECK_BOOL(sink.stored_count() == 2);
}

TEST_CASE("symbol sink preserves first-seen IDs while suppressing duplicates") {
  SinkFixture fixture;
  cidx::ast::StorageSymbolSink sink(fixture.ports);
  sink.set_current_file_id(fixture.first_file);

  const auto first = fixture.symbol("sink:@F@first", true, fixture.first_file);
  const auto second =
      fixture.symbol("sink:@F@second", true, fixture.first_file);
  sink.emit(first);
  sink.emit(second);
  sink.emit(first);

  CIDX_REQUIRE_BOOL(sink.symbol_ids().size() == 2);
  CIDX_CHECK_BOOL(sink.symbol_ids()[0] < sink.symbol_ids()[1]);
  CIDX_CHECK_BOOL(fixture.db.stats().symbols == 2);
  CIDX_CHECK_BOOL(fixture.db.symbols_in_file(fixture.first_file).size() == 2);
}

TEST_CASE("edge sink caches lookups and tracks unique facts in visit order") {
  SinkFixture fixture;
  const int64_t first_id = fixture.db.add_symbol(
      SinkFixture::stored_symbol("sink:@F@edge_first", fixture.first_file));
  const int64_t second_id = fixture.db.add_symbol(
      SinkFixture::stored_symbol("sink:@F@edge_second", fixture.second_file));

  cidx::ast::StorageEdgeSink sink(fixture.ports);
  sink.set_current_file_id(fixture.first_file);
  const auto first_lookup =
      sink.lookup_symbol_id("sink:@F@edge_first", "/tmp/hse95-sink/first.cpp");
  REQUIRE(first_lookup.has_value());
  CIDX_CHECK_BOOL(first_lookup.value_or(-1) == first_id);
  const auto cached_lookup =
      sink.lookup_symbol_id("sink:@F@edge_first", "/tmp/hse95-sink/first.cpp");
  REQUIRE(cached_lookup.has_value());
  CIDX_CHECK_BOOL(cached_lookup.value_or(-1) == first_id);
  CIDX_CHECK_BOOL(fixture.symbol_read.lookup_calls == 1);
  sink.set_current_file_id(fixture.first_file);
  const auto invalidated_lookup =
      sink.lookup_symbol_id("sink:@F@edge_first", "/tmp/hse95-sink/first.cpp");
  REQUIRE(invalidated_lookup.has_value());
  CIDX_CHECK_BOOL(invalidated_lookup.value_or(-1) == first_id);
  CIDX_CHECK_BOOL(fixture.symbol_read.lookup_calls == 2);

  const cidx::ast::EdgeRecord first_edge{.src_id = first_id,
                                         .dst_id = second_id,
                                         .kind = 1,
                                         .count = 1,
                                         .base_access = std::nullopt,
                                         .is_virtual = std::nullopt};
  const cidx::ast::EdgeRecord second_edge{.src_id = second_id,
                                          .dst_id = first_id,
                                          .kind = 2,
                                          .count = 1,
                                          .base_access = std::nullopt,
                                          .is_virtual = std::nullopt};
  const int64_t first_edge_id = sink.add_edge(first_edge);
  CIDX_CHECK_BOOL(sink.add_edge(first_edge) == first_edge_id);
  const int64_t second_edge_id = sink.ensure_edge(second_edge);
  CIDX_CHECK_BOOL(sink.ensure_edge(second_edge) == second_edge_id);
  CIDX_REQUIRE_BOOL(sink.edge_ids().size() == 2);
  CIDX_CHECK_BOOL(sink.edge_ids()[0] == first_edge_id);
  CIDX_CHECK_BOOL(sink.edge_ids()[1] == second_edge_id);

  const int64_t first_definition_id = sink.get_or_create_definition(
      first_id, fixture.first_file, 1, 1, 1, 10, std::nullopt);
  CIDX_CHECK_BOOL(sink.get_or_create_definition(first_id, fixture.first_file, 1,
                                                1, 1, 10, std::nullopt) ==
                  first_definition_id);
  const int64_t second_definition_id = sink.get_or_create_definition(
      second_id, fixture.second_file, 2, 1, 2, 10, std::nullopt);
  CIDX_REQUIRE_BOOL(sink.definition_ids().size() == 2);
  CIDX_CHECK_BOOL(sink.definition_ids()[0] == first_definition_id);
  CIDX_CHECK_BOOL(sink.definition_ids()[1] == second_definition_id);
  CIDX_CHECK_BOOL(fixture.db.stats().edges == 2);
  CIDX_CHECK_BOOL(
      fixture.db.graph_edges(first_id, "out", {1, 2}, true, 10).size() == 1);
  CIDX_CHECK_BOOL(fixture.db.definitions_of(first_id).size() == 1);

  sink.reset_fact_ids();
  CIDX_CHECK_BOOL(sink.edge_ids().empty());
  CIDX_CHECK_BOOL(sink.definition_ids().empty());
  sink.add_edge(second_edge);
  sink.get_or_create_definition(second_id, fixture.second_file, 2, 1, 2, 10,
                                std::nullopt);
  CIDX_REQUIRE_BOOL(sink.edge_ids().size() == 1);
  CIDX_REQUIRE_BOOL(sink.definition_ids().size() == 1);
  CIDX_CHECK_BOOL(sink.edge_ids().front() == second_edge_id);
  CIDX_CHECK_BOOL(sink.definition_ids().front() == second_definition_id);
  CIDX_CHECK_BOOL(fixture.db.stats().edges == 2);
  CIDX_CHECK_BOOL(fixture.db.definitions_of(second_id).size() == 1);
}
