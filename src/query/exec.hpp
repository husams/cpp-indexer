// query/exec.hpp -- SQLite executor for validated CXQ plans.
//
// Contract: docs/query-plan.md (v1). Read-only, parameterized SQL against the
// index database via a dedicated read port. Deterministic: the node
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

struct Result {
  Shape shape = Shape::Nodes;
  View view = View::Symbol;
  bool truncated = false;
  bool partial = false;
  bool unknown = false;
  int64_t scalar = 0;                  // Shape::Scalar only
  std::vector<std::string> fields;     // row column names, select order
  std::vector<std::vector<Cell>> rows; // Shape::Nodes/Rows
  IndexIdentity index;

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
  Result run(const Plan &plan);

  // Explain a plan without executing its row-producing stages. The returned
  // object contains the normalized plan and the same index identity reported
  // by Result::to_json().
  [[nodiscard]] json_out::Value explain(const Plan &plan);

private:
  QueryReadPort &read_;
};

} // namespace cidx::query
