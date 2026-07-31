// query/exec.hpp -- SQLite executor for validated CXQ plans.
//
// Contract: docs/query-plan.md (v1). Read-only, parameterized SQL against the
// index database via a dedicated read port. Deterministic: the node
// stream is kept ordered ascending by id after every stage; budgets surface
// as `truncated`, never as a silently complete result.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "cli/json_out.hpp"
#include "query/plan.hpp"
#include "query/result_protocol.hpp"
#include "storage/ports.hpp"
#include "storage/records.hpp"
#include "storage/sqlite_read_adapter.hpp"

namespace cidx {
class SqliteStorageService;
} // namespace cidx

namespace cidx::query {

class QueryReadPort {
public:
  virtual ~QueryReadPort() = default;
  virtual storage::SqliteReadDb &read_db() = 0;
  virtual std::optional<std::string> file_abs_path(int64_t file_id) = 0;
  virtual IndexIdentity index_identity() = 0;
  virtual storage::GraphReadPort &graph_read() = 0;
};

class SqliteQueryReadAdapter final : public QueryReadPort,
                                     public storage::GraphReadPort {
public:
  explicit SqliteQueryReadAdapter(SqliteStorageService &service);

  storage::SqliteReadDb &read_db() override;
  std::optional<std::string> file_abs_path(int64_t file_id) override;
  IndexIdentity index_identity() override;
  storage::GraphReadPort &graph_read() override;

  int64_t edge_count() override;
  bool graph_resolved() override;
  std::string component_abs_base(const Component &component) override;
  std::optional<SemanticUniverse>
  get_semantic_universe_by_id(int64_t id) override;
  std::optional<Symbol> graph_symbol_by_usr(const std::string &usr) override;
  std::optional<Symbol> graph_symbol_by_id(int64_t id) override;
  std::vector<Symbol> lookup_symbols_by_usr(const std::string &usr) override;
  std::vector<Symbol> find_symbols(const std::string &pattern,
                                   const std::optional<std::string> &kind,
                                   int limit) override;
  std::vector<GraphEdgeRow> graph_edges(int64_t mine_id,
                                        const std::string &direction,
                                        const std::vector<int64_t> &kind_ids,
                                        bool count_resolved,
                                        int limit) override;
  std::map<int64_t, std::vector<EdgeSiteRow>>
  edge_sites_for(const std::vector<int64_t> &edge_ids) override;
  std::vector<EdgeSiteRow> edge_sites_one(int64_t edge_id, int limit) override;
  std::vector<EdgeSiteRow> edge_sites_page(int64_t edge_id, int offset,
                                           int limit) override;
  std::optional<EdgeSiteRow> edge_site_by_key(int64_t edge_id, int64_t file_id,
                                              int64_t line,
                                              int64_t col) override;
  bool edge_has_conditional_site(int64_t edge_id) override;
  std::optional<int64_t> edge_id_for(int64_t src_id, int64_t dst_id,
                                     int64_t kind) override;
  std::vector<Symbol> redefined_symbols(int limit) override;
  std::vector<DefinitionRow> definitions_of(int64_t symbol_id) override;
  std::vector<DefinitionRow> possible_callees_of(int64_t symbol_id) override;
  std::optional<TypeNode> type_node_by_id(int64_t id) override;
  std::optional<int64_t> symbol_type_of(int64_t symbol_id,
                                        int64_t kind) override;
  std::vector<Parameter> parameters_of(int64_t symbol_id) override;
  std::vector<int64_t> type_ids_reaching(const std::string &decl_usr) override;
  std::vector<std::pair<int64_t, int64_t>>
  param_owners_of_types(const std::vector<int64_t> &type_ids) override;
  std::vector<std::pair<int64_t, int64_t>>
  symbol_type_owners_of_types(const std::vector<int64_t> &type_ids) override;

private:
  SqliteStorageService *service_;
  storage::SqliteReadDb read_db_;
};

// Execution budgets (docs/query-plan.md "Execution semantics").
constexpr int64_t kTraverseNodeBudget = 10000;
constexpr int64_t kEnumerateBudget = 10000;
constexpr int64_t kDefaultResultCap = 1000;
constexpr size_t kIdChunk = 400;

// One result cell: null, integer, or text.
using Cell = std::variant<std::nullptr_t, int64_t, std::string>;

// Execution budget for the bounded witness-path search (path()/
// reverse_type_use()): total node/type expansions across the whole stage.
constexpr int64_t kPathNodeBudget = 10000;

// Separate execution budget for path()'s witness-chain reconstruction (the
// DFS that inverts the predecessor DAG back into simple witnesses): total
// DFS descents across the whole stage. Kept independent from
// kPathNodeBudget (rather than sharing the same counter) because a single
// BFS level can legitimately hold up to kPathNodeBudget rows/successors
// -- reconstructing that one level alone costs O(kPathNodeBudget) DFS
// visits, which would immediately exhaust a shared counter that the row
// read already spent up to its own cap on. Kept well above kPathNodeBudget
// so that legitimate single- or few-level reconstructions (bounded by rows
// actually read) are never mistaken for the combinatorial blowup this
// budget exists to catch: a DAG shaped as fully-connected layers has few
// edges (linear in kPathNodeBudget) but exponentially many root-to-target
// walks through it, so DFS visits can explode long before the row budget
// notices anything is wrong (docs/query-plan.md).
constexpr int64_t kPathReconstructionBudget = 200000;

// One hop of a witness path: the node reached and the typed label of the
// relation/type-edge that reached it. `through` is empty for the start node.
// `position`/`pack_index` carry the typed view's own natural-key slot
// (parameter/template_parameter/template_argument owners; -1 = not
// applicable, e.g. a plain symbol/entity/type node) so distinct slots on the
// same owner never collapse to an indistinguishable step, and so ranking
// has a total order.
struct PathStep {
  int64_t node_id = 0;
  std::string domain;  // "symbol" | "entity" | "type" | owner-domain name
  std::string through; // relation/type_edge_kind label into this node, or the
                       // symbol_type role ("returns"/"of_type"/
                       // "underlying_type") for a symbol-domain owner ("" at
                       // the start)
  bool inbound = false;
  std::string status = "complete"; // per-hop completeness
  std::vector<EdgeSiteRow> sites;  // evidence for the hop into this node
  int64_t position = -1;           // natural-key position; -1 = not applicable
  int64_t pack_index = -1;         // pack element index; -1 = not a pack slot
};

// One bounded ordered witness path (docs/query-plan.md "Path result shape").
struct PathWitness {
  std::vector<PathStep> steps;     // ordered start..target, inclusive
  int64_t length = 0;              // number of hops (steps.size() - 1)
  std::string status = "complete"; // aggregated: partial if any hop is
};

struct Result {
  Shape shape = Shape::Nodes;
  View view = View::Symbol;
  bool truncated = false;
  bool partial = false;
  bool unknown = false;
  // Non-serialized execution metric used to verify that the path row budget
  // stops SQLite iteration at the first over-budget row.
  int64_t path_rows_examined = 0;
  // Non-serialized execution metric: cumulative DFS descents charged
  // against kPathReconstructionBudget, used to verify path() witness-chain
  // reconstruction is bounded by its own budget rather than by wall clock.
  int64_t path_reconstruction_descents_examined = 0;
  int64_t scalar = 0;                  // Shape::Scalar only
  std::vector<std::string> fields;     // row column names, select order
  std::vector<std::vector<Cell>> rows; // Shape::Nodes/Rows
  std::vector<PathWitness> paths;      // Shape::Path only
  IndexIdentity index;
  std::optional<int64_t> exhausted_budget;

  // {"shape","view","count","truncated","index","rows"} -- see
  // docs/query-plan.md.
  [[nodiscard]] json_out::Value to_json() const;

  // Versioned HSE-70 envelope. to_json() remains the legacy QueryPlan shape
  // for compatibility until downstream adapters complete their migration.
  [[nodiscard]] protocol::ResultEnvelope to_envelope() const;
  [[nodiscard]] json_out::Value to_envelope_json() const {
    return to_envelope().to_json();
  }
};

class Executor {
public:
  explicit Executor(QueryReadPort &read) : read_(read) {}

  // Validate + normalize + run. Throws PlanError on an invalid plan.
  // `after_id` pages the first nodes() enumeration from ids strictly greater
  // than the cursor. It is execution state, not part of the immutable Plan IR.
  Result run(const Plan &plan, std::optional<int64_t> after_id = std::nullopt,
             std::optional<int64_t> result_cap = std::nullopt);

  // Internal compatibility adapters already hydrate from the same read port;
  // they do not need to recompute the workspace-wide identity for each small
  // candidate probe.
  Result run_fast(const Plan &plan,
                  std::optional<int64_t> after_id = std::nullopt);

  // Explain a plan without executing its row-producing stages. The returned
  // object contains the normalized plan, the same index identity reported by
  // Result::to_json(), the final execution shape ("nodes"/"rows"/"scalar"/
  // "path"), the execution budgets, and every relation the plan touches with
  // its catalogued completeness (surfacing partial/unknown-capable inputs
  // before any query runs).
  [[nodiscard]] json_out::Value explain(const Plan &plan);

private:
  QueryReadPort &read_;
};

using PlanObserver = std::function<void(const Plan &)>;

// Test and diagnostics hook for proving that public compatibility adapters
// execute the plans they advertise. The observer sees the normalized plan at
// the executor boundary; adapters still receive only hydrated compatibility
// records.
class ScopedPlanObserver {
public:
  explicit ScopedPlanObserver(PlanObserver observer);
  ~ScopedPlanObserver();
  ScopedPlanObserver(const ScopedPlanObserver &) = delete;
  ScopedPlanObserver &operator=(const ScopedPlanObserver &) = delete;

private:
  PlanObserver observer_;
  PlanObserver *previous_ = nullptr;
};

} // namespace cidx::query
