#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <algorithm>

#include "catalogs/generated_catalog.hpp"

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
           relation.inverse == "called_by" &&
           relation.completeness == "partial";
  }));
}
