#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <algorithm>
#include <string>
#include <tuple>
#include <vector>

#include "catalogs/generated_catalog.hpp"
#include "catalogs/generated_extensions.hpp"
#include "query/plan.hpp"
#include "storage/records.hpp"
#include "storage/storage.hpp"

TEST_CASE("generated semantic catalog has stable cross-language contract") {
  using namespace cidx::catalog;

  CHECK(kCatalogVersion == 1);
  CHECK(kCatalogHash.size() == 64);
  CHECK(kSymbolKinds.size() == 17);
  CHECK(kRelations.size() == 32);
  CHECK(kFields.size() == 13);
  CHECK(kOccurrenceRoles.size() == 5);
  CHECK(kEffectRoles.size() == 5);
  CHECK(std::ranges::any_of(kRelations, [](const Relation &relation) {
    return relation.name == "calls" && relation.layer == View::Symbol &&
           relation.source == "symbol.callable" &&
           relation.target == "symbol.callable" &&
           relation.inverse == "called_by" && relation.traversal == "out|in" &&
           relation.evidence_capabilities == "call_site|declaration" &&
           relation.completeness == "partial";
  }));
  REQUIRE(kExtensionCatalogHash == kCatalogHash);
  REQUIRE(kExtensionRelations.size() == 1);
  CHECK(kExtensionRelations.front().qualified_name ==
        "test.extension/relation/taints");
  CHECK(kExtensionRelations.front().source == "symbol.declaration");
  CHECK(kExtensionRelations.front().target == "symbol.declaration");
  CHECK(kExtensionRelations.front().traversal == "out|in");
  CHECK(kExtensionRelations.front().evidence_capabilities == "derived|proof");
  CHECK(cidx::query::extension_relation_catalog().front().name == "taints");
}

TEST_CASE("template relation metadata matches stored graph direction") {
  using namespace cidx;

  Storage db(":memory:");
  auto add_symbol = [&db](const std::string &usr, const std::string &name,
                          const std::string &kind) {
    Symbol symbol;
    symbol.usr = usr;
    symbol.spelling = name;
    symbol.kind = kind;
    symbol.is_definition = true;
    symbol.resolved = true;
    return db.add_symbol(symbol);
  };
  const auto class_template =
      add_symbol("u:class-template", "Box", "class-template");
  const auto function_template =
      add_symbol("u:function-template", "make", "function-template");
  const auto structure = add_symbol("u:struct", "Box<int>", "struct");
  const auto function = add_symbol("u:function", "make<int>", "function");
  const auto method = add_symbol("u:method", "Box<int>::run", "method");
  const auto constructor =
      add_symbol("u:constructor", "Box<int>::Box", "constructor");

  auto add_edge = [&db](int64_t source, int64_t target, int64_t kind) {
    Edge edge;
    edge.src_id = source;
    edge.dst_id = target;
    edge.kind = kind;
    db.add_edge(edge);
  };
  add_edge(structure, class_template, 4);
  add_edge(function, function_template, 5);
  add_edge(method, function_template, 5);
  add_edge(constructor, function_template, 5);

  auto statement =
      db.raw_db().prepare("SELECT e.kind, src.kind, dst.kind FROM edge e "
                          "JOIN symbol src ON src.id = e.src_id "
                          "JOIN symbol dst ON dst.id = e.dst_id "
                          "WHERE e.kind IN (4, 5) ORDER BY e.kind, e.id");
  std::vector<std::tuple<int64_t, int64_t, int64_t>> rows;
  while (statement.step()) {
    rows.emplace_back(statement.col_int64(0), statement.col_int64(1),
                      statement.col_int64(2));
  }
  CHECK(rows == std::vector<std::tuple<int64_t, int64_t, int64_t>>{
                    {4, 2, 31}, {5, 8, 30}, {5, 21, 30}, {5, 24, 30}});

  const auto *specializes =
      cidx::query::resolve_relation("specializes", cidx::query::View::Symbol);
  const auto *instantiates =
      cidx::query::resolve_relation("instantiates", cidx::query::View::Symbol);
  REQUIRE(specializes != nullptr);
  REQUIRE(instantiates != nullptr);
  CHECK(specializes->source == "symbol.declaration");
  CHECK(specializes->target == "symbol.template");
  CHECK(instantiates->source == "symbol.declaration");
  CHECK(instantiates->target == "symbol.template");
}
