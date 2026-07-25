#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
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

TEST_CASE("symbol sink checks every merge-sensitive definition field") {
  struct Mutation {
    const char *name;
    std::function<void(cidx::ast::SymbolRecord &)> apply;
    std::function<void(const cidx::Symbol &)> verify;
    std::function<void(cidx::ast::SymbolRecord &)> prepare_seed;
  };
  const std::vector<Mutation> mutations = {
      {"spelling",
       [](auto &symbol) { symbol.spelling = "changed_spelling"; },
       [](const auto &symbol) { CHECK(symbol.spelling == "changed_spelling"); },
       {}},
      {"qual_name",
       [](auto &symbol) { symbol.qual_name = "changed::qual_name"; },
       [](const auto &symbol) {
         CHECK(symbol.qual_name ==
               std::optional<std::string>{"changed::qual_name"});
       },
       {}},
      {"display_name",
       [](auto &symbol) { symbol.display_name = "changed_display"; },
       [](const auto &symbol) {
         CHECK(symbol.display_name ==
               std::optional<std::string>{"changed_display"});
       },
       {}},
      {"type_info",
       [](auto &symbol) { symbol.type_info = "changed_type"; },
       [](const auto &symbol) {
         CHECK(symbol.type_info == std::optional<std::string>{"changed_type"});
       },
       {}},
      {"kind",
       [](auto &symbol) { symbol.kind = 9; },
       [](const auto &symbol) { CHECK(symbol.kind == "variable"); },
       {}},
      {"is_pure",
       [](auto &symbol) { symbol.is_pure = true; },
       [](const auto &symbol) { CHECK(symbol.is_pure); },
       {}},
      {"is_static",
       [](auto &symbol) { symbol.is_static = true; },
       [](const auto &symbol) { CHECK(symbol.is_static); },
       {}},
      {"is_instantiation_downgrade",
       [](auto &symbol) { symbol.is_instantiation = false; },
       [](const auto &symbol) { CHECK_FALSE(symbol.is_instantiation); },
       [](auto &symbol) { symbol.is_instantiation = true; }},
      {"linkage",
       [](auto &symbol) { symbol.linkage = "internal"; },
       [](const auto &symbol) {
         CHECK(symbol.linkage == std::optional<std::string>{"internal"});
       },
       {}},
      {"access",
       [](auto &symbol) { symbol.access = "private"; },
       [](const auto &symbol) {
         CHECK(symbol.access == std::optional<std::string>{"private"});
       },
       {}},
      {"parent_usr",
       [](auto &symbol) { symbol.parent_usr = "changed_parent"; },
       [](const auto &symbol) {
         CHECK(symbol.parent_usr ==
               std::optional<std::string>{"changed_parent"});
       },
       {}},
      {"const_value",
       [](auto &symbol) { symbol.const_value = "42"; },
       [](const auto &symbol) {
         CHECK(symbol.const_value == std::optional<std::string>{"42"});
       },
       {}}};

  for (const auto &mutation : mutations) {
    SinkFixture fixture;
    cidx::ast::StorageSymbolSink sink(fixture.ports);
    sink.set_current_file_id(fixture.first_file);
    auto seed =
        fixture.symbol("sink:@F@merge_sensitive", true, fixture.first_file);
    if (mutation.prepare_seed) {
      mutation.prepare_seed(seed);
    }
    sink.emit(seed);
    sink.emit(seed); // Resolve and populate the cache.
    auto changed = seed;
    mutation.apply(changed);
    sink.emit(changed);

    CHECK(fixture.symbol_read.lookup_calls == 3);
    const bool creates_new_identity =
        std::string_view(mutation.name) == "linkage";
    REQUIRE(sink.symbol_ids().size() == (creates_new_identity ? 2 : 1));
    const auto persisted =
        fixture.db.lookup_symbol_by_id(sink.symbol_ids().back());
    REQUIRE(persisted.has_value());
    mutation.verify(*persisted);
  }
}

TEST_CASE("symbol sink checks merge-sensitive declaration fields and parents") {
  struct Mutation {
    const char *name;
    std::function<void(cidx::ast::SymbolRecord &)> apply;
    std::function<void(const cidx::Symbol &)> verify;
    std::function<void(cidx::ast::SymbolRecord &)> prepare_seed;
  };
  const std::vector<Mutation> mutations = {
      {"display_name",
       [](auto &symbol) { symbol.display_name = "decl_display"; },
       [](const auto &symbol) {
         CHECK(symbol.display_name ==
               std::optional<std::string>{"decl_display"});
       },
       {}},
      {"type_info",
       [](auto &symbol) { symbol.type_info = "decl_type"; },
       [](const auto &symbol) {
         CHECK(symbol.type_info == std::optional<std::string>{"decl_type"});
       },
       {}},
      {"is_pure",
       [](auto &symbol) { symbol.is_pure = true; },
       [](const auto &symbol) { CHECK(symbol.is_pure); },
       {}},
      {"is_static",
       [](auto &symbol) { symbol.is_static = true; },
       [](const auto &symbol) { CHECK(symbol.is_static); },
       {}},
      {"is_instantiation_downgrade",
       [](auto &symbol) { symbol.is_instantiation = false; },
       [](const auto &symbol) { CHECK_FALSE(symbol.is_instantiation); },
       [](auto &symbol) { symbol.is_instantiation = true; }},
      {"linkage",
       [](auto &symbol) { symbol.linkage = "internal"; },
       [](const auto &symbol) {
         CHECK(symbol.linkage == std::optional<std::string>{"internal"});
       },
       {}},
      {"access",
       [](auto &symbol) { symbol.access = "protected"; },
       [](const auto &symbol) {
         CHECK(symbol.access == std::optional<std::string>{"protected"});
       },
       {}},
      {"parent_usr",
       [](auto &symbol) { symbol.parent_usr = "decl_parent"; },
       [](const auto &symbol) {
         CHECK(symbol.parent_usr == std::optional<std::string>{"decl_parent"});
       },
       {}},
      {"const_value",
       [](auto &symbol) { symbol.const_value = "decl_value"; },
       [](const auto &symbol) {
         CHECK(symbol.const_value == std::optional<std::string>{"decl_value"});
       },
       {}}};

  for (const auto &mutation : mutations) {
    SinkFixture fixture;
    cidx::ast::StorageSymbolSink sink(fixture.ports);
    sink.set_current_file_id(fixture.first_file);
    auto seed =
        fixture.symbol("sink:@F@declaration_merge", true, fixture.first_file);
    if (mutation.prepare_seed) {
      mutation.prepare_seed(seed);
    }
    sink.emit(seed);
    sink.emit(seed); // Resolve and populate the cache.
    auto declaration = seed;
    declaration.is_definition = false;
    declaration.line = 20;
    declaration.col = 2;
    declaration.end_line = 20;
    declaration.end_col = 12;
    declaration.decl_line = 20;
    declaration.decl_col = 2;
    mutation.apply(declaration);
    sink.emit(declaration);

    CHECK(fixture.symbol_read.lookup_calls == 3);
    const bool creates_new_identity =
        std::string_view(mutation.name) == "linkage";
    REQUIRE(sink.symbol_ids().size() == (creates_new_identity ? 2 : 1));
    const auto persisted =
        fixture.db.lookup_symbol_by_id(sink.symbol_ids().back());
    REQUIRE(persisted.has_value());
    mutation.verify(*persisted);
  }
}

TEST_CASE("symbol sink direct declaration path reconciles parents") {
  SinkFixture fixture;
  cidx::ast::StorageSymbolSink sink(fixture.ports);
  sink.set_current_file_id(fixture.first_file);
  auto child =
      fixture.symbol("sink:@F@waiting_child", true, fixture.first_file);
  child.parent_usr = "sink:@N@late_parent";
  sink.emit(child);
  sink.emit(child); // Resolve and populate the cache before the parent exists.

  cidx::Symbol parent;
  parent.usr = "sink:@N@late_parent";
  parent.spelling = "late_parent";
  parent.kind = "namespace";
  parent.file_id = fixture.first_file;
  parent.line = 1;
  parent.is_definition = true;
  parent.resolved = true;
  const int64_t parent_id = fixture.db.add_symbol(parent);

  sink.emit(child); // Must remain on the fast path and reconcile parent_id.
  CHECK(fixture.symbol_read.lookup_calls == 2);
  REQUIRE(sink.symbol_ids().size() == 1);
  auto parent_lookup =
      fixture.db.raw_db().prepare("SELECT parent_id FROM symbol WHERE id = ?");
  parent_lookup.bind(1, sink.symbol_ids().front());
  REQUIRE(parent_lookup.step());
  CHECK(parent_lookup.col_int64(0) == parent_id);
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

TEST_CASE("sink ID collections preserve order across the 32-entry transition") {
  SinkFixture fixture;
  cidx::ast::StorageSymbolSink symbols(fixture.ports);
  symbols.set_current_file_id(fixture.first_file);

  std::vector<cidx::ast::SymbolRecord> symbol_records;
  std::vector<int64_t> persisted_symbol_ids;
  for (int index = 0; index < 64; ++index) {
    symbol_records.push_back(
        fixture.symbol("sink:@F@threshold_" + std::to_string(index), true,
                       fixture.first_file));
  }
  for (const auto &record : symbol_records) {
    symbols.emit(record);
    persisted_symbol_ids.push_back(fixture.db.lookup_symbol(record.usr)->id);
  }

  for (int index = 0; index < 31; ++index) {
    symbols.emit(symbol_records[index]);
  }
  symbols.emit(symbol_records[0]);
  symbols.emit(symbol_records[31]);
  symbols.emit(symbol_records[31]);
  symbols.emit(symbol_records[32]);
  symbols.emit(symbol_records[0]);
  symbols.emit(symbol_records[32]);
  for (int index = 33; index < 64; ++index) {
    symbols.emit(symbol_records[index]);
  }
  CIDX_REQUIRE_BOOL(symbols.symbol_ids() == persisted_symbol_ids);
  CIDX_CHECK_BOOL(fixture.db.stats().symbols == 64);

  symbols.reset_counters();
  for (int index = 32; index >= 0; --index) {
    symbols.emit(symbol_records[index]);
  }
  symbols.emit(symbol_records[32]);
  symbols.emit(symbol_records[0]);
  CIDX_REQUIRE_BOOL(symbols.symbol_ids().size() == 33);
  CIDX_CHECK_BOOL(symbols.symbol_ids().front() == persisted_symbol_ids[32]);
  CIDX_CHECK_BOOL(symbols.symbol_ids().back() == persisted_symbol_ids[0]);
  CIDX_CHECK_BOOL(fixture.db.stats().symbols == 64);

  std::vector<int64_t> edge_symbol_ids;
  for (int index = 0; index < 128; ++index) {
    edge_symbol_ids.push_back(fixture.db.add_symbol(SinkFixture::stored_symbol(
        "sink:@F@edge_threshold_" + std::to_string(index),
        fixture.first_file)));
  }
  cidx::ast::StorageEdgeSink edges(fixture.ports);
  std::vector<cidx::ast::EdgeRecord> edge_records;
  for (int index = 0; index < 64; ++index) {
    edge_records.push_back({.src_id = edge_symbol_ids[index * 2],
                            .dst_id = edge_symbol_ids[index * 2 + 1],
                            .kind = 1,
                            .count = 1,
                            .base_access = std::nullopt,
                            .is_virtual = std::nullopt});
  }
  std::vector<int64_t> expected_edge_ids;
  auto record_edge = [&](int index) {
    const int64_t id = edges.ensure_edge(edge_records[index]);
    if (std::ranges::find(expected_edge_ids, id) == expected_edge_ids.end()) {
      expected_edge_ids.push_back(id);
    }
  };
  for (int index = 0; index < 31; ++index) {
    record_edge(index);
  }
  record_edge(0);
  record_edge(31);
  record_edge(31);
  record_edge(32);
  record_edge(0);
  record_edge(32);
  for (int index = 33; index < 64; ++index) {
    record_edge(index);
  }
  CIDX_REQUIRE_BOOL(edges.edge_ids() == expected_edge_ids);
  CIDX_CHECK_BOOL(fixture.db.stats().edges == 64);

  std::vector<int64_t> expected_definition_ids;
  auto record_definition = [&](int index) {
    const int64_t id = edges.get_or_create_definition(
        edge_symbol_ids[index], fixture.first_file, index + 1, 1, index + 1, 10,
        std::nullopt);
    if (std::ranges::find(expected_definition_ids, id) ==
        expected_definition_ids.end()) {
      expected_definition_ids.push_back(id);
    }
  };
  for (int index = 0; index < 31; ++index) {
    record_definition(index);
  }
  record_definition(0);
  record_definition(31);
  record_definition(31);
  record_definition(32);
  record_definition(0);
  record_definition(32);
  for (int index = 33; index < 64; ++index) {
    record_definition(index);
  }
  CIDX_REQUIRE_BOOL(edges.definition_ids() == expected_definition_ids);

  edges.reset_fact_ids();
  for (int index = 32; index >= 0; --index) {
    record_edge(index);
    record_definition(index);
  }
  record_edge(32);
  record_definition(32);
  CIDX_REQUIRE_BOOL(edges.edge_ids().size() == 33);
  CIDX_REQUIRE_BOOL(edges.definition_ids().size() == 33);
  CIDX_CHECK_BOOL(edges.edge_ids().front() == expected_edge_ids[32]);
  CIDX_CHECK_BOOL(edges.definition_ids().front() ==
                  expected_definition_ids[32]);
  CIDX_CHECK_BOOL(fixture.db.stats().edges == 64);

  int persisted_definitions = 0;
  for (int index = 0; index < 64; ++index) {
    persisted_definitions += static_cast<int>(
        fixture.db.definitions_of(edge_symbol_ids[index]).size());
  }
  CIDX_CHECK_BOOL(persisted_definitions == 64);
}
