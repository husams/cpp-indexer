// queryplan_example — using the CXQ QueryPlan DSL from C++ (src/query/).
//
// Contract: docs/query-plan.md (v1). Guide: docs/query-dsl.md. This is the
// C++ twin of python/examples/08_queryplan_basics.py / 09_queryplan_advanced.py:
// the pipeline builder in src/query/plan.hpp produces the same QueryPlan IR
// as the Python builder (byte-identical canonical JSON), and the executor in
// src/query/exec.hpp runs it read-only over an index.db.
//
// Build (from the repo root):
//   cmake -S . -B build -DCIDX_BUILD_EXAMPLES=ON && cmake --build build -j
//   ./build/cidx-queryplan-example [path/to/index.db]   # default: ./index.db
//
// The default argument targets the repo's checked-in self-index, so the
// output below is the cidx codebase describing itself.

#include <cstdlib>
#include <iostream>
#include <string>

#include "cli/json_out.hpp"
#include "query/exec.hpp"
#include "query/plan.hpp"
#include "storage/storage.hpp"

using namespace cidx::query;
namespace json_out = cidx::json_out;

namespace {

// Result cells are std::variant<nullptr_t, int64_t, std::string>.
std::string cell_text(const Cell &c) {
  if (std::holds_alternative<std::nullptr_t>(c)) {
    return "-";
  }
  if (std::holds_alternative<int64_t>(c)) {
    return std::to_string(std::get<int64_t>(c));
  }
  return std::get<std::string>(c);
}

const char *shape_name(Shape s) {
  switch (s) {
  case Shape::Scalar:
    return "scalar";
  case Shape::Rows:
    return "rows";
  case Shape::Nodes:
    break;
  }
  return "nodes";
}

void print_rows(const Result &r, size_t max_rows = 10) {
  std::cout << "  shape=" << shape_name(r.shape)
            << " view=" << view_name(r.view)
            << " truncated=" << (r.truncated ? "true" : "false") << "\n";
  for (size_t i = 0; i < r.rows.size() && i < max_rows; ++i) {
    std::cout << " ";
    for (const Cell &c : r.rows[i]) {
      std::cout << " " << cell_text(c);
    }
    std::cout << "\n";
  }
}

} // namespace

int main(int argc, char **argv) {
  const std::string db_path = argc > 1 ? argv[1] : "index.db";
  cidx::Storage db(db_path);
  Executor ex(db);

  // 1. BUILD a plan — who calls resolve_relation()? A plan is an immutable
  //    pipeline: start(source) | stage | stage. symbol(ref) resolves ref
  //    against usr, then qual_name, then spelling; in_("calls") walks call
  //    edges backwards (= callers; out() would be callees).
  Query callers = start(symbol("cidx::query::resolve_relation")) |
                  in_("calls") | select({"name", "file", "line"}) |
                  order_by({"name"});

  // 2. INSPECT it — the plan is a value. canonical_json() validates,
  //    normalizes ("calls" -> "symbol.calls") and emits the exact bytes the
  //    Python builder produces for the same plan (the cross-language IR).
  std::cout << "== canonical JSON ==\n" << canonical_json(callers.plan())
            << "\n\n";

  // 3. RUN it — read-only, deterministic, budgeted. Result::to_json() gives
  //    the stable {shape, view, count, truncated, rows} shape.
  std::cout << "== callers of resolve_relation ==\n";
  print_rows(ex.run(callers.plan()));

  // 4. FILTER + depth window — every callee reachable from validate() in
  //    1..2 calls whose name GLOBs "*pred*". `kind` is the declaration kind;
  //    predicates compose with all_of/any_of/not_.
  Query preds = start(symbol("cidx::query::validate")) | out("calls", 1, 2) |
                where(all_of({eq("kind", "function"), glob("name", "*pred*")})) |
                select({"name", "kind"}) | limit(10);
  std::cout << "\n== depth<=2 callees of validate matching '*pred*' ==\n";
  print_rows(ex.run(preds.plan()));

  // 5. The ENTITY view — Layer-1 design types. view(View::Entity) drops ids
  //    without an entity_node row; `entity_type` is the design
  //    classification, separate from `kind`.
  Query abstracts = start(codebase()) | view(View::Entity) |
                    nodes(in_list("entity_type",
                                  {"abstract_class", "interface"})) |
                    select({"name", "kind", "entity_type"}) |
                    order_by({"name"});
  std::cout << "\n== abstract classes / interfaces (entity view) ==\n";
  print_rows(ex.run(abstracts.plan()));

  // 6. SET ALGEBRA — sub-plans combine as deduped id sets. Shared callees
  //    (depth<=3) of two roots, as one JSON result document.
  Query shared = start(symbol("cidx::query::canonical_json")) |
                 out("calls", 1, 3) |
                 intersect(start(symbol("cidx::query::validate")) |
                           out("calls", 1, 3)) |
                 select({"name"}) | order_by({"name"}) | limit(5);
  std::cout << "\n== shared callees (JSON result shape) ==\n"
            << json_out::dumps_indent2(ex.run(shared.plan()).to_json())
            << "\n";

  // 7. INVALID plans fail fast with a stable E_* code before any SQL runs.
  try {
    ex.run((start(symbol("f")) | out("calls", 1, 99)).plan());
  } catch (const PlanError &e) {
    std::cout << "\n== validation ==\n  " << e.what() << "\n";
  }
  return EXIT_SUCCESS;
}
