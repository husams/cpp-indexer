#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <algorithm>

#include "catalogs/generated_catalog.hpp"
#include "catalogs/generated_extensions.hpp"
#include "query/plan.hpp"

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
