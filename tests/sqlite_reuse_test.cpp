#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "storage/sqlite.hpp"
#include "util/errors.hpp"

#include <cstdint>
#include <string>

namespace {

using namespace cidx;

auto sum(SqliteDb &database) -> std::int64_t {
  auto query =
      database.prepare("SELECT COALESCE(SUM(value),0) FROM values_test");
  REQUIRE(query.step());
  return query.col_int64(0);
}

TEST_CASE("one prepared statement supports 10000 clear rebind cycles") {
  // Per-step VM-step sampling is opt-in (it costs two clock reads and three
  // sqlite3_stmt_status calls per step), so a test that asserts on it must
  // open the measurement scope.
  const StatementMeasurementScope measuring;
  SqliteDb reusable(":memory:");
  reusable.exec("CREATE TABLE values_test(value INTEGER NOT NULL)");
  auto insert = reusable.prepare("INSERT INTO values_test(value) VALUES(?)");
  for (std::int64_t value = 0; value < 10'000; ++value) {
    insert.bind(1, value);
    insert.step_done();
    if (value + 1 != 10'000) {
      insert.reset();
    }
  }
  CHECK(insert.stats().executions == 10'000);
  CHECK(insert.stats().reuses == 9'999);
  CHECK(insert.stats().virtual_machine_steps > 0);

  SqliteDb one_shot(":memory:");
  one_shot.exec("CREATE TABLE values_test(value INTEGER NOT NULL)");
  for (std::int64_t value = 0; value < 10'000; ++value) {
    auto statement =
        one_shot.prepare("INSERT INTO values_test(value) VALUES(?)");
    statement.bind(1, value);
    statement.step_done();
  }
  CHECK(sum(reusable) == sum(one_shot));
}

TEST_CASE("statement failures retain SQL template context") {
  SqliteDb database(":memory:");
  database.exec("CREATE TABLE unique_value(value INTEGER UNIQUE)");
  auto insert = database.prepare("INSERT INTO unique_value(value) VALUES(?)");
  insert.bind(1, std::int64_t{7});
  insert.step_done();
  insert.reset();
  insert.bind(1, std::int64_t{7});
  CHECK_THROWS_WITH_AS(
      insert.step_done(),
      "step for \"INSERT INTO unique_value(value) VALUES(?)\": UNIQUE "
      "constraint failed: unique_value.value",
      StorageError);
}

} // namespace
