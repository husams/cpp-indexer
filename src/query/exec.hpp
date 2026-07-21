// query/exec.hpp -- SQLite executor for validated CXQ plans.
//
// Contract: docs/query-plan.md (v1). Read-only, parameterized SQL against the
// index database via an existing Storage handle. Deterministic: the node
// stream is kept ordered ascending by id after every stage; budgets surface
// as `truncated`, never as a silently complete result.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "cli/json_out.hpp"
#include "query/plan.hpp"
#include "storage/storage.hpp"

namespace cidx::query {

// Execution budgets (docs/query-plan.md "Execution semantics").
constexpr int64_t kTraverseNodeBudget = 10000;
constexpr int64_t kEnumerateBudget = 10000;
constexpr int64_t kDefaultResultCap = 1000;
constexpr size_t kIdChunk = 400;

// One result cell: null, integer, or text.
using Cell = std::variant<std::nullptr_t, int64_t, std::string>;

struct Result {
  Shape shape = Shape::Nodes;
  View view = View::Symbol;
  bool truncated = false;
  int64_t scalar = 0;                       // Shape::Scalar only
  std::vector<std::string> fields;          // row column names, select order
  std::vector<std::vector<Cell>> rows;      // Shape::Nodes/Rows

  // {"shape","view","count","truncated","rows"} -- see docs/query-plan.md.
  [[nodiscard]] json_out::Value to_json() const;
};

class Executor {
public:
  explicit Executor(Storage &db) : db_(db) {}

  // Validate + normalize + run. Throws PlanError on an invalid plan.
  Result run(const Plan &plan);

private:
  Storage &db_;
};

} // namespace cidx::query
