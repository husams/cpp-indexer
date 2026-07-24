// Narrow read-only graph capability used by GraphQuery and QueryPlan.
#pragma once

#include "storage/ports.hpp"

namespace cidx::graph {

class GraphReadPort {
public:
  virtual ~GraphReadPort() = default;

  virtual storage::SqliteReadDb &read_db() = 0;
  virtual int64_t edge_count() = 0;
  virtual bool graph_resolved() = 0;
  virtual std::string component_abs_base(const Component &component) = 0;
  virtual std::optional<SemanticUniverse>
  get_semantic_universe_by_id(int64_t id) = 0;
  virtual std::optional<Symbol> graph_symbol_by_usr(const std::string &usr) = 0;
  virtual std::optional<Symbol> graph_symbol_by_id(int64_t id) = 0;
  virtual std::vector<Symbol>
  lookup_symbols_by_usr(const std::string &usr) = 0;
  virtual std::vector<Symbol>
  find_symbols(const std::string &pattern,
               const std::optional<std::string> &kind, int limit) = 0;
  virtual std::vector<GraphEdgeRow>
  graph_edges(int64_t mine_id, const std::string &direction,
              const std::vector<int64_t> &kind_ids, bool count_resolved,
              int limit) = 0;
  virtual std::map<int64_t, std::vector<EdgeSiteRow>>
  edge_sites_for(const std::vector<int64_t> &edge_ids) = 0;
  virtual std::vector<EdgeSiteRow> edge_sites_one(int64_t edge_id,
                                                   int limit) = 0;
  virtual std::vector<Symbol> redefined_symbols(int limit) = 0;
  virtual std::vector<DefinitionRow> definitions_of(int64_t symbol_id) = 0;
  virtual std::vector<DefinitionRow>
  possible_callees_of(int64_t symbol_id) = 0;
  virtual std::optional<TypeNode> type_node_by_id(int64_t id) = 0;
  virtual std::optional<int64_t> symbol_type_of(int64_t symbol_id,
                                                 int64_t kind) = 0;
  virtual std::vector<Parameter> parameters_of(int64_t symbol_id) = 0;
  virtual std::vector<int64_t>
  type_ids_reaching(const std::string &decl_usr) = 0;
  virtual std::vector<std::pair<int64_t, int64_t>>
  param_owners_of_types(const std::vector<int64_t> &type_ids) = 0;
  virtual std::vector<std::pair<int64_t, int64_t>>
  symbol_type_owners_of_types(const std::vector<int64_t> &type_ids) = 0;
};

} // namespace cidx::graph
