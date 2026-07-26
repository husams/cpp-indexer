// query/exec.cpp -- SQLite executor for validated CXQ plans.
// Contract: docs/query-plan.md (v1). The Python twin is
// python/indexer/queryplan.py: both build the same SQL shapes over the same
// tables, so semantics stay identical by construction.

#include "query/exec.hpp"
#include "graph/query.hpp"

#include "catalogs/generated_catalog.hpp"
#include "storage/storage.hpp"
#include "util/version.hpp"

#include <algorithm>
#include <compare>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "util/pathutil.hpp"

namespace cidx::query {

SqliteQueryReadAdapter::SqliteQueryReadAdapter(SqliteStorageService &service)
    : service_(&service), read_db_(service.raw_db()) {}

storage::SqliteReadDb &SqliteQueryReadAdapter::read_db() { return read_db_; }

std::optional<std::string>
SqliteQueryReadAdapter::file_abs_path(int64_t file_id) {
  return service_->file_abs_path(file_id);
}

IndexIdentity SqliteQueryReadAdapter::index_identity() {
  return service_->index_identity();
}

storage::GraphReadPort &SqliteQueryReadAdapter::graph_read() { return *this; }

int64_t SqliteQueryReadAdapter::edge_count() { return service_->edge_count(); }

bool SqliteQueryReadAdapter::graph_resolved() {
  return service_->graph_resolved();
}

std::string
SqliteQueryReadAdapter::component_abs_base(const Component &component) {
  return service_->component_abs_base(component);
}

std::optional<SemanticUniverse>
SqliteQueryReadAdapter::get_semantic_universe_by_id(int64_t id) {
  return service_->get_semantic_universe_by_id(id);
}

std::optional<Symbol>
SqliteQueryReadAdapter::graph_symbol_by_usr(const std::string &usr) {
  return service_->graph_symbol_by_usr(usr);
}

std::optional<Symbol> SqliteQueryReadAdapter::graph_symbol_by_id(int64_t id) {
  return service_->graph_symbol_by_id(id);
}

std::vector<Symbol>
SqliteQueryReadAdapter::lookup_symbols_by_usr(const std::string &usr) {
  return service_->lookup_symbols_by_usr(usr);
}

std::vector<Symbol>
SqliteQueryReadAdapter::find_symbols(const std::string &pattern,
                                     const std::optional<std::string> &kind,
                                     int limit) {
  return service_->find_symbols(pattern, kind, limit);
}

std::vector<GraphEdgeRow> SqliteQueryReadAdapter::graph_edges(
    int64_t mine_id, const std::string &direction,
    const std::vector<int64_t> &kind_ids, bool count_resolved, int limit) {
  return service_->graph_edges(mine_id, direction, kind_ids, count_resolved,
                               limit);
}

std::map<int64_t, std::vector<EdgeSiteRow>>
SqliteQueryReadAdapter::edge_sites_for(const std::vector<int64_t> &edge_ids) {
  return service_->edge_sites_for(edge_ids);
}

std::vector<EdgeSiteRow> SqliteQueryReadAdapter::edge_sites_one(int64_t edge_id,
                                                                int limit) {
  return service_->edge_sites_one(edge_id, limit);
}

std::vector<Symbol> SqliteQueryReadAdapter::redefined_symbols(int limit) {
  return service_->redefined_symbols(limit);
}

std::vector<DefinitionRow>
SqliteQueryReadAdapter::definitions_of(int64_t symbol_id) {
  return service_->definitions_of(symbol_id);
}

std::vector<DefinitionRow>
SqliteQueryReadAdapter::possible_callees_of(int64_t symbol_id) {
  return service_->possible_callees_of(symbol_id);
}

std::optional<TypeNode> SqliteQueryReadAdapter::type_node_by_id(int64_t id) {
  return service_->type_node_by_id(id);
}

std::optional<int64_t> SqliteQueryReadAdapter::symbol_type_of(int64_t symbol_id,
                                                              int64_t kind) {
  return service_->symbol_type_of(symbol_id, kind);
}

std::vector<Parameter>
SqliteQueryReadAdapter::parameters_of(int64_t symbol_id) {
  return service_->parameters_of(symbol_id);
}

std::vector<int64_t>
SqliteQueryReadAdapter::type_ids_reaching(const std::string &decl_usr) {
  return service_->type_ids_reaching(decl_usr);
}

std::vector<std::pair<int64_t, int64_t>>
SqliteQueryReadAdapter::param_owners_of_types(
    const std::vector<int64_t> &type_ids) {
  return service_->param_owners_of_types(type_ids);
}

std::vector<std::pair<int64_t, int64_t>>
SqliteQueryReadAdapter::symbol_type_owners_of_types(
    const std::vector<int64_t> &type_ids) {
  return service_->symbol_type_owners_of_types(type_ids);
}

namespace {

std::string identity_segment(const std::string &value) {
  return std::to_string(value.size()) + ":" + value;
}

std::string component_owner(const std::string &repository,
                            const std::string &remote_url,
                            const std::string &semantic_universe) {
  if (!remote_url.empty()) {
    return "remote:" + remote_url;
  }
  if (!repository.empty()) {
    return "repo:" + repository;
  }
  return "universe:" +
         (semantic_universe.empty() ? "legacy" : semantic_universe);
}

json_out::Value index_identity_json(const IndexIdentity &index) {
  json_out::Object o;
  o.emplace_back("schema_version", json_out::Value::of(index.schema_version));
  if (index.source_revision) {
    o.emplace_back("source_revision",
                   json_out::Value::of(*index.source_revision));
  } else {
    o.emplace_back("source_revision", json_out::Value::null());
  }
  if (index.source_fingerprint) {
    o.emplace_back("source_fingerprint",
                   json_out::Value::of(*index.source_fingerprint));
  } else {
    o.emplace_back("source_fingerprint", json_out::Value::null());
  }
  if (index.index_config) {
    o.emplace_back("index_config", json_out::Value::of(*index.index_config));
  } else {
    o.emplace_back("index_config", json_out::Value::null());
  }
  if (index.index_config_fingerprint) {
    o.emplace_back("index_config_fingerprint",
                   json_out::Value::of(*index.index_config_fingerprint));
  } else {
    o.emplace_back("index_config_fingerprint", json_out::Value::null());
  }
  o.emplace_back("freshness", json_out::Value::of(index.freshness));
  if (index.expected_source_revision) {
    o.emplace_back("expected_source_revision",
                   json_out::Value::of(*index.expected_source_revision));
  } else {
    o.emplace_back("expected_source_revision", json_out::Value::null());
  }
  if (index.expected_source_fingerprint) {
    o.emplace_back("expected_source_fingerprint",
                   json_out::Value::of(*index.expected_source_fingerprint));
  } else {
    o.emplace_back("expected_source_fingerprint", json_out::Value::null());
  }
  if (index.expected_index_config_fingerprint) {
    o.emplace_back(
        "expected_index_config_fingerprint",
        json_out::Value::of(*index.expected_index_config_fingerprint));
  } else {
    o.emplace_back("expected_index_config_fingerprint",
                   json_out::Value::null());
  }
  return json_out::Value::obj(std::move(o));
}

// ---- field -> SQL column expression -----------------------------------------
// `kind` is ALWAYS the C++ declaration kind (symbol.kind); `entity_type` is
// ALWAYS the Layer-1 classification (entity_node.kind, NULL for non-entities).
// The two are separate fields so `kind in [class, struct]` keeps its
// declaration-kind meaning while `entity_type = abstract_class` filters on
// classification (PR #20 review).

std::string col_expr(const std::string &field, const std::string &symbol_alias,
                     const std::string &entity_alias) {
  if (field == "id") {
    return symbol_alias + ".id";
  }
  if (field == "usr") {
    return symbol_alias + ".usr";
  }
  if (field == "semantic_universe") {
    return "(SELECT su.key FROM semantic_universe su WHERE su.id = " +
           symbol_alias + ".semantic_universe_id)";
  }
  if (field == "identity_key") {
    return symbol_alias + ".identity_key";
  }
  if (field == "name") {
    return "COALESCE(" + symbol_alias + ".qual_name, " + symbol_alias +
           ".spelling)";
  }
  if (field == "spelling") {
    return symbol_alias + ".spelling";
  }
  if (field == "qual_name") {
    return symbol_alias + ".qual_name";
  }
  if (field == "kind") {
    return symbol_alias + ".kind";
  }
  if (field == "entity_type") {
    return entity_alias + ".kind";
  }
  if (field == "is_definition") {
    return symbol_alias + ".is_definition";
  }
  if (field == "is_pure") {
    return symbol_alias + ".is_pure";
  }
  if (field == "is_static") {
    return symbol_alias + ".is_static";
  }
  if (field == "callable_kind") {
    return symbol_alias + ".callable_kind";
  }
  if (field == "template_origin") {
    return symbol_alias + ".template_origin";
  }
  if (field == "template_form") {
    return symbol_alias + ".template_form";
  }
  if (field == "owner") {
    return std::string("(SELECT COALESCE(p.qual_name, p.spelling) FROM symbol "
                       "p WHERE ") +
           "p.usr = " + symbol_alias + ".parent_usr)";
  }
  if (field == "file") {
    return symbol_alias + ".file_id";
  }
  if (field == "line") {
    return symbol_alias + ".line";
  }
  if (field == "col") {
    return symbol_alias + ".col";
  }
  throw PlanError("E_FIELD: unknown field '" + field + "'");
}

// entity_node.kind -> display name (entity_kind seed), null when out of range.
Cell entity_type_name_cell(int64_t raw) {
  static const char *names[] = {
      "other",
      "class",
      "abstract_class",
      "interface",
      "union",
      "enum",
      "class_template",
      "abstract_class_template",
      "interface_template",
      "namespace",
  };
  if (raw >= 0 && raw <= 9) {
    return Cell(std::string(names[raw]));
  }
  return Cell(nullptr);
}

std::string shape_name(Shape shape) {
  switch (shape) {
  case Shape::Nodes:
    return "nodes";
  case Shape::Rows:
    return "rows";
  case Shape::Scalar:
    return "scalar";
  case Shape::Path:
    return "path";
  }
  return "scalar";
}

// type_edge_kind.id -> name ("pointee", "element_type", ...): the typed
// `through` label for one reverse-type-use climb hop.
std::string type_edge_kind_name(int64_t kind_id) {
  for (const auto &k : catalog::kTypeEdgeKinds) {
    if (k.id == kind_id) {
      return std::string(k.name);
    }
  }
  return "unknown";
}

// kind/entity_type predicate value: name -> stored int, by FIELD (not view).
int64_t kind_value_id(const std::string &field, const std::string &name) {
  return field == "entity_type" ? entity_kind_id(name) : symbol_kind_id(name);
}

// ---- predicate -> SQL
// ---------------------------------------------------------

bool is_kind_field(const std::string &field) {
  return field == "kind" || field == "entity_type";
}

struct SqlAliasState {
  size_t next_id = 0;

  std::string next(const char *prefix) {
    return std::string(prefix) + std::to_string(next_id++);
  }
};

enum class TargetTruth : std::uint8_t { True, False, Unknown };

void pred_sql(const Pred &p, View active, std::string &sql,
              std::vector<SqlValue> &args, SqlAliasState &aliases,
              const std::string &symbol_alias = "s",
              const std::string &entity_alias = "en");

std::string relation_candidates_sql(const Pred &p, View active,
                                    std::vector<SqlValue> &args,
                                    SqlAliasState &aliases,
                                    const std::string &outer_alias) {
  const RelationDesc *relation = resolve_relation(p.relation, active);
  if (relation == nullptr) {
    throw PlanError("E_RELATION: unknown relation '" + p.relation + "'");
  }
  const bool entity_layer = relation->layer == View::Entity;
  const std::string table = entity_layer ? "entity_edge" : "edge";
  const std::string from_col = p.inbound ? "dst_id" : "src_id";
  const std::string to_col = p.inbound ? "src_id" : "dst_id";
  const std::string edge_alias = aliases.next("qe");
  const std::string target_alias = aliases.next("qt");
  const std::string target_entity_alias = aliases.next("qen");
  const bool recursive = p.max_depth > 1 || p.min_depth > 1;
  std::string sql;
  if (recursive) {
    sql = "WITH RECURSIVE reach(id, depth) AS (SELECT e." + to_col +
          ", 1 FROM " + table + " e WHERE e.kind = ? AND e." + from_col +
          " = " + outer_alias + ".id UNION ALL SELECT e." + to_col +
          ", reach.depth + 1 FROM " + table + " e JOIN reach ON e." + from_col +
          " = reach.id WHERE e.kind = ? AND reach.depth < ?) ";
    args.emplace_back(relation->kind_id);
    args.emplace_back(relation->kind_id);
    args.emplace_back(p.max_depth);
    std::string value = "1";
    if (p.target) {
      std::string expression;
      pred_sql(*p.target, relation->layer, expression, args, aliases,
               target_alias, target_entity_alias);
      value = "(" + expression + ")";
    }
    sql += "SELECT DISTINCT " + target_alias + ".id, " + value +
           " AS value FROM reach JOIN symbol " + target_alias + " ON " +
           target_alias + ".id = reach.id LEFT JOIN entity_node " +
           target_entity_alias + " ON " + target_entity_alias +
           ".id = " + target_alias + ".id WHERE reach.depth BETWEEN ? AND ?";
    args.emplace_back(p.min_depth);
    args.emplace_back(p.max_depth);
  } else {
    std::string value = "1";
    if (p.target) {
      std::string expression;
      pred_sql(*p.target, relation->layer, expression, args, aliases,
               target_alias, target_entity_alias);
      value = "(" + expression + ")";
    }
    args.emplace_back(relation->kind_id);
    sql = "SELECT DISTINCT " + target_alias + ".id, " + value +
          " AS value "
          "FROM " +
          table + " " + edge_alias + " JOIN symbol " + target_alias + " ON " +
          target_alias + ".id = " + edge_alias + "." + to_col +
          " LEFT JOIN entity_node " + target_entity_alias + " ON " +
          target_entity_alias + ".id = " + target_alias + ".id WHERE " +
          edge_alias + ".kind = ? AND " + edge_alias + "." + from_col + " = " +
          outer_alias + ".id";
  }
  return sql;
}

std::string relation_count_sql(const Pred &p, View active,
                               std::vector<SqlValue> &args,
                               SqlAliasState &aliases, TargetTruth truth,
                               const std::string &outer_alias) {
  const std::string candidates =
      relation_candidates_sql(p, active, args, aliases, outer_alias);
  const char *test = " IS NULL";
  if (truth == TargetTruth::True) {
    test = " IS TRUE";
  } else if (truth == TargetTruth::False) {
    test = " IS FALSE";
  }
  return "(SELECT COUNT(*) FROM (" + candidates + ") AS " +
         aliases.next("rows") + " WHERE value" + test + ")";
}

void pred_sql(const Pred &p, View active, std::string &sql,
              std::vector<SqlValue> &args, SqlAliasState &aliases,
              const std::string &symbol_alias,
              const std::string &entity_alias) {
  switch (p.op) {
  case PredOp::AllOf:
  case PredOp::AnyOf: {
    if (p.kids.empty()) {
      sql += p.op == PredOp::AllOf ? "1" : "0";
      return;
    }
    const char *joiner = p.op == PredOp::AllOf ? " AND " : " OR ";
    sql += "(";
    for (size_t i = 0; i < p.kids.size(); ++i) {
      if (i != 0) {
        sql += joiner;
      }
      pred_sql(p.kids[i], active, sql, args, aliases, symbol_alias,
               entity_alias);
    }
    sql += ")";
    return;
  }
  case PredOp::Not:
    sql += "NOT (";
    pred_sql(p.kids[0], active, sql, args, aliases, symbol_alias, entity_alias);
    sql += ")";
    return;
  case PredOp::Eq:
  case PredOp::Ne: {
    sql += col_expr(p.field, symbol_alias, entity_alias);
    sql += p.op == PredOp::Eq ? " = ?" : " != ?";
    if (p.int_value.has_value()) {
      args.emplace_back(*p.int_value);
    } else if (is_kind_field(p.field)) {
      args.emplace_back(kind_value_id(p.field, p.str_values[0]));
    } else {
      args.emplace_back(p.str_values[0]);
    }
    return;
  }
  case PredOp::Glob:
    sql += col_expr(p.field, symbol_alias, entity_alias);
    sql += " GLOB ?";
    args.emplace_back(p.str_values[0]);
    return;
  case PredOp::In: {
    sql += col_expr(p.field, symbol_alias, entity_alias);
    sql += " IN (";
    for (size_t i = 0; i < p.str_values.size(); ++i) {
      sql += i == 0 ? "?" : ",?";
      if (is_kind_field(p.field)) {
        args.emplace_back(kind_value_id(p.field, p.str_values[i]));
      } else {
        args.emplace_back(p.str_values[i]);
      }
    }
    sql += ")";
    return;
  }
  case PredOp::Exists:
  case PredOp::None:
  case PredOp::All:
  case PredOp::AtLeast:
  case PredOp::Exactly: {
    const RelationDesc *relation = resolve_relation(p.relation, active);
    if (relation == nullptr) {
      throw PlanError("E_RELATION: unknown relation '" + p.relation + "'");
    }
    const bool complete = relation->completeness == "complete";
    if (p.op == PredOp::Exists || p.op == PredOp::None) {
      const std::string true_count = relation_count_sql(
          p, active, args, aliases, TargetTruth::True, symbol_alias);
      const std::string unknown_count =
          p.target ? relation_count_sql(p, active, args, aliases,
                                        TargetTruth::Unknown, symbol_alias)
                   : "0";
      sql += "CASE WHEN " + true_count + " > 0 THEN " +
             (p.op == PredOp::Exists ? "1" : "0") + " WHEN " + unknown_count +
             " > 0 THEN NULL WHEN " + (complete ? "1" : "0") + " THEN " +
             (p.op == PredOp::Exists ? "0" : "1") + " ELSE NULL END";
      return;
    }
    if (p.op == PredOp::All) {
      const std::string false_count = relation_count_sql(
          p, active, args, aliases, TargetTruth::False, symbol_alias);
      const std::string unknown_count =
          p.target ? relation_count_sql(p, active, args, aliases,
                                        TargetTruth::Unknown, symbol_alias)
                   : "0";
      sql += "CASE WHEN " + false_count + " > 0 THEN 0 WHEN " + unknown_count +
             " > 0 THEN NULL WHEN " + (complete ? "1" : "0") +
             " THEN 1 ELSE NULL END";
      return;
    }
    const std::string true_count = relation_count_sql(
        p, active, args, aliases, TargetTruth::True, symbol_alias);
    if (p.op == PredOp::Exactly) {
      const std::string equal_count = relation_count_sql(
          p, active, args, aliases, TargetTruth::True, symbol_alias);
      const std::string unknown_for_equal =
          p.target ? relation_count_sql(p, active, args, aliases,
                                        TargetTruth::Unknown, symbol_alias)
                   : "0";
      const std::string unknown_for_complete =
          p.target ? relation_count_sql(p, active, args, aliases,
                                        TargetTruth::Unknown, symbol_alias)
                   : "0";
      sql += "CASE WHEN " + true_count + " > " + std::to_string(p.threshold) +
             " THEN 0 WHEN " + (complete ? "1" : "0") + " AND " + equal_count +
             " = " + std::to_string(p.threshold) + " AND " + unknown_for_equal +
             " = 0 THEN 1 WHEN " + (complete ? "1" : "0") + " AND " +
             unknown_for_complete + " = 0 THEN 0 ELSE NULL END";
      return;
    }
    const std::string unknown_count =
        p.target ? relation_count_sql(p, active, args, aliases,
                                      TargetTruth::Unknown, symbol_alias)
                 : "0";
    sql += "CASE WHEN " + true_count + " >= " + std::to_string(p.threshold) +
           " THEN 1 WHEN " + (complete ? "1" : "0") + " AND " + unknown_count +
           " = 0 THEN 0 ELSE NULL END";
    return;
  }
  }
}

// ---- small SQL helpers
// ----------------------------------------------------------

std::string placeholders(size_t n) {
  std::string s;
  for (size_t i = 0; i < n; ++i) {
    s += i == 0 ? "?" : ",?";
  }
  return s;
}

std::vector<int64_t> fetch_ids(storage::SqliteReadDb &db,
                               const std::string &sql,
                               const std::vector<SqlValue> &args) {
  auto st = db.prepare(sql);
  for (size_t i = 0; i < args.size(); ++i) {
    st.bind(static_cast<int>(i + 1), args[i]);
  }
  std::vector<int64_t> out;
  while (st.step()) {
    out.push_back(st.col_int64(0));
  }
  return out;
}

// ---- executor state
// --------------------------------------------------------------

struct Stream {
  struct LogicalKey {
    int64_t a = 0;
    int64_t b = 0;
    int64_t c = 0;
    int64_t d = 0;
    int64_t e = 0;
    int64_t tag = 0;
    bool operator<(const LogicalKey &other) const {
      return std::tie(a, b, c, d, e, tag) <
             std::tie(other.a, other.b, other.c, other.d, other.e, other.tag);
    }
    bool operator>(const LogicalKey &other) const { return other < *this; }
    bool operator<=(const LogicalKey &other) const { return !(other < *this); }
    bool operator>=(const LogicalKey &other) const { return !(*this < other); }
    bool operator==(const LogicalKey &) const = default;
  };

  struct RowStatus {
    bool partial = false;
    bool unknown = false;
  };

  View view = View::Symbol;
  Shape shape = Shape::Nodes;
  std::vector<int64_t> ids;     // nodes shape; ascending, deduped
  std::vector<LogicalKey> keys; // typed logical rows; never SQLite row ids
  std::vector<std::string> fields;
  std::vector<std::vector<Cell>> rows; // rows shape
  std::vector<int64_t> row_ids;        // per-row id (order_by tie-break)
  std::vector<RowStatus> row_status;   // aligned with rows
  std::vector<PathWitness> paths;      // Shape::Path
  bool truncated = false;
  bool partial = false;
  bool unknown = false;
  int64_t path_rows_examined = 0;
  // True only while a limit() is in effect with NO cardinality-expanding
  // stage (nodes/out/in/union) after it -- otherwise finish() re-applies the
  // default result cap (PR #20 review: an early limit must not disable the
  // final safety cap).
  bool limit_in_effect = false;
};

bool is_typed_view(View view) {
  return view != View::Symbol && view != View::Entity;
}

// Deterministic Cell ordering: ints < strings < null.
int cell_rank(const Cell &c) {
  if (std::holds_alternative<std::nullptr_t>(c)) {
    return 2;
  }
  if (std::holds_alternative<int64_t>(c)) {
    return 0;
  }
  return 1;
}

bool cell_less(const Cell &a, const Cell &b) {
  const int ra = cell_rank(a);
  const int rb = cell_rank(b);
  if (ra != rb) {
    return ra < rb;
  }
  if (ra == 0) {
    return std::get<int64_t>(a) < std::get<int64_t>(b);
  }
  if (ra == 1) {
    return std::get<std::string>(a) < std::get<std::string>(b);
  }
  return false; // two nulls
}

bool cell_eq(const Cell &a, const Cell &b) {
  return !cell_less(a, b) && !cell_less(b, a);
}

struct ReceiverTypes {
  bool top = true;
  std::set<std::string> types;
};

ReceiverTypes exact_receiver(const std::string &type_usr) {
  return {.top = false, .types = {type_usr}};
}

void join_receiver_types(ReceiverTypes &into, const ReceiverTypes &other) {
  if (into.top || other.top) {
    into.top = true;
    into.types.clear();
    return;
  }
  into.types.insert(other.types.begin(), other.types.end());
}

ReceiverTypes resolve_receiver(const std::vector<graph::Site> &sites,
                               const ReceiverTypes &current_this) {
  ReceiverTypes result{.top = false, .types = {}};
  bool saw_receiver = false;
  for (const auto &site : sites) {
    if (!site.recv_src_kind) {
      continue;
    }
    saw_receiver = true;
    ReceiverTypes resolved;
    const bool exact_static_type =
        site.recv_type_usr &&
        ((site.recv_type_is_value && *site.recv_type_is_value != 0) ||
         *site.recv_src_kind == "construct");
    if (exact_static_type) {
      resolved = exact_receiver(*site.recv_type_usr);
    } else if (*site.recv_src_kind == "this") {
      resolved = current_this;
    }
    join_receiver_types(result, resolved);
  }
  return saw_receiver ? result : ReceiverTypes{};
}

void append_unique(std::vector<graph::Sym> &symbols, const graph::Sym &symbol) {
  if (std::ranges::none_of(symbols, [&symbol](const graph::Sym &item) {
        return item.id == symbol.id;
      })) {
    symbols.push_back(symbol);
  }
}

std::optional<graph::Sym>
select_dispatch_target(graph::GraphQuery &graph,
                       const std::vector<graph::Sym> &candidates,
                       const std::string &receiver_usr) {
  const auto receiver = graph.get_by_usr(receiver_usr);
  if (!receiver) {
    return std::nullopt;
  }
  std::vector<std::string> owners{receiver_usr};
  for (const auto &base : graph.bases(receiver->id, false)) {
    owners.push_back(base.usr);
  }
  for (const auto &owner_usr : owners) {
    const auto target = std::ranges::find_if(
        candidates, [&owner_usr](const graph::Sym &candidate) {
          return candidate.parent_usr && *candidate.parent_usr == owner_usr;
        });
    if (target != candidates.end()) {
      return *target;
    }
  }
  return std::nullopt;
}

std::vector<graph::Sym>
receiver_aware_callees(graph::GraphQuery &graph, const graph::Sym &callee,
                       const ReceiverTypes &receiver_types) {
  std::vector<graph::Sym> result{callee};
  if (!graph.is_virtual_method(callee.id)) {
    return result;
  }
  const auto candidates = graph.dispatch_targets(callee.id);
  if (receiver_types.top) {
    for (const auto &candidate : candidates) {
      append_unique(result, candidate);
    }
    return result;
  }
  for (const auto &receiver_usr : receiver_types.types) {
    const auto target = select_dispatch_target(graph, candidates, receiver_usr);
    if (!target) {
      for (const auto &candidate : candidates) {
        append_unique(result, candidate);
      }
      return result;
    }
    append_unique(result, *target);
  }
  return result;
}

class Exec {
public:
  explicit Exec(QueryReadPort &read) : read_(read) {}

  Stream run_plan(const Plan &plan) {
    Stream st;
    st.view =
        plan.source.kind == SourceKind::Entity ? View::Entity : View::Symbol;
    if (plan.source.kind != SourceKind::Codebase) {
      st.ids = resolve_source(plan.source);
    }

    for (const auto &stage : plan.stages) {
      reject_ambiguous_ungrouped(st);
      switch (stage.op) {
      case StageOp::Nodes:
        enumerate(st, stage.pred, stage.unknown);
        st.limit_in_effect = false;
        break;
      case StageOp::ChangeView:
        change_view(st, stage.level);
        break;
      case StageOp::Where:
        filter(st, *stage.pred, stage.unknown);
        break;
      case StageOp::Out:
      case StageOp::In:
        if (stage.mode == TraversalMode::Devirtualized) {
          traverse_devirtualized(st, stage);
        } else {
          traverse(st, stage);
        }
        st.limit_in_effect = false;
        break;
      case StageOp::Sites:
        expand_sites(st);
        st.limit_in_effect = false;
        break;
      case StageOp::Union:
      case StageOp::Intersect:
      case StageOp::Except:
        set_op(st, stage);
        if (stage.op == StageOp::Union) {
          st.limit_in_effect = false;
        }
        break;
      case StageOp::Select:
        materialize(st, stage.fields);
        st.shape = Shape::Rows;
        break;
      case StageOp::Count:
        st.shape = Shape::Scalar;
        break;
      case StageOp::Distinct:
        apply_distinct(st);
        break;
      case StageOp::OrderBy:
        apply_order(st, stage.fields);
        break;
      case StageOp::Limit:
        apply_limit(st, stage.n);
        break;
      case StageOp::Path:
        path_stage(st, stage);
        break;
      case StageOp::Rank:
        rank_stage(st, stage);
        break;
      case StageOp::ReverseTypeUse:
        reverse_type_use_stage(st, stage);
        break;
      }
      if (st.shape == Shape::Scalar) {
        break; // count() is terminal
      }
    }
    reject_ambiguous_ungrouped(st);
    return st;
  }

private:
  QueryReadPort &read_;
  std::map<int64_t, std::optional<std::string>> file_paths_;

  void expand_sites(Stream &st) {
    std::ranges::sort(st.keys);
    st.keys.erase(std::ranges::unique(st.keys).begin(), st.keys.end());
    std::vector<Stream::LogicalKey> sites;
    for (const auto &edge : st.keys) {
      auto query = read_.read_db().prepare(
          "SELECT edge_id,file_id,COALESCE(line,0),COALESCE(col,0) "
          "FROM edge_site WHERE edge_id=? "
          "ORDER BY file_id,COALESCE(line,0),COALESCE(col,0)");
      query.bind(1, edge.a);
      while (query.step()) {
        sites.push_back(Stream::LogicalKey{.a = query.col_int64(0),
                                           .b = query.col_int64(1),
                                           .c = query.col_int64(2),
                                           .d = query.col_int64(3)});
        if (sites.size() > static_cast<size_t>(kTraverseNodeBudget)) {
          st.truncated = true;
          break;
        }
      }
      if (st.truncated) {
        break;
      }
    }
    std::ranges::sort(sites);
    sites.erase(std::ranges::unique(sites).begin(), sites.end());
    if (sites.size() > static_cast<size_t>(kTraverseNodeBudget)) {
      sites.resize(static_cast<size_t>(kTraverseNodeBudget));
    }
    st.keys = std::move(sites);
    st.ids.clear();
    st.view = View::Site;
  }

  bool ambiguous_ungrouped_file(int64_t file_id) {
    auto file = read_.read_db().prepare(
        "SELECT c.name,c.path,r.name,r.remote_url,su.key "
        "FROM file f "
        "JOIN directory d ON d.id=f.directory_id "
        "JOIN component c ON c.id=d.component_id "
        "LEFT JOIN repository r ON r.id=c.repository_id "
        "LEFT JOIN semantic_universe su ON "
        "su.id=COALESCE(c.semantic_universe_id,"
        "r.semantic_universe_id,1) WHERE f.id=?");
    file.bind(1, file_id);
    if (!file.step() || !pathutil::isabs(file.col_text(1))) {
      return false;
    }
    const std::string component_name = file.col_text(0);
    if (component_name.empty()) {
      return true;
    }
    const std::string owner =
        component_owner(file.col_text(2), file.col_text(3), file.col_text(4));
    auto components = read_.read_db().prepare(
        "SELECT DISTINCT c.path,r.name,r.remote_url,su.key "
        "FROM component c "
        "LEFT JOIN repository r ON r.id=c.repository_id "
        "LEFT JOIN semantic_universe su ON "
        "su.id=COALESCE(c.semantic_universe_id,"
        "r.semantic_universe_id,1) "
        "WHERE c.name=?");
    components.bind(1, std::string_view{component_name});
    while (components.step()) {
      if (pathutil::isabs(components.col_text(0)) &&
          component_owner(components.col_text(1), components.col_text(2),
                          components.col_text(3)) == owner &&
          components.col_text(0) != file.col_text(1)) {
        return true;
      }
    }
    return false;
  }

  void reject_ambiguous_ungrouped(Stream &st) {
    if (st.view != View::CallArgument && st.view != View::Site &&
        st.view != View::Evidence) {
      return;
    }
    for (const auto &key : st.keys) {
      if (st.view == View::Evidence && key.tag == 1) {
        continue;
      }
      if (ambiguous_ungrouped_file(key.b)) {
        throw PlanError("E_IDENTITY: ambiguous ungrouped component identity");
      }
    }
  }

  static const std::string &join_clause(bool need_entity) {
    static const std::string entity_join =
        " LEFT JOIN entity_node en ON en.id = s.id";
    static const std::string none;
    return need_entity ? entity_join : none;
  }

  static bool pred_uses_entity_type(const Pred &p) {
    if (p.field == "entity_type") {
      return true;
    }
    for (const auto &k : p.kids) {
      if (pred_uses_entity_type(k)) {
        return true;
      }
    }
    return p.target && pred_uses_entity_type(*p.target);
  }

  std::vector<int64_t> resolve_source(const Source &src) {
    const char *join = src.kind == SourceKind::Entity
                           ? " JOIN entity_node en ON en.id = s.id"
                           : "";
    for (const char *col : {"s.usr", "s.qual_name", "s.spelling"}) {
      std::string sql = std::string("SELECT s.id FROM symbol s") + join +
                        " WHERE " + col + " = ? ORDER BY s.id";
      auto ids = fetch_ids(read_.read_db(), sql, {SqlValue(src.ref)});
      if (!ids.empty()) {
        return ids;
      }
    }
    return {};
  }

  // view(entity) enforces the typed-view invariant: ids without an
  // entity_node row are DROPPED, never surfaced as entity rows (PR #20
  // review). view(symbol) is a pure relabel (every entity id is a symbol id).
  void change_view(Stream &st, View level) {
    if (level != View::Symbol && level != View::Entity) {
      st.ids.clear();
      st.keys.clear();
      st.view = level;
      return;
    }
    if (st.view != View::Symbol && st.view != View::Entity) {
      st.keys.clear();
    }
    if (level == View::Entity && st.view != View::Entity) {
      std::vector<int64_t> kept;
      for (size_t at = 0; at < st.ids.size(); at += kIdChunk) {
        const size_t n = std::min(kIdChunk, st.ids.size() - at);
        std::string sql = "SELECT id FROM entity_node WHERE id IN (" +
                          placeholders(n) + ") ORDER BY id";
        std::vector<SqlValue> args;
        args.reserve(n);
        for (size_t i = 0; i < n; ++i) {
          args.emplace_back(st.ids[at + i]);
        }
        auto part = fetch_ids(read_.read_db(), sql, args);
        kept.insert(kept.end(), part.begin(), part.end());
      }
      std::ranges::sort(kept);
      kept.erase(std::ranges::unique(kept).begin(), kept.end());
      st.ids = std::move(kept);
    }
    st.view = level;
  }

  static void append_unknown_policy(std::string &sql, UnknownPolicy policy) {
    sql += policy == UnknownPolicy::Include ? " IS NOT FALSE" : " IS TRUE";
  }

  void enumerate(Stream &st, const std::optional<Pred> &pred,
                 UnknownPolicy unknown) {
    if (st.view != View::Symbol && st.view != View::Entity) {
      std::string sql;
      switch (st.view) {
      case View::Parameter:
        sql = "SELECT owner_id,position,pack_index FROM parameter ORDER BY "
              "owner_id,position,pack_index";
        break;
      case View::TemplateParameter:
        sql = "SELECT owner_id,position FROM template_param ORDER BY "
              "owner_id,position";
        break;
      case View::TemplateArgument:
        sql = "SELECT owner_id,position,pack_index FROM template_arg ORDER BY "
              "owner_id,position,pack_index";
        break;
      case View::SignatureSlot:
        sql =
            "SELECT symbol_id,-1,-1,0,0,1 FROM symbol_type "
            "WHERE kind=1 UNION ALL SELECT owner_id,position,pack_index,0,0,2 "
            "FROM parameter UNION ALL SELECT owner_id,position,-1,0,0,3 "
            "FROM template_param UNION ALL SELECT "
            "owner_id,position,pack_index,0,0,4 "
            "FROM template_arg ORDER BY 1,2,3,6";
        break;
      case View::CallArgument:
        sql = "SELECT edge_id,file_id,line,col,position FROM call_arg ORDER BY "
              "edge_id,file_id,line,col,position";
        break;
      case View::Edge:
        sql = "SELECT id FROM edge ORDER BY id";
        break;
      case View::Evidence:
      case View::Site:
        sql = "SELECT edge_id,file_id,COALESCE(line,0),COALESCE(col,0) "
              "FROM edge_site ORDER BY edge_id,file_id,line,col";
        break;
      case View::Type:
        sql = "SELECT id FROM type_node ORDER BY id";
        break;
      case View::TypeLayer: {
        graph::GraphQuery graph(read_.graph_read());
        for (const auto &row :
             logical_rows(View::Type, "SELECT id FROM type_node ORDER BY id",
                          {SqlValue(kEnumerateBudget + 1)})) {
          const auto layers = graph.type_layers(row.a);
          for (std::size_t i = 0; i < layers.size(); ++i) {
            st.keys.push_back(
                LogicalKey{.a = row.a, .b = static_cast<int64_t>(i)});
            if (st.keys.size() > static_cast<std::size_t>(kEnumerateBudget)) {
              st.truncated = true;
              st.keys.resize(kEnumerateBudget);
              break;
            }
          }
          if (st.truncated) {
            break;
          }
        }
        if (pred) {
          filter(st, *pred, unknown);
        }
        return;
      }
      case View::Symbol:
      case View::Entity:
        break;
      }
      sql += " LIMIT ?";
      st.keys = logical_rows(st.view, sql, {SqlValue(kEnumerateBudget + 1)});
      if (st.keys.size() > static_cast<size_t>(kEnumerateBudget)) {
        st.keys.resize(kEnumerateBudget);
        st.truncated = true;
      }
      if (pred) {
        filter(st, *pred, unknown);
      }
      return;
    }
    std::string sql = "SELECT s.id FROM symbol s";
    if (st.view == View::Entity) {
      sql += " JOIN entity_node en ON en.id = s.id";
    } else if (pred && pred_uses_entity_type(*pred)) {
      sql += join_clause(true);
    }
    std::vector<SqlValue> args;
    if (pred) {
      sql += " WHERE ";
      SqlAliasState aliases;
      pred_sql(*pred, st.view, sql, args, aliases);
      if (unknown == UnknownPolicy::Error &&
          !fetch_ids(read_.read_db(), sql + " IS NULL LIMIT 1", args).empty()) {
        throw PlanError("E_UNKNOWN: predicate evaluation is unknown");
      }
      append_unknown_policy(sql, unknown);
    }
    sql += " ORDER BY s.id LIMIT ?";
    args.emplace_back(kEnumerateBudget + 1);
    st.ids = fetch_ids(read_.read_db(), sql, args);
    if (st.ids.size() > static_cast<size_t>(kEnumerateBudget)) {
      st.ids.resize(kEnumerateBudget);
      st.truncated = true;
    }
  }

  void filter(Stream &st, const Pred &pred, UnknownPolicy unknown) {
    if (is_typed_view(st.view)) {
      const auto fields = predicate_fields(pred);
      const auto cells = fetch_typed_cells(st, fields);
      std::vector<LogicalKey> kept;
      for (const auto &key : st.keys) {
        const auto it = cells.find(key);
        if (it != cells.end() && predicate_matches(pred, fields, it->second)) {
          kept.push_back(key);
        }
      }
      st.keys = std::move(kept);
      return;
    }
    std::vector<int64_t> out;
    const std::string join = join_clause(pred_uses_entity_type(pred));
    for (size_t at = 0; at < st.ids.size(); at += kIdChunk) {
      const size_t n = std::min(kIdChunk, st.ids.size() - at);
      std::string sql = "SELECT s.id FROM symbol s" + join +
                        " WHERE s.id IN (" + placeholders(n) + ") AND (";
      std::vector<SqlValue> args;
      args.reserve(n);
      for (size_t i = 0; i < n; ++i) {
        args.emplace_back(st.ids[at + i]);
      }
      SqlAliasState aliases;
      pred_sql(pred, st.view, sql, args, aliases);
      sql += ")";
      if (unknown == UnknownPolicy::Error &&
          !fetch_ids(read_.read_db(), sql + " IS NULL LIMIT 1", args).empty()) {
        throw PlanError("E_UNKNOWN: predicate evaluation is unknown");
      }
      append_unknown_policy(sql, unknown);
      sql += " ORDER BY s.id";
      auto part = fetch_ids(read_.read_db(), sql, args);
      out.insert(out.end(), part.begin(), part.end());
    }
    std::ranges::sort(out);
    out.erase(std::ranges::unique(out).begin(), out.end());
    st.ids = std::move(out);
  }

  static std::vector<std::string> predicate_fields(const Pred &pred) {
    if (pred.op == PredOp::AllOf || pred.op == PredOp::AnyOf ||
        pred.op == PredOp::Not) {
      std::set<std::string> unique;
      for (const auto &kid : pred.kids) {
        const auto nested = predicate_fields(kid);
        unique.insert(nested.begin(), nested.end());
      }
      std::vector<std::string> result(unique.begin(), unique.end());
      return result;
    }
    return {pred.field};
  }

  static bool predicate_matches(const Pred &pred,
                                const std::vector<std::string> &fields,
                                const std::vector<Cell> &cells) {
    if (pred.op == PredOp::AllOf || pred.op == PredOp::AnyOf) {
      const bool all = pred.op == PredOp::AllOf;
      for (const auto &kid : pred.kids) {
        const bool matched = predicate_matches(kid, fields, cells);
        if (all ? !matched : matched) {
          return !all;
        }
      }
      return all;
    }
    if (pred.op == PredOp::Not) {
      return !predicate_matches(pred.kids.front(), fields, cells);
    }
    const auto found = std::ranges::find(fields, pred.field);
    const auto at = std::ranges::distance(fields.begin(), found);
    if (at < 0 || static_cast<size_t>(at) >= cells.size()) {
      return false;
    }
    const Cell &value = cells[static_cast<size_t>(at)];
    auto equals = [&value](const Cell &expected) {
      return cell_eq(value, expected);
    };
    if (pred.int_value) {
      const Cell expected(*pred.int_value);
      return pred.op == PredOp::Eq ? equals(expected) : !equals(expected);
    }
    if (pred.op == PredOp::In) {
      return std::ranges::any_of(pred.str_values, [&value](const auto &item) {
        return cell_eq(value, Cell(item));
      });
    }
    const Cell expected(pred.str_values.front());
    return pred.op == PredOp::Eq ? equals(expected) : !equals(expected);
  }

  // Path-length-window BFS (PR #20 review): a node is emitted iff SOME path
  // of length d in [min_depth, max_depth] reaches it -- NOT only its shortest
  // first-discovery depth. There is no cross-level visited set; termination
  // is guaranteed by the finite max_depth (<= 32) and the state budget
  // (cumulative level sizes). In a diamond A->B, A->C->B, out(r, 2, 2)
  // therefore DOES emit B. The stream view follows the relation's layer.
  void traverse_devirtualized(Stream &st, const Stage &stage) {
    graph::GraphQuery graph(read_.graph_read());
    const std::optional<std::vector<std::string>> call_kinds =
        std::vector<std::string>{"calls"};
    std::map<int64_t, ReceiverTypes> frontier;
    for (const int64_t id : st.ids) {
      frontier.emplace(id, ReceiverTypes{});
    }
    std::set<int64_t> emitted;
    int64_t states = 0;

    for (int64_t depth = 1; depth <= stage.max_depth && !frontier.empty();
         ++depth) {
      std::map<int64_t, ReceiverTypes> level;
      for (const auto &[caller_id, current_this] : frontier) {
        const auto edges = graph.edges_out(
            caller_id, call_kinds, static_cast<int>(kTraverseNodeBudget));
        for (const auto &edge : edges) {
          const ReceiverTypes receiver =
              resolve_receiver(edge.sites, current_this);
          for (const auto &callee :
               receiver_aware_callees(graph, edge.peer, receiver)) {
            const auto [at, inserted] = level.try_emplace(callee.id, receiver);
            if (!inserted) {
              join_receiver_types(at->second, receiver);
            }
          }
        }
      }

      if (states + static_cast<int64_t>(level.size()) > kTraverseNodeBudget) {
        const auto keep = static_cast<size_t>(kTraverseNodeBudget - states);
        auto first_removed = level.begin();
        std::advance(first_removed, static_cast<int64_t>(keep));
        level.erase(first_removed, level.end());
        st.truncated = true;
      }
      states += static_cast<int64_t>(level.size());
      if (depth >= stage.min_depth) {
        for (const auto &[id, _receiver] : level) {
          emitted.insert(id);
        }
      }
      if (st.truncated) {
        break;
      }
      frontier = std::move(level);
    }
    st.ids.assign(emitted.begin(), emitted.end());
    st.view = View::Symbol;
  }

  using LogicalKey = Stream::LogicalKey;

  std::vector<LogicalKey> logical_rows(View target, const std::string &sql,
                                       const std::vector<SqlValue> &args) {
    auto query = read_.read_db().prepare(sql);
    for (size_t i = 0; i < args.size(); ++i) {
      query.bind(static_cast<int>(i + 1), args[i]);
    }
    std::vector<LogicalKey> rows;
    while (query.step()) {
      LogicalKey key;
      switch (target) {
      case View::Parameter:
      case View::TemplateArgument:
        key.a = query.col_int64(0);
        key.b = query.col_int64(1);
        key.c = query.col_int64(2);
        break;
      case View::TemplateParameter:
        key.a = query.col_int64(0);
        key.b = query.col_int64(1);
        break;
      case View::SignatureSlot:
        key.a = query.col_int64(0);
        key.b = query.col_int64(1);
        key.c = query.col_int64(2);
        key.d = query.col_int64(3);
        key.e = query.col_int64(4);
        key.tag = query.col_int64(5);
        break;
      case View::CallArgument:
        key.a = query.col_int64(0);
        key.b = query.col_int64(1);
        key.c = query.col_int64(2);
        key.d = query.col_int64(3);
        key.e = query.col_int64(4);
        break;
      case View::Evidence:
      case View::Site:
        key.a = query.col_int64(0);
        key.b = query.col_int64(1);
        key.c = query.col_int64(2);
        key.d = query.col_int64(3);
        break;
      case View::Edge:
      case View::Type:
        key.a = query.col_int64(0);
        break;
      case View::TypeLayer:
        key.a = query.col_int64(0);
        key.b = query.col_int64(1);
        break;
      case View::Symbol:
      case View::Entity:
        break;
      }
      rows.push_back(key);
    }
    std::ranges::sort(rows);
    rows.erase(std::ranges::unique(rows).begin(), rows.end());
    return rows;
  }

  std::vector<int64_t> logical_ids(const std::string &sql,
                                   const std::vector<SqlValue> &args) {
    return fetch_ids(read_.read_db(), sql, args);
  }

  void traverse_typed(Stream &st, const Stage &stage, const RelationDesc &rel) {
    const bool inbound = stage.op == StageOp::In;
    const View target = inbound ? rel.layer : rel.target_view;
    std::vector<LogicalKey> keys;
    std::vector<int64_t> ids;
    const auto add_keys = [&](View view, const std::string &sql,
                              std::vector<SqlValue> args) {
      const auto used = keys.size() + ids.size();
      if (used >= static_cast<size_t>(kTraverseNodeBudget)) {
        st.truncated = true;
        return;
      }
      const auto remaining = static_cast<size_t>(kTraverseNodeBudget) - used;
      args.emplace_back(static_cast<int64_t>(remaining + 1));
      auto rows = logical_rows(view, sql + " LIMIT ?", args);
      if (rows.size() > remaining) {
        rows.resize(remaining);
        st.truncated = true;
      }
      keys.insert(keys.end(), rows.begin(), rows.end());
    };
    const auto add_ids = [&](const std::string &sql,
                             std::vector<SqlValue> args) {
      const auto used = keys.size() + ids.size();
      if (used >= static_cast<size_t>(kTraverseNodeBudget)) {
        st.truncated = true;
        return;
      }
      const auto remaining = static_cast<size_t>(kTraverseNodeBudget) - used;
      args.emplace_back(static_cast<int64_t>(remaining + 1));
      auto rows = logical_ids(sql + " LIMIT ?", args);
      if (rows.size() > remaining) {
        rows.resize(remaining);
        st.truncated = true;
      }
      ids.insert(ids.end(), rows.begin(), rows.end());
    };
    const auto add_synthetic = [&](LogicalKey key) {
      if (keys.size() + ids.size() >=
          static_cast<size_t>(kTraverseNodeBudget)) {
        st.truncated = true;
        return;
      }
      keys.push_back(key);
    };
    const auto type_column = [](const std::string &name) {
      if (name == "of_type") {
        return "type_id";
      }
      if (name == "declared_type") {
        return "declared_type_id";
      }
      return "adjusted_type_id";
    };

    if (!inbound && st.view == View::Symbol) {
      for (const auto owner : st.ids) {
        if (rel.name == "has_parameter") {
          add_keys(View::Parameter,
                   "SELECT owner_id,position,pack_index FROM parameter "
                   "WHERE owner_id=? ORDER BY position,pack_index",
                   {SqlValue(owner)});
        } else if (rel.name == "has_template_parameter") {
          add_keys(View::TemplateParameter,
                   "SELECT owner_id,position FROM template_param WHERE "
                   "owner_id=? ORDER BY position",
                   {SqlValue(owner)});
        } else if (rel.name == "has_template_argument") {
          add_keys(View::TemplateArgument,
                   "SELECT owner_id,position,pack_index FROM template_arg "
                   "WHERE owner_id=? ORDER BY position,pack_index",
                   {SqlValue(owner)});
        } else if (rel.name == "has_signature_slot") {
          add_keys(
              View::SignatureSlot,
              "SELECT symbol_id,-1,-1,0,0,1 FROM symbol_type WHERE "
              "symbol_id=? AND kind=1 UNION ALL SELECT owner_id,position,"
              "pack_index,0,0,2 FROM parameter WHERE owner_id=? UNION ALL "
              "SELECT owner_id,position,-1,0,0,3 FROM template_param WHERE "
              "owner_id=? UNION ALL SELECT owner_id,position,pack_index,0,0,4 "
              "FROM template_arg WHERE owner_id=? ORDER BY 1,2,3,6",
              {SqlValue(owner), SqlValue(owner), SqlValue(owner),
               SqlValue(owner)});
        } else if (rel.name == "has_call_edge") {
          add_keys(View::Edge,
                   "SELECT id FROM edge WHERE src_id=? AND kind=? ORDER BY id",
                   {SqlValue(owner), SqlValue(rel.kind_id - 23)});
        } else if (rel.name == "has_evidence") {
          add_keys(View::Evidence,
                   "SELECT es.edge_id,es.file_id,COALESCE(es.line,0),"
                   "COALESCE(es.col,0) FROM edge_site es JOIN edge e ON "
                   "e.id=es.edge_id WHERE e.src_id=? ORDER BY es.edge_id,"
                   "es.file_id,es.line,es.col",
                   {SqlValue(owner)});
        } else if (rel.name == "has_site") {
          add_keys(View::Site,
                   "SELECT es.edge_id,es.file_id,COALESCE(es.line,0),"
                   "COALESCE(es.col,0) FROM edge_site es JOIN edge e ON "
                   "e.id=es.edge_id WHERE e.src_id=? ORDER BY es.edge_id,"
                   "es.file_id,es.line,es.col",
                   {SqlValue(owner)});
        } else if (rel.name == "of_type") {
          add_ids("SELECT type_id FROM symbol_type WHERE symbol_id=? "
                  "ORDER BY type_id",
                  {SqlValue(owner)});
        }
      }
    } else if (inbound && st.view == View::Symbol &&
               rel.name == "of_callable") {
      for (const auto owner : st.ids) {
        add_keys(
            View::SignatureSlot,
            "SELECT symbol_id,-1,-1,0,0,1 FROM symbol_type WHERE "
            "symbol_id=? AND kind=1 UNION ALL SELECT owner_id,position,"
            "pack_index,0,0,2 FROM parameter WHERE owner_id=? UNION ALL "
            "SELECT owner_id,position,-1,0,0,3 FROM template_param WHERE "
            "owner_id=? UNION ALL SELECT owner_id,position,pack_index,0,0,4 "
            "FROM template_arg WHERE owner_id=? ORDER BY 1,2,3,6",
            {SqlValue(owner), SqlValue(owner), SqlValue(owner),
             SqlValue(owner)});
      }
    } else if (!inbound && st.view == View::SignatureSlot &&
               rel.name == "of_callable") {
      for (const auto &key : st.keys) {
        add_ids("SELECT ?", {SqlValue(key.a)});
      }
    } else if (inbound && st.view == View::Symbol && rel.name == "of_type") {
      for (const auto owner : st.ids) {
        add_keys(
            View::SignatureSlot,
            "SELECT symbol_id,-1,-1,0,0,1 FROM symbol_type WHERE "
            "symbol_id=? AND kind=1 UNION ALL SELECT owner_id,position,"
            "pack_index,0,0,2 FROM parameter WHERE owner_id=? UNION ALL "
            "SELECT owner_id,position,-1,0,0,3 FROM template_param WHERE "
            "owner_id=? UNION ALL SELECT owner_id,position,pack_index,0,0,4 "
            "FROM template_arg WHERE owner_id=? ORDER BY 1,2,3,6",
            {SqlValue(owner), SqlValue(owner), SqlValue(owner),
             SqlValue(owner)});
      }
    } else if (inbound && st.view == View::Type &&
               rel.layer == View::SignatureSlot && rel.name == "of_type") {
      for (const auto &key : st.keys) {
        add_keys(
            View::SignatureSlot,
            "SELECT symbol_id,-1,-1,0,0,1 FROM symbol_type WHERE "
            "type_id=? AND kind=1 UNION ALL SELECT owner_id,position,"
            "pack_index,0,0,2 FROM parameter WHERE type_id=? UNION ALL "
            "SELECT owner_id,position,-1,0,0,3 FROM template_param WHERE "
            "type_id=? UNION ALL SELECT owner_id,position,pack_index,0,0,4 "
            "FROM template_arg WHERE type_id=? ORDER BY 1,2,3,6",
            {SqlValue(key.a), SqlValue(key.a), SqlValue(key.a),
             SqlValue(key.a)});
      }
    } else if (!inbound && st.view == View::SignatureSlot &&
               rel.name == "of_type") {
      for (const auto &key : st.keys) {
        if (key.tag == 1) {
          add_ids(
              "SELECT type_id FROM symbol_type WHERE symbol_id=? AND kind=1",
              {SqlValue(key.a)});
        } else if (key.tag == 2) {
          add_ids("SELECT type_id FROM parameter WHERE owner_id=? AND "
                  "position=? AND pack_index=?",
                  {SqlValue(key.a), SqlValue(key.b), SqlValue(key.c)});
        } else if (key.tag == 3) {
          add_ids("SELECT type_id FROM template_param WHERE owner_id=? AND "
                  "position=?",
                  {SqlValue(key.a), SqlValue(key.b)});
        } else {
          add_ids("SELECT type_id FROM template_arg WHERE owner_id=? AND "
                  "position=? AND pack_index=?",
                  {SqlValue(key.a), SqlValue(key.b), SqlValue(key.c)});
        }
      }
    } else if (!inbound && st.view == View::Type && rel.name == "has_layer") {
      graph::GraphQuery graph(read_.graph_read());
      for (const auto &key : st.keys) {
        const auto layers = graph.type_layers(key.a);
        for (std::size_t i = 0; i < layers.size(); ++i) {
          add_synthetic({.a = key.a, .b = static_cast<int64_t>(i)});
        }
      }
    } else if (!inbound && st.view == View::TypeLayer &&
               rel.name == "of_type") {
      for (const auto &key : st.keys) {
        add_ids("SELECT ?", {SqlValue(key.a)});
      }
    } else if (inbound && st.view == View::SignatureSlot &&
               rel.name == "has_signature_slot") {
      for (const auto &key : st.keys) {
        add_ids("SELECT ?", {SqlValue(key.a)});
      }
    } else if (inbound && st.view == View::TypeLayer &&
               rel.name == "has_layer") {
      for (const auto &key : st.keys) {
        add_ids("SELECT ?", {SqlValue(key.a)});
      }
    } else if (!inbound && st.view == View::TypeLayer && rel.name == "child") {
      graph::GraphQuery graph(read_.graph_read());
      for (const auto &key : st.keys) {
        const auto layers = graph.type_layers(key.a);
        if (key.b < 0 || static_cast<std::size_t>(key.b) >= layers.size()) {
          continue;
        }
        const auto &parent = layers[static_cast<std::size_t>(key.b)];
        for (std::size_t i = 0; i < layers.size(); ++i) {
          const auto &child = layers[i];
          if (child.depth == parent.depth + 1 &&
              child.path.starts_with(parent.path + ".")) {
            add_synthetic({.a = key.a, .b = static_cast<int64_t>(i)});
          }
        }
      }
    } else if (inbound && st.view == View::TypeLayer &&
               (rel.name == "child" || rel.name == "parent")) {
      // Parent links are derived from the deterministic path/depth relation.
      graph::GraphQuery graph(read_.graph_read());
      for (const auto &key : st.keys) {
        const auto layers = graph.type_layers(key.a);
        if (key.b < 0 || static_cast<std::size_t>(key.b) >= layers.size()) {
          continue;
        }
        const auto &child = layers[static_cast<std::size_t>(key.b)];
        for (std::size_t i = 0; i < layers.size(); ++i) {
          const auto &parent = layers[i];
          if (child.depth == parent.depth + 1 &&
              child.path.starts_with(parent.path + ".")) {
            add_synthetic({.a = key.a, .b = static_cast<int64_t>(i)});
          }
        }
      }
    } else if (inbound && st.view == View::Parameter &&
               rel.name == "has_parameter") {
      for (const auto &key : st.keys) {
        ids.push_back(key.a);
      }
    } else if (inbound && st.view == View::TemplateParameter &&
               rel.name == "has_template_parameter") {
      for (const auto &key : st.keys) {
        ids.push_back(key.a);
      }
    } else if (inbound && st.view == View::TemplateArgument &&
               rel.name == "has_template_argument") {
      for (const auto &key : st.keys) {
        ids.push_back(key.a);
      }
    } else if (inbound && st.view == View::Edge &&
               rel.name == "has_call_edge") {
      for (const auto &key : st.keys) {
        add_ids("SELECT src_id FROM edge WHERE id=?", {SqlValue(key.a)});
      }
    } else if (!inbound && st.view == View::Parameter) {
      for (const auto &key : st.keys) {
        if (rel.name == "of_type" || rel.name == "declared_type" ||
            rel.name == "adjusted_type") {
          const char *column = type_column(rel.name);
          add_ids(std::string("SELECT ") + column +
                      " FROM parameter WHERE owner_id=? AND position=? AND "
                      "pack_index=? AND " +
                      column + " IS NOT NULL",
                  {SqlValue(key.a), SqlValue(key.b), SqlValue(key.c)});
        } else if (rel.name == "references_symbol") {
          add_ids(
              "SELECT decl_id FROM type_node WHERE id=(SELECT type_id FROM "
              "parameter WHERE owner_id=? AND position=? AND pack_index=?) "
              "AND decl_id IS NOT NULL UNION SELECT symbol_id FROM "
              "symbol_type WHERE type_id=(SELECT type_id FROM parameter WHERE "
              "owner_id=? AND position=? AND pack_index=?) ORDER BY 1",
              {SqlValue(key.a), SqlValue(key.b), SqlValue(key.c),
               SqlValue(key.a), SqlValue(key.b), SqlValue(key.c)});
        } else if (rel.name == "has_evidence") {
          add_keys(View::Evidence,
                   "SELECT e.edge_id,e.file_id,COALESCE(e.line,0),"
                   "COALESCE(e.col,0) FROM edge_site e JOIN parameter p ON "
                   "p.file_id=e.file_id AND p.line=e.line AND p.col=e.col "
                   "WHERE p.owner_id=? AND p.position=? AND p.pack_index=? "
                   "ORDER BY e.edge_id,e.file_id,e.line,e.col",
                   {SqlValue(key.a), SqlValue(key.b), SqlValue(key.c)});
        }
      }
    } else if (inbound && st.view == View::Type &&
               (rel.name == "of_type" || rel.name == "declared_type" ||
                rel.name == "adjusted_type")) {
      const char *column = type_column(rel.name);
      View source = rel.layer;
      std::string table = "call_arg";
      std::string columns = "edge_id,file_id,line,col,position";
      if (source == View::Parameter) {
        table = "parameter";
        columns = "owner_id,position,pack_index";
      } else if (source == View::TemplateParameter) {
        table = "template_param";
        columns = "owner_id,position";
      } else if (source == View::TemplateArgument) {
        table = "template_arg";
        columns = "owner_id,position,pack_index";
      }
      for (const auto &key : st.keys) {
        if (source == View::Symbol) {
          add_ids("SELECT symbol_id FROM symbol_type WHERE type_id=? "
                  "ORDER BY symbol_id",
                  {SqlValue(key.a)});
        } else {
          std::string sql = "SELECT ";
          sql += columns;
          sql += " FROM ";
          sql += table;
          sql += " WHERE ";
          sql += column;
          sql += "=? ORDER BY ";
          sql += columns;
          add_keys(source, sql, {SqlValue(key.a)});
        }
      }
    } else if (!inbound && st.view == View::TemplateParameter) {
      for (const auto &key : st.keys) {
        if (rel.name == "of_type") {
          add_ids("SELECT type_id FROM template_param WHERE owner_id=? AND "
                  "position=? AND type_id IS NOT NULL",
                  {SqlValue(key.a), SqlValue(key.b)});
        } else if (rel.name == "has_default") {
          auto query = read_.read_db().prepare(
              "SELECT 1 FROM template_param WHERE owner_id=? AND "
              "position=? AND (default_txt IS NOT NULL OR "
              "default_type_id IS NOT NULL OR default_ref_id IS NOT NULL)");
          query.bind(1, key.a);
          query.bind(2, key.b);
          if (query.step()) {
            add_synthetic(LogicalKey{.a = key.a, .b = key.b, .tag = 1});
          }
        }
      }
    } else if (!inbound && st.view == View::TemplateArgument) {
      for (const auto &key : st.keys) {
        if (rel.name == "of_type") {
          add_ids("SELECT type_id FROM template_arg WHERE owner_id=? AND "
                  "position=? AND pack_index=? AND type_id IS NOT NULL",
                  {SqlValue(key.a), SqlValue(key.b), SqlValue(key.c)});
        } else if (rel.name == "references_symbol") {
          add_ids("SELECT ref_id FROM template_arg WHERE owner_id=? AND "
                  "position=? AND pack_index=? AND ref_id IS NOT NULL",
                  {SqlValue(key.a), SqlValue(key.b), SqlValue(key.c)});
        }
      }
    } else if (!inbound && st.view == View::Edge) {
      for (const auto &key : st.keys) {
        if (rel.name == "has_argument") {
          add_keys(View::CallArgument,
                   "SELECT edge_id,file_id,line,col,position FROM call_arg "
                   "WHERE edge_id=? ORDER BY file_id,line,col,position",
                   {SqlValue(key.a)});
        } else if (rel.name == "has_evidence") {
          add_keys(View::Evidence,
                   "SELECT edge_id,file_id,COALESCE(line,0),COALESCE(col,0) "
                   "FROM edge_site WHERE edge_id=? ORDER BY file_id,line,col",
                   {SqlValue(key.a)});
        } else if (rel.name == "has_site") {
          add_keys(View::Site,
                   "SELECT edge_id,file_id,COALESCE(line,0),COALESCE(col,0) "
                   "FROM edge_site WHERE edge_id=? ORDER BY file_id,line,col",
                   {SqlValue(key.a)});
        }
      }
    } else if (!inbound && st.view == View::CallArgument) {
      for (const auto &key : st.keys) {
        if (rel.name == "of_type") {
          add_ids("SELECT type_id FROM call_arg WHERE edge_id=? AND file_id=? "
                  "AND line=? AND col=? AND position=? AND type_id IS NOT NULL",
                  {SqlValue(key.a), SqlValue(key.b), SqlValue(key.c),
                   SqlValue(key.d), SqlValue(key.e)});
        } else if (rel.name == "references_symbol") {
          add_ids("SELECT decl_id FROM call_arg WHERE edge_id=? AND file_id=? "
                  "AND line=? AND col=? AND position=? AND decl_id IS NOT NULL",
                  {SqlValue(key.a), SqlValue(key.b), SqlValue(key.c),
                   SqlValue(key.d), SqlValue(key.e)});
        } else if (rel.name == "has_evidence") {
          add_keys(View::Evidence,
                   "SELECT edge_id,file_id,COALESCE(line,0),COALESCE(col,0) "
                   "FROM edge_site WHERE edge_id=? AND file_id=? AND line=? "
                   "AND col=?",
                   {SqlValue(key.a), SqlValue(key.b), SqlValue(key.c),
                    SqlValue(key.d)});
        }
      }
    } else if (inbound && st.view == View::Evidence) {
      for (const auto &key : st.keys) {
        if (rel.name == "has_evidence") {
          if (rel.layer == View::Edge) {
            add_keys(View::Edge,
                     "SELECT edge_id FROM edge_site WHERE edge_id=? "
                     "AND file_id=? AND line=? AND col=?",
                     {SqlValue(key.a), SqlValue(key.b), SqlValue(key.c),
                      SqlValue(key.d)});
          } else if (rel.layer == View::CallArgument) {
            add_keys(View::CallArgument,
                     "SELECT edge_id,file_id,line,col,position FROM call_arg "
                     "WHERE edge_id=? AND file_id=? AND line=? AND col=?",
                     {SqlValue(key.a), SqlValue(key.b), SqlValue(key.c),
                      SqlValue(key.d)});
          } else if (rel.layer == View::Symbol) {
            add_ids("SELECT src_id FROM edge WHERE id=?", {SqlValue(key.a)});
          } else if (rel.layer == View::Parameter) {
            add_keys(View::Parameter,
                     "SELECT owner_id,position,pack_index FROM parameter "
                     "WHERE file_id=? AND line=? AND col=?",
                     {SqlValue(key.b), SqlValue(key.c), SqlValue(key.d)});
          }
        }
      }
    } else if (inbound && st.view == View::Site) {
      for (const auto &key : st.keys) {
        if (rel.name == "has_site" || rel.name == "of_edge") {
          add_keys(View::Edge,
                   "SELECT edge_id FROM edge_site WHERE edge_id=? AND "
                   "file_id=? AND COALESCE(line,0)=? AND COALESCE(col,0)=?",
                   {SqlValue(key.a), SqlValue(key.b), SqlValue(key.c),
                    SqlValue(key.d)});
        }
      }
    } else if (inbound && st.view == View::Symbol &&
               rel.name == "references_symbol") {
      for (const auto symbol_id : st.ids) {
        if (rel.layer == View::Parameter) {
          add_keys(
              View::Parameter,
              "SELECT p.owner_id,p.position,p.pack_index FROM parameter p "
              "WHERE EXISTS (SELECT 1 FROM type_node t WHERE t.id=p.type_id "
              "AND t.decl_id=?) OR EXISTS (SELECT 1 FROM symbol_type st "
              "WHERE st.type_id=p.type_id AND st.symbol_id=?) ORDER BY "
              "p.owner_id,p.position,p.pack_index",
              {SqlValue(symbol_id), SqlValue(symbol_id)});
        } else if (rel.layer == View::TemplateArgument) {
          add_keys(View::TemplateArgument,
                   "SELECT owner_id,position,pack_index FROM template_arg "
                   "WHERE ref_id=? ORDER BY owner_id,position,pack_index",
                   {SqlValue(symbol_id)});
        } else if (rel.layer == View::CallArgument) {
          add_keys(View::CallArgument,
                   "SELECT edge_id,file_id,line,col,position FROM call_arg "
                   "WHERE decl_id=? ORDER BY edge_id,file_id,line,col,position",
                   {SqlValue(symbol_id)});
        } else if (rel.layer == View::Type) {
          add_ids("SELECT type_id FROM symbol_type WHERE symbol_id=? UNION "
                  "SELECT id FROM type_node WHERE decl_id=? ORDER BY 1",
                  {SqlValue(symbol_id), SqlValue(symbol_id)});
        }
      }
    } else if (inbound && st.view == View::Type &&
               rel.name == "has_type_edge") {
      for (const auto &key : st.keys) {
        add_ids("SELECT src_id FROM type_edge WHERE dst_id=? ORDER BY src_id",
                {SqlValue(key.a)});
      }
    } else if (inbound && st.view == View::CallArgument &&
               rel.name == "has_argument") {
      for (const auto &key : st.keys) {
        add_keys(View::Edge, "SELECT id FROM edge WHERE id=?",
                 {SqlValue(key.a)});
      }
    } else if (inbound && st.view == View::CallArgument &&
               rel.name == "of_occurrence") {
      for (const auto &key : st.keys) {
        add_keys(View::Evidence,
                 "SELECT edge_id,file_id,COALESCE(line,0),COALESCE(col,0) "
                 "FROM edge_site WHERE edge_id=? AND file_id=? AND "
                 "COALESCE(line,0)=? AND COALESCE(col,0)=?",
                 {SqlValue(key.a), SqlValue(key.b), SqlValue(key.c),
                  SqlValue(key.d)});
      }
    } else if (inbound && st.view == View::Edge && rel.name == "of_edge") {
      for (const auto &key : st.keys) {
        add_keys(rel.layer == View::Site ? View::Site : View::Evidence,
                 "SELECT edge_id,file_id,COALESCE(line,0),COALESCE(col,0) "
                 "FROM edge_site WHERE edge_id=? ORDER BY file_id,line,col",
                 {SqlValue(key.a)});
      }
    } else if (!inbound && st.view == View::Evidence) {
      for (const auto &key : st.keys) {
        if (rel.name == "of_edge") {
          add_keys(View::Edge,
                   "SELECT edge_id FROM edge_site WHERE edge_id=? "
                   "AND file_id=? AND line=? AND col=?",
                   {SqlValue(key.a), SqlValue(key.b), SqlValue(key.c),
                    SqlValue(key.d)});
        } else if (rel.name == "of_occurrence") {
          add_keys(View::CallArgument,
                   "SELECT edge_id,file_id,line,col,position FROM call_arg "
                   "WHERE edge_id=? AND file_id=? AND line=? AND col=?",
                   {SqlValue(key.a), SqlValue(key.b), SqlValue(key.c),
                    SqlValue(key.d)});
        }
      }
    } else if (!inbound && st.view == View::Site) {
      for (const auto &key : st.keys) {
        if (rel.name == "of_edge") {
          add_keys(View::Edge,
                   "SELECT edge_id FROM edge_site WHERE edge_id=? AND "
                   "file_id=? AND COALESCE(line,0)=? AND COALESCE(col,0)=?",
                   {SqlValue(key.a), SqlValue(key.b), SqlValue(key.c),
                    SqlValue(key.d)});
        }
      }
    } else if (!inbound && st.view == View::Type) {
      for (const auto &key : st.keys) {
        if (rel.name == "references_symbol") {
          add_ids("SELECT decl_id FROM type_node WHERE id=? AND decl_id IS NOT "
                  "NULL "
                  "UNION SELECT symbol_id FROM symbol_type WHERE type_id=? "
                  "ORDER BY 1",
                  {SqlValue(key.a), SqlValue(key.a)});
        } else if (rel.name == "has_type_edge") {
          add_ids(
              "SELECT dst_id FROM type_edge WHERE src_id=? ORDER BY position",
              {SqlValue(key.a)});
        }
      }
    }

    std::ranges::sort(ids);
    ids.erase(std::ranges::unique(ids).begin(), ids.end());
    std::ranges::sort(keys);
    keys.erase(std::ranges::unique(keys).begin(), keys.end());
    if (target == View::Type) {
      keys.reserve(keys.size() + ids.size());
      for (const auto id : ids) {
        keys.push_back(LogicalKey{.a = id});
      }
      std::ranges::sort(keys);
      keys.erase(std::ranges::unique(keys).begin(), keys.end());
    }
    const size_t result_size = target == View::Symbol || target == View::Entity
                                   ? ids.size()
                                   : keys.size();
    if (result_size > static_cast<size_t>(kTraverseNodeBudget)) {
      if (target == View::Symbol || target == View::Entity) {
        ids.resize(kTraverseNodeBudget);
      } else {
        keys.resize(kTraverseNodeBudget);
      }
      st.truncated = true;
    }
    if (target == View::Symbol || target == View::Entity) {
      st.ids = std::move(ids);
      st.keys.clear();
    } else {
      st.keys = std::move(keys);
      st.ids.clear();
    }
    st.view = target;
  }

  void traverse(Stream &st, const Stage &stage) {
    const bool inbound = stage.op == StageOp::In;
    const RelationDesc *rel =
        resolve_relation(stage.relation, st.view, inbound);
    if (rel->virtual_relation ||
        (rel->target_view != View::Symbol &&
         rel->target_view != View::Entity) ||
        (st.view != View::Symbol && st.view != View::Entity)) {
      traverse_typed(st, stage, *rel);
      return;
    }
    const bool entity_layer = rel->layer == View::Entity;
    const std::string table = entity_layer ? "entity_edge" : "edge";
    const bool outward = stage.op == StageOp::Out;
    const std::string from_col = outward ? "src_id" : "dst_id";
    const std::string to_col = outward ? "dst_id" : "src_id";

    std::vector<int64_t> frontier = st.ids;
    std::ranges::sort(frontier);
    frontier.erase(std::ranges::unique(frontier).begin(), frontier.end());
    std::set<int64_t> emitted;
    int64_t states = 0; // cumulative level sizes, bounded by the budget

    for (int64_t depth = 1; depth <= stage.max_depth && !frontier.empty();
         ++depth) {
      std::vector<int64_t> level;
      for (size_t at = 0; at < frontier.size(); at += kIdChunk) {
        const size_t n = std::min(kIdChunk, frontier.size() - at);
        std::string sql = "SELECT DISTINCT ";
        sql += to_col;
        sql += " FROM ";
        sql += table;
        sql += " WHERE kind = ? AND ";
        sql += from_col;
        sql += " IN (";
        sql += placeholders(n);
        sql += ") ORDER BY 1";
        std::vector<SqlValue> args;
        args.emplace_back(rel->kind_id);
        for (size_t i = 0; i < n; ++i) {
          args.emplace_back(frontier[at + i]);
        }
        auto part = fetch_ids(read_.read_db(), sql, args);
        level.insert(level.end(), part.begin(), part.end());
      }
      std::ranges::sort(level);
      level.erase(std::ranges::unique(level).begin(), level.end());
      if (states + static_cast<int64_t>(level.size()) > kTraverseNodeBudget) {
        level.resize(static_cast<size_t>(kTraverseNodeBudget - states));
        st.truncated = true;
      }
      states += static_cast<int64_t>(level.size());
      if (depth >= stage.min_depth) {
        emitted.insert(level.begin(), level.end());
      }
      if (st.truncated) {
        break;
      }
      frontier = std::move(level);
    }
    st.ids.assign(emitted.begin(), emitted.end());
    st.view = rel->layer;
  }

  void set_op(Stream &st, const Stage &stage) {
    Stream sub = run_plan(*stage.operand);
    st.truncated = st.truncated || sub.truncated;
    if (is_typed_view(st.view)) {
      std::vector<LogicalKey> left = st.keys;
      std::vector<LogicalKey> right = sub.keys;
      std::ranges::sort(left);
      std::ranges::sort(right);
      left.erase(std::ranges::unique(left).begin(), left.end());
      right.erase(std::ranges::unique(right).begin(), right.end());
      std::vector<LogicalKey> out;
      if (stage.op == StageOp::Union) {
        std::ranges::set_union(left, right, std::back_inserter(out));
      } else if (stage.op == StageOp::Intersect) {
        std::ranges::set_intersection(left, right, std::back_inserter(out));
      } else {
        std::ranges::set_difference(left, right, std::back_inserter(out));
      }
      if (out.size() > static_cast<size_t>(kTraverseNodeBudget)) {
        out.resize(kTraverseNodeBudget);
        st.truncated = true;
      }
      st.keys = std::move(out);
      st.ids.clear();
      return;
    }
    auto dedup = [](std::vector<int64_t> v) {
      std::ranges::sort(v);
      v.erase(std::ranges::unique(v).begin(), v.end());
      return v;
    };
    const std::vector<int64_t> a = dedup(st.ids);
    const std::vector<int64_t> b = dedup(sub.ids);
    std::vector<int64_t> out;
    // All three are SET operations (PR #20 review: union must not
    // double-count overlapping ids).
    if (stage.op == StageOp::Union) {
      std::ranges::set_union(a, b, std::back_inserter(out));
    } else if (stage.op == StageOp::Intersect) {
      std::ranges::set_intersection(a, b, std::back_inserter(out));
    } else {
      std::ranges::set_difference(a, b, std::back_inserter(out));
    }
    st.ids = std::move(out);
  }

  // Evidence for one witness hop, capped at the default result cap. `sites`
  // never silently drops rows past the cap without saying so: `truncated`
  // is set whenever the underlying edge has more sites than the cap, so the
  // caller can mark that hop/witness partial instead of presenting
  // incomplete evidence as complete (docs/query-plan.md).
  struct HopSites {
    std::vector<EdgeSiteRow> sites;
    bool truncated = false;
  };

  // Evidence for one witness hop over a non-typed symbol/entity relation.
  // Entity-layer relations are derived (no site rows); symbol-layer relations
  // look up the physical edge row for (parent,child) respecting direction.
  HopSites hop_sites(bool entity_layer, int64_t kind_id, bool inbound,
                     int64_t parent_id, int64_t child_id) {
    if (entity_layer) {
      return {};
    }
    const int64_t src = inbound ? child_id : parent_id;
    const int64_t dst = inbound ? parent_id : child_id;
    auto query = read_.read_db().prepare(
        "SELECT id FROM edge WHERE src_id=? AND dst_id=? AND kind=?");
    query.bind(1, src);
    query.bind(2, dst);
    query.bind(3, kind_id);
    if (!query.step()) {
      return {};
    }
    // Fetch one row past the cap to detect an over-cap hop without a
    // separate COUNT query.
    auto sites = read_.graph_read().edge_sites_one(
        query.col_int64(0), static_cast<int>(kDefaultResultCap) + 1);
    HopSites result;
    if (std::cmp_greater(sites.size(), kDefaultResultCap)) {
      sites.resize(kDefaultResultCap);
      result.truncated = true;
    }
    result.sites = std::move(sites);
    return result;
  }

  // Bounded deterministic shortest witness path(s): a multi-source BFS that
  // also records, per reached node, every predecessor reaching it at that
  // depth (the shortest-path DAG), so every minimal-depth witness -- not only
  // one -- can be reconstructed once the target set is first reached.
  void path_stage(Stream &st, const Stage &stage) {
    const RelationDesc *rel =
        resolve_relation(stage.relation, st.view, stage.inbound);
    const bool entity_layer = rel->layer == View::Entity;
    const std::string table = entity_layer ? "entity_edge" : "edge";
    const bool inbound = stage.inbound;
    const std::string from_col = inbound ? "dst_id" : "src_id";
    const std::string to_col = inbound ? "src_id" : "dst_id";

    Stream target_stream = run_plan(*stage.operand);
    const std::set<int64_t> targets(target_stream.ids.begin(),
                                    target_stream.ids.end());
    st.truncated = st.truncated || target_stream.truncated;

    std::vector<int64_t> starts = st.ids;
    std::ranges::sort(starts);
    starts.erase(std::ranges::unique(starts).begin(), starts.end());

    std::vector<PathWitness> results;
    int64_t budget_used = 0;
    bool truncated = false;
    bool result_truncated = false;
    bool evidence_truncated = false; // capped per-hop sites: partial, but the
                                     // search itself continues
    bool depth_limited = false; // a start's search was cut off by max_depth
                                // while its frontier was still expandable
    const std::string domain = entity_layer ? "entity" : "symbol";

    const auto frontier_expandable =
        [&](const std::vector<int64_t> &candidate_frontier) {
          for (size_t at = 0; at < candidate_frontier.size(); at += kIdChunk) {
            const size_t n = std::min(kIdChunk, candidate_frontier.size() - at);
            std::string sql = "SELECT 1 FROM ";
            sql += table;
            sql += " WHERE kind = ? AND ";
            sql += from_col;
            sql += " IN (";
            sql += placeholders(n);
            sql += ") LIMIT 1";
            auto query = read_.read_db().prepare(sql);
            query.bind(1, rel->kind_id);
            for (size_t i = 0; i < n; ++i) {
              query.bind(static_cast<int>(i + 2), candidate_frontier[at + i]);
            }
            if (query.step()) {
              return true;
            }
          }
          return false;
        };

    for (const int64_t start : starts) {
      if (truncated) {
        break;
      }
      std::vector<int64_t> frontier{start};
      // preds[d-1]: child -> sorted parent ids reaching it at depth d. No
      // cross-level visited set: a node can legitimately be reached again at
      // a later depth via a different route (path-length-window semantics,
      // matching out()/in()'s "some path of length d in [min,max]" rule --
      // docs/query-plan.md), so pruning a child once it is first seen would
      // silently discard an in-window witness reached only through it later.
      std::vector<std::map<int64_t, std::vector<int64_t>>> preds;
      int64_t found_depth = -1;
      // Whether the BFS ran out of graph to expand (a true dead end,
      // reached via the `parent_of.empty()` break below) before hitting
      // `max_depth`. When it is still false after the loop, the search
      // stopped only because the depth window closed while the frontier
      // was still expandable -- absence of a witness here does not prove
      // no path exists, so it must not be reported as a complete "not
      // found" (docs/query-plan.md).
      bool frontier_exhausted = false;
      for (int64_t depth = 1; depth <= stage.max_depth && !frontier.empty();
           ++depth) {
        std::map<int64_t, std::vector<int64_t>> parent_of;
        bool budget_exceeded = false;
        for (size_t at = 0; at < frontier.size() && !budget_exceeded;
             at += kIdChunk) {
          const size_t n = std::min(kIdChunk, frontier.size() - at);
          std::string sql = "SELECT ";
          sql += from_col;
          sql += ",";
          sql += to_col;
          sql += " FROM ";
          sql += table;
          sql += " WHERE kind = ? AND ";
          sql += from_col;
          sql += " IN (";
          sql += placeholders(n);
          sql += ") ORDER BY 1,2";
          std::vector<SqlValue> args;
          args.emplace_back(rel->kind_id);
          for (size_t i = 0; i < n; ++i) {
            args.emplace_back(frontier[at + i]);
          }
          auto query = read_.read_db().prepare(sql);
          for (size_t i = 0; i < args.size(); ++i) {
            query.bind(static_cast<int>(i + 1), args[i]);
          }
          // Check the budget on every row: a single level's frontier can be
          // far larger than the budget, and this must bound the rows read
          // (and stored in parent_of), not just notice the overflow after
          // the whole level has already been materialized.
          while (query.step()) {
            ++st.path_rows_examined;
            if (++budget_used > kPathNodeBudget) {
              budget_exceeded = true;
              break;
            }
            const int64_t parent = query.col_int64(0);
            const int64_t child = query.col_int64(1);
            parent_of[child].push_back(parent);
          }
        }
        if (budget_exceeded) {
          truncated = true;
          break;
        }
        if (parent_of.empty()) {
          frontier_exhausted = true;
          break;
        }
        for (auto &[child, parents] : parent_of) {
          std::ranges::sort(parents);
        }
        preds.push_back(parent_of);
        std::vector<int64_t> level;
        level.reserve(parent_of.size());
        for (const auto &[child, parents] : parent_of) {
          level.push_back(child);
        }
        std::ranges::sort(level);
        if (depth >= stage.min_depth &&
            std::ranges::any_of(
                level, [&](int64_t id) { return targets.contains(id); })) {
          found_depth = depth;
          break;
        }
        frontier = std::move(level);
      }
      if (truncated) {
        break;
      }
      if (found_depth < 0) {
        if (!frontier_exhausted && frontier_expandable(frontier)) {
          // Finite-depth exhaustion: the depth window (not a dead end in
          // the graph) is what stopped this start's search, so its "no
          // witness" result is unknown, not a proven negative. This does
          // not abort the search for other starts.
          depth_limited = true;
        }
        continue;
      }
      // Invert the predecessor DAG and enumerate complete witnesses forward
      // from the source. Children are sorted at every depth, so enumeration is
      // already lexicographic in the documented rank key's node-id sequence.
      // Keeping the first cap+1 complete chains proves truncation without ever
      // stopping a reconstruction halfway back to its real source.
      std::vector<std::map<int64_t, std::vector<int64_t>>> successors(
          static_cast<size_t>(found_depth));
      for (int64_t depth = 0; depth < found_depth; ++depth) {
        for (const auto &[child, parents] : preds[static_cast<size_t>(depth)]) {
          for (const int64_t parent : parents) {
            successors[static_cast<size_t>(depth)][parent].push_back(child);
          }
        }
        for (auto &[parent, children] :
             successors[static_cast<size_t>(depth)]) {
          std::ranges::sort(children);
          children.erase(std::ranges::unique(children).begin(), children.end());
        }
      }

      std::vector<std::vector<int64_t>> chains;
      std::vector<int64_t> chain{start};
      const auto enumerate = [&](const auto &self, size_t depth) -> void {
        if (std::cmp_greater(chains.size(), kDefaultResultCap)) {
          return;
        }
        if (std::cmp_equal(depth, found_depth)) {
          if (targets.contains(chain.back())) {
            chains.push_back(chain);
          }
          return;
        }
        const auto next = successors[depth].find(chain.back());
        if (next == successors[depth].end()) {
          return;
        }
        for (const int64_t child : next->second) {
          if (std::ranges::find(chain, child) != chain.end()) {
            continue;
          }
          chain.push_back(child);
          self(self, depth + 1);
          chain.pop_back();
          if (std::cmp_greater(chains.size(), kDefaultResultCap)) {
            break;
          }
        }
      };
      enumerate(enumerate, 0);
      if (std::cmp_greater(chains.size(), kDefaultResultCap)) {
        result_truncated = true;
        chains.resize(kDefaultResultCap);
      }

      for (const auto &complete_chain : chains) {
        PathWitness witness;
        witness.length = found_depth;
        bool witness_evidence_truncated = false;
        witness.status =
            rel->completeness == "complete" ? "complete" : "partial";
        for (size_t i = 0; i < complete_chain.size(); ++i) {
          PathStep step;
          step.node_id = complete_chain[i];
          step.domain = domain;
          step.inbound = inbound;
          step.status = rel->completeness;
          if (i > 0) {
            step.through = rel->name;
            HopSites hop = hop_sites(entity_layer, rel->kind_id, inbound,
                                     complete_chain[i - 1], complete_chain[i]);
            step.sites = std::move(hop.sites);
            if (hop.truncated) {
              step.status = "partial";
              witness_evidence_truncated = true;
            }
          }
          witness.steps.push_back(std::move(step));
        }
        if (witness_evidence_truncated) {
          witness.status = "partial";
          evidence_truncated = true; // incomplete evidence is never
                                     // presented as complete, but does not
                                     // abort the search for other starts
        }
        results.push_back(std::move(witness));
      }
    }

    truncated = truncated || result_truncated;
    sort_and_cap_witnesses(results, stage.n, truncated);
    st.paths = std::move(results);
    st.truncated =
        st.truncated || truncated || evidence_truncated || depth_limited;
    st.ids.clear();
    st.keys.clear();
    st.shape = Shape::Path;
  }

  static bool witness_less(const PathWitness &a, const PathWitness &b) {
    if (a.length != b.length) {
      return a.length < b.length;
    }
    for (size_t i = 0; i < a.steps.size() && i < b.steps.size(); ++i) {
      const PathStep &sa = a.steps[i];
      const PathStep &sb = b.steps[i];
      if (sa.node_id != sb.node_id) {
        return sa.node_id < sb.node_id;
      }
      if (sa.domain != sb.domain) {
        return sa.domain < sb.domain;
      }
      if (sa.through != sb.through) {
        return sa.through < sb.through;
      }
      if (sa.inbound != sb.inbound) {
        return !sa.inbound && sb.inbound;
      }
      if (sa.position != sb.position) {
        return sa.position < sb.position;
      }
      if (sa.pack_index != sb.pack_index) {
        return sa.pack_index < sb.pack_index;
      }
    }
    return a.steps.size() < b.steps.size();
  }

  // Deterministic default/rerank order for a Path stream: shortest-first,
  // ties broken by each step's full logical typed-step identity (see below).
  static void sort_and_cap_witnesses(std::vector<PathWitness> &results,
                                     int64_t cap, bool &truncated) {
    // Ties are broken lexicographically over each step's full logical typed-
    // step identity -- (node_id, domain, through, inbound, position,
    // pack_index): a total order even when two witnesses share the same
    // node-id sequence but differ only by which relation/type_edge hop
    // reached a node (e.g. `member_owner` vs `member_component`, both
    // landing on the same node at the same position) or which typed-view
    // slot (e.g. parameter position) the final owner step names
    // (docs/query-plan.md). `inbound` is included so this key matches
    // apply_distinct()'s step identity -- it is a no-op today (`step.inbound`
    // is constant per stage call, and reverse_type_use() never sets it), but
    // the two must not silently diverge if that ever changes.
    std::ranges::stable_sort(results, witness_less);
    if (cap > 0 && std::cmp_greater(results.size(), cap)) {
      results.resize(static_cast<size_t>(cap));
    }
    if (std::cmp_greater(results.size(), kDefaultResultCap)) {
      results.resize(kDefaultResultCap);
      truncated = true;
    }
  }

  static void rank_stage(Stream &st, const Stage &stage) {
    bool truncated = st.truncated;
    sort_and_cap_witnesses(st.paths, stage.n, truncated);
    st.truncated = truncated;
  }

  // One owner (declaration whose signature/template facts reach `type_id`
  // directly, with no further nesting) across every owner-fact domain,
  // carrying that view's own natural-key identity (position/pack_index) and
  // declaration site so distinct slots on the same owner symbol are never
  // collapsed together (e.g. two `int` parameters at positions 0 and 1).
  struct TypeOwner {
    std::string domain;      // "symbol" | "parameter" | "template_parameter" |
                             // "template_argument"
    std::string role;        // symbol_type role ("returns"/"of_type"/
                             // "underlying_type"), else same as domain
    int64_t node_id = 0;     // owning declaration's symbol id
    int64_t position = -1;   // natural-key position; -1 = not applicable
    int64_t pack_index = -1; // pack element index; -1 = not a pack slot
    std::optional<int64_t> file_id;
    std::optional<int64_t> line;
    std::optional<int64_t> col;
  };

  std::vector<TypeOwner> owners_of_type(int64_t type_id) {
    std::vector<TypeOwner> out;
    {
      // Join the owning symbol's own declaration/definition site (symbol.
      // file_id/line/col) so a direct symbol-domain owner carries the same
      // provenance every other owner domain (parameter/template_parameter/
      // template_argument) already reports -- a bare symbol_id with no site
      // is not enough evidence to present as a witness step.
      auto query = read_.read_db().prepare(
          "SELECT st.symbol_id, st.kind, s.file_id, s.line, s.col "
          "FROM symbol_type st LEFT JOIN symbol s ON s.id = st.symbol_id "
          "WHERE st.type_id=? ORDER BY st.symbol_id, st.kind");
      query.bind(1, type_id);
      while (query.step()) {
        const int64_t kind = query.col_int64(1);
        const char *role = "underlying_type";
        if (kind == 1) {
          role = "returns";
        } else if (kind == 2) {
          role = "of_type";
        }
        TypeOwner owner{.domain = "symbol",
                        .role = role,
                        .node_id = query.col_int64(0),
                        .position = -1,
                        .pack_index = -1,
                        .file_id = std::nullopt,
                        .line = std::nullopt,
                        .col = std::nullopt};
        if (!query.col_is_null(2)) {
          owner.file_id = query.col_int64(2);
        }
        if (!query.col_is_null(3)) {
          owner.line = query.col_int64(3);
        }
        if (!query.col_is_null(4)) {
          owner.col = query.col_int64(4);
        }
        out.push_back(std::move(owner));
      }
    }
    {
      auto query = read_.read_db().prepare(
          "SELECT owner_id, position, pack_index, file_id, line, col FROM "
          "parameter WHERE type_id=? OR declared_type_id=? OR "
          "adjusted_type_id=? ORDER BY owner_id, position, pack_index");
      query.bind(1, type_id);
      query.bind(2, type_id);
      query.bind(3, type_id);
      while (query.step()) {
        TypeOwner owner{.domain = "parameter",
                        .role = "parameter",
                        .node_id = query.col_int64(0),
                        .position = query.col_int64(1),
                        .pack_index = query.col_int64(2),
                        .file_id = std::nullopt,
                        .line = std::nullopt,
                        .col = std::nullopt};
        if (!query.col_is_null(3)) {
          owner.file_id = query.col_int64(3);
        }
        if (!query.col_is_null(4)) {
          owner.line = query.col_int64(4);
        }
        if (!query.col_is_null(5)) {
          owner.col = query.col_int64(5);
        }
        out.push_back(std::move(owner));
      }
    }
    {
      auto query = read_.read_db().prepare(
          "SELECT owner_id, position FROM template_param WHERE type_id=? "
          "ORDER BY owner_id, position");
      query.bind(1, type_id);
      while (query.step()) {
        out.push_back({.domain = "template_parameter",
                       .role = "template_parameter",
                       .node_id = query.col_int64(0),
                       .position = query.col_int64(1),
                       .pack_index = -1,
                       .file_id = std::nullopt,
                       .line = std::nullopt,
                       .col = std::nullopt});
      }
    }
    {
      auto query = read_.read_db().prepare(
          "SELECT owner_id, position, pack_index FROM template_arg WHERE "
          "type_id=? ORDER BY owner_id, position, pack_index");
      query.bind(1, type_id);
      while (query.step()) {
        out.push_back({.domain = "template_argument",
                       .role = "template_argument",
                       .node_id = query.col_int64(0),
                       .position = query.col_int64(1),
                       .pack_index = query.col_int64(2),
                       .file_id = std::nullopt,
                       .line = std::nullopt,
                       .col = std::nullopt});
      }
    }
    return out;
  }

  // Typed reverse type-use: from a seed `type`/`type_layer` id, climb
  // type_edge (structural nesting: pointee/element_type/return_type/
  // param_type/template_argument_type/member_owner/member_component) and
  // type_node.canonical_id (cv/sugar desugaring) backward, emitting one
  // witness per owner found at every depth -- direct use at depth 0 and
  // every nested layer up to max_depth. Mirrors graph::GraphQuery::
  // type_users()'s type_ids_reaching() closure (type_edge + canonical_id,
  // both backward) but keeps the per-branch ordered `through` chain that a
  // flat closure set cannot carry.
  void reverse_type_use_stage(Stream &st, const Stage &stage) {
    std::vector<int64_t> seeds;
    if (st.view == View::Type) {
      for (const auto &key : st.keys) {
        seeds.push_back(key.a); // View::Type keys are the type_node id itself
      }
    } else {
      graph::GraphQuery graph(read_.graph_read());
      for (const auto &key : st.keys) {
        const auto layers = graph.type_layers(key.a);
        if (key.b >= 0 && static_cast<size_t>(key.b) < layers.size()) {
          seeds.push_back(layers[static_cast<size_t>(key.b)].type.id);
        }
      }
    }
    std::ranges::sort(seeds);
    seeds.erase(std::ranges::unique(seeds).begin(), seeds.end());

    // One climbed layer's identity: the type reached, the type_edge_kind
    // label that reached it, and (for a type_edge hop) that edge's own
    // `position` -- the natural-key column distinguishing e.g. a function
    // type's several `param_type` edges to the same pointee type from one
    // another. -1 for the seed link and for a `sugared_by` (canonical_id)
    // hop, neither of which has a position.
    struct ChainLink {
      int64_t type_id = 0;
      std::string through;
      int64_t position = -1;
    };

    struct Frame {
      int64_t type_id = 0;
      int64_t depth = 0;
      std::vector<ChainLink> chain;
    };

    std::multiset<PathWitness,
                  bool (*)(const PathWitness &, const PathWitness &)>
        best_results(witness_less);
    int64_t budget_used = 0;
    bool truncated = false;
    bool result_truncated = false;
    // A frame's climb cut off by `max_depth` while its own parents (the
    // type_edge + canonical_id climb) were still non-empty means "no owner
    // beyond this point" is unknown, not a proven negative -- mirrors
    // path_stage's frontier_exhausted/depth_limited pattern
    // (docs/query-plan.md). Does not abort the climb for other frames/seeds.
    bool depth_limited = false;

    for (const int64_t seed : seeds) {
      if (truncated) {
        break;
      }
      std::vector<Frame> stack;
      stack.push_back(
          {.type_id = seed,
           .depth = 0,
           .chain = {{.type_id = seed, .through = "", .position = -1}}});
      while (!stack.empty() && !truncated) {
        Frame frame = std::move(stack.back());
        stack.pop_back();
        if (++budget_used > kPathNodeBudget) {
          truncated = true;
          break;
        }
        for (const auto &owner : owners_of_type(frame.type_id)) {
          PathWitness witness;
          witness.length = frame.depth + 1;
          witness.status = "partial"; // of_type/parameter/... are cataloged
                                      // partial
          for (const auto &link : frame.chain) {
            PathStep step;
            step.node_id = link.type_id;
            step.domain = "type";
            step.through = link.through;
            step.status = "complete"; // structural type nesting is complete
            step.position = link.position;
            witness.steps.push_back(std::move(step));
          }
          PathStep final_step;
          final_step.node_id = owner.node_id;
          final_step.domain = owner.domain;
          final_step.through = owner.role;
          final_step.status = "partial";
          final_step.position = owner.position;
          final_step.pack_index = owner.pack_index;
          if (owner.file_id || owner.line || owner.col) {
            EdgeSiteRow site;
            site.file_id = owner.file_id;
            site.line = owner.line;
            site.col = owner.col;
            final_step.sites.push_back(std::move(site));
          }
          witness.steps.push_back(std::move(final_step));
          best_results.insert(std::move(witness));
          if (std::cmp_greater(best_results.size(), kDefaultResultCap)) {
            // best_results is ordered ascending by witness_less (best
            // witness first), so the element to evict once the set is over
            // capacity is the current worst -- the last one -- never the
            // first. Evicting begin() here would silently keep the K worst
            // witnesses discovered instead of the K best, which the
            // ranks_before_its_internal_result_cap regression below proves.
            best_results.erase(std::prev(best_results.end()));
            result_truncated = true;
          }
        }
        // (parent_id, through label, type_edge.position or -1 when the hop
        // has none). A function type's several `param_type` edges to the
        // same pointee share (parent_id, through) but differ by position --
        // without it, two such climbs would be indistinguishable. Queried
        // unconditionally, even once `frame.depth` has already reached
        // `max_depth`, so a depth-cut-off frame can be told apart from a
        // true dead end (see depth_limited below).
        std::vector<ChainLink> parents;
        {
          auto query = read_.read_db().prepare(
              "SELECT src_id, kind, position FROM type_edge WHERE dst_id=? "
              "ORDER BY src_id, position");
          query.bind(1, frame.type_id);
          while (query.step()) {
            parents.push_back(
                {.type_id = query.col_int64(0),
                 .through = type_edge_kind_name(query.col_int64(1)),
                 .position = query.col_int64(2)});
          }
        }
        {
          auto query = read_.read_db().prepare(
              "SELECT id FROM type_node WHERE canonical_id=? ORDER BY id");
          query.bind(1, frame.type_id);
          while (query.step()) {
            parents.push_back({.type_id = query.col_int64(0),
                               .through = "sugared_by",
                               .position = -1});
          }
        }
        // Parents already present in the climb chain are proven dead ends
        // (structural nesting is acyclic in practice; this guard exists so a
        // data anomaly cannot loop forever) -- filter them out once, before
        // the frontier check, so the same set is used both to decide
        // depth_limited and to expand.
        std::vector<ChainLink> climbable_parents;
        for (const auto &parent : parents) {
          if (!std::ranges::any_of(frame.chain, [&](const ChainLink &link) {
                return link.type_id == parent.type_id;
              })) {
            climbable_parents.push_back(parent);
          }
        }
        if (frame.depth >= stage.max_depth) {
          if (!climbable_parents.empty()) {
            // The climb still had parents to explore, but max_depth cut it
            // off first: "no owner beyond here" is unknown, not a proven
            // negative.
            depth_limited = true;
          }
          continue;
        }
        for (const auto &parent : climbable_parents) {
          Frame next;
          next.type_id = parent.type_id;
          next.depth = frame.depth + 1;
          next.chain = frame.chain;
          next.chain.push_back(parent);
          stack.push_back(std::move(next));
        }
      }
    }

    std::vector<PathWitness> results(best_results.begin(), best_results.end());
    truncated = truncated || result_truncated;
    sort_and_cap_witnesses(results, 0, truncated);
    st.paths = std::move(results);
    st.truncated = st.truncated || truncated || depth_limited;
    st.ids.clear();
    st.keys.clear();
    st.shape = Shape::Path;
  }

  std::optional<std::string> file_path(int64_t file_id) {
    auto it = file_paths_.find(file_id);
    if (it == file_paths_.end()) {
      it = file_paths_.emplace(file_id, read_.file_abs_path(file_id)).first;
    }
    return it->second;
  }

  std::string portable_symbol(int64_t id) {
    auto query = read_.read_db().prepare(
        "SELECT COALESCE(su.key,''),s.identity_key,s.usr FROM symbol s "
        "LEFT JOIN semantic_universe su ON su.id=s.semantic_universe_id "
        "WHERE s.id=?");
    query.bind(1, id);
    if (!query.step()) {
      return "missing-symbol:" + std::to_string(id);
    }
    const std::string identity = query.col_text(1);
    const std::string usr = query.col_text(2);
    return identity.empty() ? query.col_text(0) + "\x1f" + usr : identity;
  }

  std::string portable_file(int64_t id) {
    auto query = read_.read_db().prepare(
        "SELECT c.name,c.path,d.path,f.name,r.name,r.remote_url,su.key "
        "FROM file f "
        "JOIN directory d ON d.id=f.directory_id "
        "JOIN component c ON c.id=d.component_id "
        "LEFT JOIN repository r ON r.id=c.repository_id "
        "LEFT JOIN semantic_universe su ON "
        "su.id=COALESCE(c.semantic_universe_id,"
        "r.semantic_universe_id,1) WHERE f.id=?");
    query.bind(1, id);
    if (!query.step()) {
      return "missing-file:" + std::to_string(id);
    }
    std::string owner;
    if (!query.col_text(5).empty()) {
      owner = "remote:" + query.col_text(5);
    } else if (!query.col_text(4).empty()) {
      owner = "repo:" + query.col_text(4);
    } else {
      owner = "universe:" +
              (query.col_text(6).empty() ? "legacy" : query.col_text(6));
    }
    const std::string component =
        pathutil::isabs(query.col_text(1))
            ? "ungrouped:" + query.col_text(0)
            : "grouped:" + query.col_text(0) + "\x1f" + query.col_text(1);
    std::string relative;
    for (int column = 2; column < 4; ++column) {
      const auto raw = query.col_text(column);
      const auto first = raw.find_first_not_of('/');
      if (first != std::string::npos) {
        const auto last = raw.find_last_not_of('/');
        const auto part = raw.substr(first, last - first + 1);
        if (!relative.empty()) {
          relative += "/";
        }
        relative += part;
      }
    }
    return "file:" + identity_segment(owner) + identity_segment(component) +
           identity_segment(relative);
  }

  std::string portable_type(int64_t id) {
    auto query = read_.read_db().prepare(
        "SELECT type_key,spelling FROM type_node WHERE id=?");
    query.bind(1, id);
    if (!query.step()) {
      return "missing-type:" + std::to_string(id);
    }
    const auto type_key = query.col_text(0);
    return type_key.empty() ? query.col_text(1) : type_key;
  }

  std::string portable_edge(int64_t id) {
    auto query = read_.read_db().prepare(
        "SELECT src_id,dst_id,kind FROM edge WHERE id=?");
    query.bind(1, id);
    if (!query.step()) {
      return "missing-edge:" + std::to_string(id);
    }
    return portable_symbol(query.col_int64(0)) + ":" +
           std::to_string(query.col_int64(2)) + ":" +
           portable_symbol(query.col_int64(1));
  }

  std::string logical_identity(View view, const LogicalKey &key) {
    if (view == View::Parameter) {
      return "parameter:" + portable_symbol(key.a) + ":" +
             std::to_string(key.b) + ":" + std::to_string(key.c);
    }
    if (view == View::TemplateParameter) {
      return "template_parameter:" + portable_symbol(key.a) + ":" +
             std::to_string(key.b);
    }
    if (view == View::TemplateArgument) {
      return "template_argument:" + portable_symbol(key.a) + ":" +
             std::to_string(key.b) + ":" + std::to_string(key.c);
    }
    if (view == View::CallArgument) {
      return "call_argument:" + portable_edge(key.a) + ":" +
             portable_file(key.b) + ":" + std::to_string(key.c) + ":" +
             std::to_string(key.d) + ":" + std::to_string(key.e);
    }
    if (view == View::Evidence) {
      if (key.tag == 1) {
        return "evidence:template_default:" + portable_symbol(key.a) + ":" +
               std::to_string(key.b);
      }
      return "evidence:" + portable_edge(key.a) + ":" + portable_file(key.b) +
             ":" + std::to_string(key.c) + ":" + std::to_string(key.d);
    }
    if (view == View::Site) {
      return "site:" + portable_edge(key.a) + ":" + portable_file(key.b) + ":" +
             std::to_string(key.c) + ":" + std::to_string(key.d);
    }
    if (view == View::Edge) {
      return "edge:" + portable_edge(key.a);
    }
    if (view == View::Type) {
      return "type:" + portable_type(key.a);
    }
    if (view == View::SignatureSlot) {
      return "signature_slot:" + portable_symbol(key.a) + ":" +
             std::to_string(key.b) + ":" + std::to_string(key.c) + ":" +
             std::to_string(key.tag);
    }
    if (view == View::TypeLayer) {
      return "type_layer:" + portable_type(key.a) + ":" + std::to_string(key.b);
    }
    return std::string(view_name(view)) + ":" + std::to_string(key.a);
  }

  int64_t logical_row_id(View view, const LogicalKey &key) {
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char ch : logical_identity(view, key)) {
      hash ^= ch;
      hash *= 1099511628211ULL;
    }
    return static_cast<int64_t>(hash & 0x7fffffffffffffffULL);
  }

  static std::string typed_table(View view) {
    switch (view) {
    case View::Parameter:
      return "parameter";
    case View::TemplateParameter:
      return "template_param";
    case View::TemplateArgument:
      return "template_arg";
    case View::CallArgument:
      return "call_arg";
    case View::Edge:
      return "edge";
    case View::Evidence:
    case View::Site:
      return "edge_site";
    case View::Type:
      return "type_node";
    case View::SignatureSlot:
    case View::TypeLayer:
      return "";
    case View::Symbol:
    case View::Entity:
      break;
    }
    return "";
  }

  static std::string typed_column(View view, const std::string &field) {
    if (field == "file") {
      return "file_id";
    }
    if (field == "cv_qualifiers") {
      return "(is_const + 2 * is_volatile + 4 * is_restrict)";
    }
    if (field == "id" || field == "identity_key") {
      if (field == "id" && view == View::Edge) {
        return "edge.id";
      }
      return "";
    }
    if (field == "edge_id" && view == View::Edge) {
      return "edge.id";
    }
    const std::string table = typed_table(view);
    const std::set<std::string> allowed = [&] {
      if (view == View::Parameter) {
        return std::set<std::string>{"owner_id",
                                     "position",
                                     "pack_index",
                                     "name",
                                     "type_id",
                                     "declared_type_id",
                                     "adjusted_type_id",
                                     "default_text",
                                     "default_origin",
                                     "reference_semantics",
                                     "file_id",
                                     "line",
                                     "col"};
      }
      if (view == View::TemplateParameter) {
        return std::set<std::string>{
            "owner_id",    "position", "param_kind",      "name",
            "default_txt", "type_id",  "default_type_id", "default_ref_id"};
      }
      if (view == View::TemplateArgument) {
        return std::set<std::string>{"owner_id", "position", "pack_index",
                                     "arg_kind", "ref_id",   "literal",
                                     "type_id"};
      }
      if (view == View::CallArgument) {
        return std::set<std::string>{
            "edge_id",  "file_id",   "line",         "col",        "position",
            "src_kind", "type_usr",  "decl_usr",     "callee_usr", "type_id",
            "decl_id",  "callee_id", "type_is_value"};
      }
      if (view == View::Evidence) {
        return std::set<std::string>{"edge_id",
                                     "file_id",
                                     "line",
                                     "col",
                                     "conditional",
                                     "args_sig",
                                     "recv_src_kind",
                                     "recv_type_usr",
                                     "recv_decl_usr",
                                     "recv_type_id",
                                     "recv_decl_id",
                                     "recv_param_pos",
                                     "recv_type_is_value"};
      }
      if (view == View::Site) {
        return std::set<std::string>{"edge_id", "file_id", "line", "col"};
      }
      if (view == View::Type) {
        return std::set<std::string>{"id",           "type_key", "spelling",
                                     "kind",         "is_const", "is_volatile",
                                     "is_restrict",  "decl_usr", "decl_id",
                                     "canonical_id", "extent"};
      }
      if (view == View::SignatureSlot) {
        return std::set<std::string>{"owner_id",
                                     "position",
                                     "pack_index",
                                     "slot_kind",
                                     "name",
                                     "type_id",
                                     "declared_type_id",
                                     "adjusted_type_id",
                                     "default_text",
                                     "default_origin",
                                     "reference_semantics"};
      }
      if (view == View::TypeLayer) {
        return std::set<std::string>{"root_id",  "path",        "relation",
                                     "position", "depth",       "status",
                                     "type_id",  "spelling",    "kind",
                                     "extent",   "decl_usr",    "canonical_id",
                                     "is_const", "is_volatile", "is_restrict"};
      }
      if (view == View::Edge) {
        return std::set<std::string>{"id",         "src_id",     "dst_id",
                                     "kind",       "count",      "base_access",
                                     "is_virtual", "vtable_slot"};
      }
      return std::set<std::string>{};
    }();
    if (allowed.contains(field)) {
      return table + "." + field;
    }
    return "";
  }

  static std::vector<SqlValue> logical_args(View view, const LogicalKey &key) {
    switch (view) {
    case View::Parameter:
    case View::TemplateArgument:
      return {SqlValue(key.a), SqlValue(key.b), SqlValue(key.c)};
    case View::TemplateParameter:
      return {SqlValue(key.a), SqlValue(key.b)};
    case View::CallArgument:
      return {SqlValue(key.a), SqlValue(key.b), SqlValue(key.c),
              SqlValue(key.d), SqlValue(key.e)};
    case View::Site:
    case View::Evidence:
      return {SqlValue(key.a), SqlValue(key.b), SqlValue(key.c),
              SqlValue(key.d)};
    case View::Edge:
    case View::Type:
      return {SqlValue(key.a)};
    case View::SignatureSlot:
      return {};
    case View::TypeLayer:
      return {};
    case View::Symbol:
    case View::Entity:
      return {};
    }
    return {};
  }

  static std::string logical_where(View view) {
    switch (view) {
    case View::Parameter:
    case View::TemplateArgument:
      return "owner_id=? AND position=? AND pack_index=?";
    case View::TemplateParameter:
      return "owner_id=? AND position=?";
    case View::CallArgument:
      return "edge_id=? AND file_id=? AND line=? AND col=? AND position=?";
    case View::Site:
    case View::Evidence:
      return "edge_id=? AND file_id=? AND line=? AND col=?";
    case View::Edge:
    case View::Type:
      return "id=?";
    case View::SignatureSlot:
    case View::TypeLayer:
      return "1=0";
    case View::Symbol:
    case View::Entity:
      return "1=0";
    }
    return "1=0";
  }

  std::optional<Cell> derived_typed_cell(View view, const LogicalKey &key,
                                         const std::string &field) {
    const bool derived = field == "relation" || field == "source" ||
                         field == "target" || field == "evidence" ||
                         field == "status" || field == "partial" ||
                         field == "unknown";
    if (!derived) {
      return std::nullopt;
    }
    if (view == View::Evidence && key.tag == 1) {
      if (field == "evidence") {
        return Cell(std::string("declaration"));
      }
      if (field == "status") {
        return Cell(std::string("partial"));
      }
      if (field == "partial") {
        return Cell(int64_t{1});
      }
      if (field == "unknown") {
        return Cell(int64_t{0});
      }
      return Cell(nullptr);
    }
    if (view != View::Edge && view != View::Site && view != View::Evidence) {
      return Cell(nullptr);
    }
    auto query = read_.read_db().prepare("SELECT kind FROM edge WHERE id=?");
    query.bind(1, key.a);
    if (!query.step()) {
      return field == "unknown" ? Cell(int64_t{1}) : Cell(nullptr);
    }
    const int64_t kind = query.col_int64(0);
    const auto &relation = relation_catalog();
    const auto it =
        std::ranges::find_if(relation, [kind](const RelationDesc &item) {
          return item.layer == View::Symbol && item.kind_id == kind;
        });
    if (it == relation.end()) {
      return field == "unknown" ? Cell(int64_t{1}) : Cell(nullptr);
    }
    const bool unresolved = has_unresolved_typed_provenance(view, key);
    if (field == "relation") {
      return Cell(it->name);
    }
    if (field == "source") {
      return Cell(it->source);
    }
    if (field == "target") {
      return Cell(it->target);
    }
    if (field == "evidence") {
      return Cell(it->evidence);
    }
    if (field == "status") {
      return Cell(unresolved ? "unknown" : it->completeness);
    }
    if (field == "partial") {
      return Cell(int64_t{!unresolved && it->completeness == "partial"});
    }
    return Cell(int64_t{unresolved || it->completeness == "unknown"});
  }

  bool has_unresolved_typed_provenance(View view, const LogicalKey &key) {
    if (view == View::Evidence && key.tag == 1) {
      return false;
    }
    auto edge =
        read_.read_db().prepare("SELECT src_id,dst_id FROM edge WHERE id=?");
    edge.bind(1, key.a);
    if (!edge.step()) {
      return true;
    }
    const int64_t src_id = edge.col_int64(0);
    const int64_t dst_id = edge.col_int64(1);
    auto endpoints = read_.read_db().prepare(
        "SELECT 1 FROM symbol WHERE id IN (?,?) AND resolved=0 LIMIT 1");
    endpoints.bind(1, src_id);
    endpoints.bind(2, dst_id);
    if (endpoints.step()) {
      return true;
    }
    const bool site_scoped =
        view == View::Site || (view == View::Evidence && key.tag == 0);
    const std::string site_scope = site_scoped ? " AND es.file_id=? AND "
                                                 "COALESCE(es.line,0)=? AND "
                                                 "COALESCE(es.col,0)=?"
                                               : "";
    auto sites = read_.read_db().prepare(
        "SELECT 1 FROM edge_site es "
        "LEFT JOIN external_identity eti ON eti.id=es.recv_type_identity_id "
        "LEFT JOIN external_identity edi ON edi.id=es.recv_decl_identity_id "
        "LEFT JOIN symbol ds ON ds.id=es.recv_decl_id "
        "WHERE es.edge_id=? AND ("
        "(es.recv_type_usr IS NOT NULL AND es.recv_type_id IS NULL AND "
        " es.recv_type_identity_id IS NULL) OR "
        "(es.recv_type_identity_id IS NOT NULL AND "
        " COALESCE(eti.resolution_status,0)=0) OR "
        "(es.recv_decl_usr IS NOT NULL AND "
        " (es.recv_decl_id IS NULL OR COALESCE(ds.resolved,0)=0) AND "
        " es.recv_decl_identity_id IS NULL) OR "
        "(es.recv_decl_identity_id IS NOT NULL AND "
        " COALESCE(edi.resolution_status,0)=0))" +
        site_scope + " LIMIT 1");
    sites.bind(1, key.a);
    if (site_scoped) {
      sites.bind(2, key.b);
      sites.bind(3, key.c);
      sites.bind(4, key.d);
    }
    if (sites.step()) {
      return true;
    }
    const std::string argument_scope = site_scoped ? " AND ca.file_id=? AND "
                                                     "ca.line=? AND ca.col=?"
                                                   : "";
    auto args = read_.read_db().prepare(
        "SELECT 1 FROM call_arg ca "
        "LEFT JOIN external_identity ati ON ati.id=ca.type_identity_id "
        "LEFT JOIN external_identity adi ON adi.id=ca.decl_identity_id "
        "LEFT JOIN external_identity aci ON aci.id=ca.callee_identity_id "
        "LEFT JOIN symbol ds ON ds.id=ca.decl_id "
        "LEFT JOIN symbol cs ON cs.id=ca.callee_id "
        "WHERE ca.edge_id=? AND ("
        "(ca.type_usr IS NOT NULL AND ca.type_id IS NULL AND "
        " ca.type_identity_id IS NULL) OR "
        "(ca.type_identity_id IS NOT NULL AND "
        " COALESCE(ati.resolution_status,0)=0) OR "
        "(ca.decl_usr IS NOT NULL AND "
        " (ca.decl_id IS NULL OR COALESCE(ds.resolved,0)=0) AND "
        " ca.decl_identity_id IS NULL) OR "
        "(ca.decl_identity_id IS NOT NULL AND "
        " COALESCE(adi.resolution_status,0)=0) OR "
        "(ca.callee_usr IS NOT NULL AND "
        " (ca.callee_id IS NULL OR COALESCE(cs.resolved,0)=0) AND "
        " ca.callee_identity_id IS NULL) OR "
        "(ca.callee_identity_id IS NOT NULL AND "
        " COALESCE(aci.resolution_status,0)=0))" +
        argument_scope + " LIMIT 1");
    args.bind(1, key.a);
    if (site_scoped) {
      args.bind(2, key.b);
      args.bind(3, key.c);
      args.bind(4, key.d);
    }
    return args.step();
  }

  Stream::RowStatus status_for_key(View view, const LogicalKey &key) {
    Stream::RowStatus status;
    const auto partial = derived_typed_cell(view, key, "partial");
    const auto unknown = derived_typed_cell(view, key, "unknown");
    status.partial = partial && std::holds_alternative<int64_t>(*partial) &&
                     std::get<int64_t>(*partial) != 0;
    status.unknown = unknown && std::holds_alternative<int64_t>(*unknown) &&
                     std::get<int64_t>(*unknown) != 0;
    return status;
  }

  void recompute_status(Stream &st) {
    st.partial = false;
    st.unknown = false;
    if (st.shape == Shape::Rows || !st.row_status.empty()) {
      for (const auto &status : st.row_status) {
        st.partial = st.partial || status.partial;
        st.unknown = st.unknown || status.unknown;
      }
      return;
    }
    for (const auto &key : st.keys) {
      const auto status = status_for_key(st.view, key);
      st.partial = st.partial || status.partial;
      st.unknown = st.unknown || status.unknown;
    }
  }

  std::map<LogicalKey, std::vector<Cell>>
  fetch_typed_cells(Stream &st, const std::vector<std::string> &fields) {
    std::map<LogicalKey, std::vector<Cell>> result;
    graph::GraphQuery graph(read_.graph_read());
    for (const auto &key : st.keys) {
      if (st.view == View::SignatureSlot) {
        std::vector<Cell> cells;
        cells.reserve(fields.size());
        const auto text_at = [](auto &statement,
                                int index) -> std::optional<std::string> {
          return statement.col_is_null(index)
                     ? std::nullopt
                     : std::optional<std::string>(statement.col_text(index));
        };
        const auto int_at = [](auto &statement,
                               int index) -> std::optional<int64_t> {
          return statement.col_is_null(index)
                     ? std::nullopt
                     : std::optional<int64_t>(statement.col_int64(index));
        };
        const auto push_slot =
            [&](const std::string &slot_kind,
                const std::optional<std::string> &name,
                const std::optional<int64_t> &type_id,
                const std::optional<int64_t> &declared,
                const std::optional<int64_t> &adjusted,
                const std::optional<std::string> &default_text,
                const std::optional<std::string> &default_origin,
                const std::optional<std::string> &reference,
                const graph::GraphQuery::SlotFacts &facts) {
              for (const auto &field : fields) {
                if (field == "id") {
                  cells.emplace_back(logical_row_id(st.view, key));
                } else if (field == "identity_key") {
                  cells.emplace_back(logical_identity(st.view, key));
                } else if (field == "owner_id") {
                  cells.emplace_back(key.a);
                } else if (field == "position") {
                  cells.emplace_back(key.b < 0 ? Cell(nullptr) : Cell(key.b));
                } else if (field == "pack_index") {
                  cells.emplace_back(key.c < 0 ? Cell(nullptr) : Cell(key.c));
                } else if (field == "slot_kind") {
                  cells.emplace_back(slot_kind);
                } else if (field == "name") {
                  name ? cells.emplace_back(*name)
                       : cells.emplace_back(nullptr);
                } else if (field == "type_id") {
                  type_id ? cells.emplace_back(*type_id)
                          : cells.emplace_back(nullptr);
                } else if (field == "declared_type_id") {
                  declared ? cells.emplace_back(*declared)
                           : cells.emplace_back(nullptr);
                } else if (field == "adjusted_type_id") {
                  adjusted ? cells.emplace_back(*adjusted)
                           : cells.emplace_back(nullptr);
                } else if (field == "default_text") {
                  default_text ? cells.emplace_back(*default_text)
                               : cells.emplace_back(nullptr);
                } else if (field == "default_origin") {
                  default_origin ? cells.emplace_back(*default_origin)
                                 : cells.emplace_back(nullptr);
                } else if (field == "reference_semantics") {
                  reference ? cells.emplace_back(*reference)
                            : cells.emplace_back(nullptr);
                } else if (field == "mode") {
                  cells.emplace_back(facts.mode);
                } else if (field == "value_kind") {
                  cells.emplace_back(facts.value_kind);
                } else if (field == "named_decl") {
                  facts.named_decl ? cells.emplace_back(*facts.named_decl)
                                   : cells.emplace_back(nullptr);
                } else {
                  cells.emplace_back(nullptr);
                }
              }
            };
        if (key.tag == 1) {
          auto query = read_.read_db().prepare(
              "SELECT type_id FROM symbol_type WHERE symbol_id=? AND kind=1");
          query.bind(1, key.a);
          if (!query.step()) {
            continue;
          }
          const int64_t type_id = query.col_int64(0);
          const auto facts = graph.slot_facts_for_ids(type_id, type_id);
          push_slot("return", std::nullopt, type_id, type_id, type_id,
                    std::nullopt, std::nullopt, std::nullopt, facts);
        } else if (key.tag == 2) {
          auto query = read_.read_db().prepare(
              "SELECT name,type_id,declared_type_id,adjusted_type_id,"
              "default_text,default_origin,reference_semantics FROM parameter "
              "WHERE owner_id=? AND position=? AND pack_index=?");
          query.bind(1, key.a);
          query.bind(2, key.b);
          query.bind(3, key.c);
          if (!query.step()) {
            continue;
          }
          const auto type_id = int_at(query, 1);
          const auto declared = int_at(query, 2);
          const auto adjusted = int_at(query, 3);
          const auto facts = graph.slot_facts_for_ids(
              declared ? declared : type_id, adjusted ? adjusted : type_id);
          push_slot("parameter", text_at(query, 0), type_id, declared, adjusted,
                    text_at(query, 4), text_at(query, 5), text_at(query, 6),
                    facts);
        } else if (key.tag == 3) {
          auto query = read_.read_db().prepare(
              "SELECT name,type_id,default_txt,default_type_id FROM "
              "template_param "
              "WHERE owner_id=? AND position=?");
          query.bind(1, key.a);
          query.bind(2, key.b);
          if (!query.step()) {
            continue;
          }
          const auto type_id = int_at(query, 1);
          const auto facts = graph.slot_facts_for_ids(type_id, type_id);
          push_slot("template_parameter", text_at(query, 0), type_id, type_id,
                    type_id, text_at(query, 2), std::nullopt, std::nullopt,
                    facts);
        } else {
          auto query = read_.read_db().prepare(
              "SELECT arg_kind,type_id,literal FROM template_arg "
              "WHERE owner_id=? AND position=? AND pack_index=?");
          query.bind(1, key.a);
          query.bind(2, key.b);
          query.bind(3, key.c);
          if (!query.step()) {
            continue;
          }
          const auto type_id = int_at(query, 1);
          const auto facts = graph.slot_facts_for_ids(type_id, type_id);
          push_slot("template_argument", std::nullopt, type_id, type_id,
                    type_id, text_at(query, 2), std::nullopt, std::nullopt,
                    facts);
        }
        result.emplace(key, std::move(cells));
        continue;
      }
      if (st.view == View::TypeLayer) {
        graph::GraphQuery graph(read_.graph_read());
        const auto layers = graph.type_layers(key.a);
        if (key.b < 0 || static_cast<std::size_t>(key.b) >= layers.size()) {
          continue;
        }
        const auto &layer = layers[static_cast<std::size_t>(key.b)];
        std::optional<int64_t> canonical_id;
        auto node = read_.read_db().prepare(
            "SELECT canonical_id FROM type_node WHERE id=?");
        node.bind(1, layer.type.id);
        if (node.step() && !node.col_is_null(0)) {
          canonical_id = node.col_int64(0);
        }
        std::vector<Cell> cells;
        cells.reserve(fields.size());
        for (const auto &field : fields) {
          if (field == "id") {
            cells.emplace_back(logical_row_id(st.view, key));
          } else if (field == "identity_key") {
            cells.emplace_back(logical_identity(st.view, key));
          } else if (field == "root_id") {
            cells.emplace_back(key.a);
          } else if (field == "path") {
            cells.emplace_back(layer.path);
          } else if (field == "relation") {
            cells.emplace_back(layer.relation);
          } else if (field == "position") {
            cells.emplace_back(layer.position);
          } else if (field == "depth") {
            cells.emplace_back(static_cast<int64_t>(layer.depth));
          } else if (field == "status") {
            cells.emplace_back(layer.status);
          } else if (field == "type_id") {
            cells.emplace_back(layer.type.id);
          } else if (field == "spelling") {
            cells.emplace_back(layer.type.spelling);
          } else if (field == "kind") {
            cells.emplace_back(layer.type.kind);
          } else if (field == "extent") {
            layer.type.extent ? cells.emplace_back(*layer.type.extent)
                              : cells.emplace_back(nullptr);
          } else if (field == "element_type") {
            layer.element_type ? cells.emplace_back(*layer.element_type)
                               : cells.emplace_back(nullptr);
          } else if (field == "decl_usr") {
            layer.type.decl_usr ? cells.emplace_back(*layer.type.decl_usr)
                                : cells.emplace_back(nullptr);
          } else if (field == "canonical_id") {
            canonical_id ? cells.emplace_back(*canonical_id)
                         : cells.emplace_back(nullptr);
          } else if (field == "is_const") {
            cells.emplace_back(static_cast<int64_t>(layer.type.is_const));
          } else if (field == "is_volatile") {
            cells.emplace_back(static_cast<int64_t>(layer.type.is_volatile));
          } else if (field == "is_restrict") {
            cells.emplace_back(static_cast<int64_t>(layer.type.is_restrict));
          } else {
            cells.emplace_back(nullptr);
          }
        }
        result.emplace(key, std::move(cells));
        continue;
      }
      if (st.view == View::Evidence && key.tag == 1) {
        auto query = read_.read_db().prepare(
            "SELECT default_txt,default_type_id,default_ref_id FROM "
            "template_param WHERE owner_id=? AND position=?");
        query.bind(1, key.a);
        query.bind(2, key.b);
        if (!query.step()) {
          continue;
        }
        std::vector<Cell> cells;
        cells.reserve(fields.size());
        for (const auto &field : fields) {
          if (const auto derived = derived_typed_cell(st.view, key, field)) {
            cells.push_back(*derived);
          } else if (field == "identity_key") {
            cells.emplace_back(logical_identity(st.view, key));
          } else if (field == "id") {
            cells.emplace_back(logical_row_id(st.view, key));
          } else if (field == "owner_id") {
            cells.emplace_back(key.a);
          } else if (field == "position") {
            cells.emplace_back(key.b);
          } else if (field == "default_txt") {
            if (query.col_is_null(0)) {
              cells.emplace_back(nullptr);
            } else {
              cells.emplace_back(query.col_text(0));
            }
          } else if (field == "default_type_id") {
            if (query.col_is_null(1)) {
              cells.emplace_back(nullptr);
            } else {
              cells.emplace_back(query.col_int64(1));
            }
          } else if (field == "default_ref_id") {
            if (query.col_is_null(2)) {
              cells.emplace_back(nullptr);
            } else {
              cells.emplace_back(query.col_int64(2));
            }
          } else {
            cells.emplace_back(nullptr);
          }
        }
        result.emplace(key, std::move(cells));
        continue;
      }
      std::vector<std::string> columns;
      std::vector<int> raw_index(fields.size(), -1);
      for (size_t i = 0; i < fields.size(); ++i) {
        const std::string column = typed_column(st.view, fields[i]);
        if (!column.empty()) {
          raw_index[i] = static_cast<int>(columns.size());
          columns.push_back(column);
        }
      }
      std::string sql = "SELECT ";
      if (columns.empty()) {
        sql += "1";
      } else {
        for (size_t i = 0; i < columns.size(); ++i) {
          if (i != 0) {
            sql += ",";
          }
          sql += columns[i];
        }
      }
      sql +=
          " FROM " + typed_table(st.view) + " WHERE " + logical_where(st.view);
      auto query = read_.read_db().prepare(sql);
      const auto args = logical_args(st.view, key);
      for (size_t i = 0; i < args.size(); ++i) {
        query.bind(static_cast<int>(i + 1), args[i]);
      }
      if (!query.step()) {
        continue;
      }
      std::vector<Cell> cells;
      cells.reserve(fields.size());
      for (size_t i = 0; i < fields.size(); ++i) {
        const std::string &field = fields[i];
        if (const auto derived = derived_typed_cell(st.view, key, field)) {
          cells.push_back(*derived);
          continue;
        }
        if (field == "identity_key") {
          cells.emplace_back(logical_identity(st.view, key));
          continue;
        }
        if (field == "id") {
          cells.emplace_back(logical_row_id(st.view, key));
          continue;
        }
        const int col = raw_index[i];
        if (col < 0) {
          cells.emplace_back(nullptr);
          continue;
        }
        if (query.col_is_null(col)) {
          cells.emplace_back(nullptr);
        } else if (field == "file") {
          const auto path = file_path(query.col_int64(col));
          cells.emplace_back(path ? Cell(*path) : Cell(nullptr));
        } else if (field == "name" || field == "spelling" ||
                   field == "type_key" || field == "extent" ||
                   field == "element_type" || field == "default_text" ||
                   field == "default_origin" || field == "default_txt" ||
                   field == "reference_semantics" || field == "literal" ||
                   field == "src_kind" || field == "type_usr" ||
                   field == "decl_usr" || field == "callee_usr" ||
                   field == "args_sig" || field == "recv_src_kind" ||
                   field == "recv_type_usr" || field == "recv_decl_usr" ||
                   field == "provenance" || field == "role") {
          cells.emplace_back(query.col_text(col));
        } else {
          cells.emplace_back(query.col_int64(col));
        }
      }
      result.emplace(key, std::move(cells));
    }
    return result;
  }

  // Fetch the cells for `fields` for every unique id in st.ids.
  std::map<int64_t, std::vector<Cell>>
  fetch_cells(Stream &st, const std::vector<std::string> &fields) {
    const bool need_entity =
        std::ranges::find(fields, "entity_type") != fields.end();
    const std::string join = join_clause(need_entity);

    std::vector<int64_t> uniq = st.ids;
    std::ranges::sort(uniq);
    uniq.erase(std::ranges::unique(uniq).begin(), uniq.end());

    std::string cols;
    for (const auto &f : fields) {
      cols += ", " + col_expr(f, "s", "en");
    }

    std::map<int64_t, std::vector<Cell>> by_id;
    for (size_t at = 0; at < uniq.size(); at += kIdChunk) {
      const size_t n = std::min(kIdChunk, uniq.size() - at);
      std::string sql = "SELECT s.id" + cols + " FROM symbol s" + join +
                        " WHERE s.id IN (" + placeholders(n) + ")";
      std::vector<SqlValue> args;
      args.reserve(n);
      for (size_t i = 0; i < n; ++i) {
        args.emplace_back(uniq[at + i]);
      }
      auto stq = read_.read_db().prepare(sql);
      for (size_t i = 0; i < args.size(); ++i) {
        stq.bind(static_cast<int>(i + 1), args[i]);
      }
      while (stq.step()) {
        const int64_t id = stq.col_int64(0);
        std::vector<Cell> cells;
        cells.reserve(fields.size());
        for (size_t i = 0; i < fields.size(); ++i) {
          const int col = static_cast<int>(i + 1);
          const std::string &f = fields[i];
          if (stq.col_is_null(col)) {
            cells.emplace_back(nullptr);
          } else if (f == "kind") {
            cells.emplace_back(symbol_kind_name(stq.col_int64(col)));
          } else if (f == "entity_type") {
            cells.push_back(entity_type_name_cell(stq.col_int64(col)));
          } else if (f == "file") {
            auto p = file_path(stq.col_int64(col));
            cells.emplace_back(p ? Cell(*p) : Cell(nullptr));
          } else if (f == "usr" || f == "name" || f == "spelling" ||
                     f == "qual_name" || f == "semantic_universe" ||
                     f == "identity_key" || f == "callable_kind" ||
                     f == "template_origin" || f == "template_form" ||
                     f == "owner") {
            cells.emplace_back(stq.col_text(col));
          } else {
            cells.emplace_back(stq.col_int64(col));
          }
        }
        by_id.emplace(id, std::move(cells));
      }
    }
    return by_id;
  }

  void materialize(Stream &st, const std::vector<std::string> &fields) {
    reject_ambiguous_ungrouped(st);
    if (!st.keys.empty() ||
        (st.view != View::Symbol && st.view != View::Entity)) {
      auto by_key = fetch_typed_cells(st, fields);
      st.fields = fields;
      st.rows.clear();
      st.row_ids.clear();
      st.row_status.clear();
      for (const auto &key : st.keys) {
        auto it = by_key.find(key);
        if (it != by_key.end()) {
          st.rows.push_back(it->second);
          st.row_ids.push_back(logical_row_id(st.view, key));
          st.row_status.push_back(status_for_key(st.view, key));
        }
      }
      st.keys.clear();
      return;
    }
    auto by_id = fetch_cells(st, fields);
    st.fields = fields;
    st.rows.clear();
    st.row_ids.clear();
    st.row_status.clear();
    for (int64_t id : st.ids) {
      auto it = by_id.find(id);
      if (it != by_id.end()) {
        st.rows.push_back(it->second);
        st.row_ids.push_back(id);
        st.row_status.emplace_back();
      }
    }
    st.ids.clear();
  }

  static void apply_distinct(Stream &st) {
    if (st.shape == Shape::Path) {
      std::vector<PathWitness> out;
      for (auto &witness : st.paths) {
        const bool dup = std::ranges::any_of(out, [&](const PathWitness &prev) {
          if (prev.steps.size() != witness.steps.size()) {
            return false;
          }
          for (size_t i = 0; i < prev.steps.size(); ++i) {
            const PathStep &a = prev.steps[i];
            const PathStep &b = witness.steps[i];
            // Full logical step identity: two typed-view slots on the same
            // owner (e.g. parameter positions 0 and 1) share node_id/
            // through/inbound but are still distinct witnesses.
            if (a.node_id != b.node_id || a.through != b.through ||
                a.inbound != b.inbound || a.domain != b.domain ||
                a.position != b.position || a.pack_index != b.pack_index) {
              return false;
            }
          }
          return true;
        });
        if (!dup) {
          out.push_back(std::move(witness));
        }
      }
      st.paths = std::move(out);
      return;
    }
    if (st.shape == Shape::Nodes) {
      if (!st.keys.empty()) {
        std::ranges::sort(st.keys);
        st.keys.erase(std::ranges::unique(st.keys).begin(), st.keys.end());
      } else {
        std::ranges::sort(st.ids);
        st.ids.erase(std::ranges::unique(st.ids).begin(), st.ids.end());
      }
      return;
    }
    std::vector<std::vector<Cell>> rows;
    std::vector<int64_t> row_ids;
    std::vector<Stream::RowStatus> row_status;
    for (size_t i = 0; i < st.rows.size(); ++i) {
      bool dup = false;
      for (const auto &prev : rows) {
        if (prev.size() == st.rows[i].size()) {
          bool same = true;
          for (size_t c = 0; c < prev.size(); ++c) {
            if (!cell_eq(prev[c], st.rows[i][c])) {
              same = false;
              break;
            }
          }
          if (same) {
            dup = true;
            break;
          }
        }
      }
      if (!dup) {
        rows.push_back(st.rows[i]);
        row_ids.push_back(st.row_ids[i]);
        row_status.push_back(st.row_status[i]);
      }
    }
    st.rows = std::move(rows);
    st.row_ids = std::move(row_ids);
    st.row_status = std::move(row_status);
  }

  void apply_order(Stream &st, const std::vector<std::string> &fields) {
    if (st.shape == Shape::Nodes) {
      if (!st.keys.empty()) {
        auto by_key = fetch_typed_cells(st, fields);
        std::ranges::stable_sort(st.keys,
                                 [&](const LogicalKey &a, const LogicalKey &b) {
                                   const auto &ca = by_key.at(a);
                                   const auto &cb = by_key.at(b);
                                   for (size_t i = 0; i < ca.size(); ++i) {
                                     if (!cell_eq(ca[i], cb[i]))
                                       return cell_less(ca[i], cb[i]);
                                   }
                                   return a < b;
                                 });
        return;
      }
      auto by_id = fetch_cells(st, fields);
      std::ranges::stable_sort(st.ids, [&](int64_t a, int64_t b) {
        const auto &ca = by_id.at(a);
        const auto &cb = by_id.at(b);
        for (size_t i = 0; i < ca.size(); ++i) {
          if (!cell_eq(ca[i], cb[i])) {
            return cell_less(ca[i], cb[i]);
          }
        }
        return a < b;
      });
      return;
    }
    std::vector<size_t> pos;
    pos.reserve(fields.size());
    for (const auto &f : fields) {
      pos.push_back(static_cast<size_t>(std::ranges::find(st.fields, f) -
                                        st.fields.begin()));
    }
    std::vector<size_t> idx(st.rows.size());
    for (size_t i = 0; i < idx.size(); ++i) {
      idx[i] = i;
    }
    std::ranges::stable_sort(idx, [&](size_t a, size_t b) {
      for (size_t p : pos) {
        if (!cell_eq(st.rows[a][p], st.rows[b][p])) {
          return cell_less(st.rows[a][p], st.rows[b][p]);
        }
      }
      return st.row_ids[a] < st.row_ids[b];
    });
    std::vector<std::vector<Cell>> rows;
    std::vector<int64_t> row_ids;
    std::vector<Stream::RowStatus> row_status;
    rows.reserve(idx.size());
    row_ids.reserve(idx.size());
    for (size_t i : idx) {
      rows.push_back(std::move(st.rows[i]));
      row_ids.push_back(st.row_ids[i]);
      row_status.push_back(st.row_status[i]);
    }
    st.rows = std::move(rows);
    st.row_ids = std::move(row_ids);
    st.row_status = std::move(row_status);
  }

  static void apply_limit(Stream &st, int64_t n) {
    st.limit_in_effect = true;
    if (st.shape == Shape::Path) {
      if (std::cmp_greater(st.paths.size(), n)) {
        st.paths.resize(n);
      }
    } else if (st.shape == Shape::Nodes) {
      if (!st.keys.empty()) {
        if (std::cmp_greater(st.keys.size(), n))
          st.keys.resize(n);
      } else if (std::cmp_greater(st.ids.size(), n)) {
        st.ids.resize(n);
      }
    } else if (std::cmp_greater(st.rows.size(), n)) {
      st.rows.resize(n);
      st.row_ids.resize(n);
      st.row_status.resize(n);
    }
  }

public:
  // Finish: default cap, default node fields.
  Result finish(Stream st) {
    Result res;
    res.view = st.view;
    res.path_rows_examined = st.path_rows_examined;
    reject_ambiguous_ungrouped(st);
    if (st.shape == Shape::Scalar) {
      // count() after path()/reverse_type_use() counts witnesses; after
      // select() it counts rows; otherwise ids/keys hold the stream.
      if (!st.paths.empty() || st.rows.empty()) {
        res.truncated = st.truncated;
        res.partial = std::ranges::any_of(st.paths, [](const PathWitness &w) {
          return w.status == "partial";
        });
        res.unknown = st.unknown;
      } else {
        recompute_status(st);
        res.truncated = st.truncated;
        res.partial = st.partial;
        res.unknown = st.unknown;
      }
      res.shape = Shape::Scalar;
      if (!st.paths.empty()) {
        res.scalar = static_cast<int64_t>(st.paths.size());
      } else if (!st.rows.empty()) {
        res.scalar = static_cast<int64_t>(st.rows.size());
      } else if (!st.keys.empty()) {
        res.scalar = static_cast<int64_t>(st.keys.size());
      } else {
        res.scalar = static_cast<int64_t>(st.ids.size());
      }
      return res;
    }
    if (st.shape == Shape::Path) {
      if (!st.limit_in_effect &&
          static_cast<int64_t>(st.paths.size()) > kDefaultResultCap) {
        st.paths.resize(kDefaultResultCap);
        st.truncated = true;
      }
      res.shape = Shape::Path;
      res.truncated = st.truncated;
      res.partial = std::ranges::any_of(
          st.paths, [](const PathWitness &w) { return w.status == "partial"; });
      res.unknown = st.unknown;
      res.paths = std::move(st.paths);
      return res;
    }
    if (st.shape == Shape::Nodes) {
      if (!st.keys.empty()) {
        materialize(st, {"id", "identity_key"});
      } else {
        materialize(st, {"id", "usr", "semantic_universe", "identity_key",
                         "name", "kind"});
      }
    }
    if (!st.limit_in_effect &&
        static_cast<int64_t>(st.rows.size()) > kDefaultResultCap) {
      st.rows.resize(kDefaultResultCap);
      st.row_ids.resize(kDefaultResultCap);
      st.row_status.resize(kDefaultResultCap);
      st.truncated = true;
    }
    recompute_status(st);
    res.shape = st.shape;
    res.truncated = st.truncated;
    res.partial = st.partial;
    res.unknown = st.unknown;
    res.fields = st.fields;
    res.rows = std::move(st.rows);
    return res;
  }
};

} // namespace

namespace {

json_out::Value path_step_to_json(const PathStep &step) {
  using namespace json_out;
  Object o;
  o.emplace_back("id", Value::of(step.node_id));
  o.emplace_back("domain", Value::of(step.domain));
  o.emplace_back("through", Value::of(step.through));
  if (step.inbound) {
    o.emplace_back("direction", Value::of(std::string("in")));
  }
  o.emplace_back("status", Value::of(step.status));
  if (step.position >= 0) {
    o.emplace_back("position", Value::of(step.position));
  }
  if (step.pack_index >= 0) {
    o.emplace_back("pack_index", Value::of(step.pack_index));
  }
  Array sites;
  for (const auto &site : step.sites) {
    Object so;
    so.emplace_back("file_id",
                    site.file_id ? Value::of(*site.file_id) : Value::null());
    so.emplace_back("line", site.line ? Value::of(*site.line) : Value::null());
    so.emplace_back("col", site.col ? Value::of(*site.col) : Value::null());
    so.emplace_back("conditional", Value::of(site.conditional));
    sites.push_back(Value::obj(std::move(so)));
  }
  o.emplace_back("sites", Value::arr(std::move(sites)));
  return Value::obj(std::move(o));
}

json_out::Value path_witness_to_json(const PathWitness &witness) {
  using namespace json_out;
  Object o;
  o.emplace_back("length", Value::of(witness.length));
  o.emplace_back("status", Value::of(witness.status));
  Array steps;
  for (const auto &step : witness.steps) {
    steps.push_back(path_step_to_json(step));
  }
  o.emplace_back("steps", Value::arr(std::move(steps)));
  return Value::obj(std::move(o));
}

} // namespace

json_out::Value Result::to_json() const {
  using namespace json_out;
  Object o;
  o.emplace_back("shape", Value::of(shape_name(shape)));
  o.emplace_back("view", Value::of(std::string(view_name(view))));
  if (shape == Shape::Scalar) {
    o.emplace_back("count", Value::of(scalar));
    o.emplace_back("truncated", Value::of(truncated));
    o.emplace_back("index", index_identity_json(index));
    return Value::obj(std::move(o));
  }
  if (shape == Shape::Path) {
    o.emplace_back("count", Value::of(static_cast<int64_t>(paths.size())));
    o.emplace_back("truncated", Value::of(truncated));
    o.emplace_back("index", index_identity_json(index));
    Array arr;
    for (const auto &witness : paths) {
      arr.push_back(path_witness_to_json(witness));
    }
    o.emplace_back("paths", Value::arr(std::move(arr)));
    return Value::obj(std::move(o));
  }
  o.emplace_back("count", Value::of(static_cast<int64_t>(rows.size())));
  o.emplace_back("truncated", Value::of(truncated));
  o.emplace_back("index", index_identity_json(index));
  Array arr;
  for (const auto &row : rows) {
    Object ro;
    for (size_t i = 0; i < fields.size(); ++i) {
      const Cell &c = row[i];
      if (std::holds_alternative<std::nullptr_t>(c)) {
        ro.emplace_back(fields[i], Value::null());
      } else if (std::holds_alternative<int64_t>(c)) {
        ro.emplace_back(fields[i], Value::of(std::get<int64_t>(c)));
      } else {
        ro.emplace_back(fields[i], Value::of(std::get<std::string>(c)));
      }
    }
    arr.push_back(Value::obj(std::move(ro)));
  }
  o.emplace_back("rows", Value::arr(std::move(arr)));
  return Value::obj(std::move(o));
}

protocol::ResultEnvelope Result::to_envelope() const {
  using namespace protocol;
  using namespace json_out;

  Object payload;
  payload.emplace_back("shape", Value::of(shape_name(shape)));
  payload.emplace_back("view", Value::of(std::string(view_name(view))));
  if (shape == Shape::Scalar) {
    payload.emplace_back("count", Value::of(scalar));
  } else if (shape == Shape::Path) {
    payload.emplace_back("count",
                         Value::of(static_cast<int64_t>(paths.size())));
  } else {
    payload.emplace_back("count", Value::of(static_cast<int64_t>(rows.size())));
  }
  payload.emplace_back("truncated", Value::of(truncated));
  if (shape == Shape::Path) {
    Array path_values;
    for (const auto &witness : paths) {
      path_values.push_back(path_witness_to_json(witness));
    }
    payload.emplace_back("paths", Value::arr(std::move(path_values)));
  } else if (shape != Shape::Scalar) {
    Array row_values;
    for (const auto &row : rows) {
      Object row_value;
      for (size_t i = 0; i < fields.size(); ++i) {
        const Cell &cell = row[i];
        if (std::holds_alternative<std::nullptr_t>(cell)) {
          row_value.emplace_back(fields[i], Value::null());
        } else if (std::holds_alternative<int64_t>(cell)) {
          row_value.emplace_back(fields[i], Value::of(std::get<int64_t>(cell)));
        } else {
          row_value.emplace_back(fields[i],
                                 Value::of(std::get<std::string>(cell)));
        }
      }
      row_values.push_back(Value::obj(std::move(row_value)));
    }
    payload.emplace_back("rows", Value::arr(std::move(row_values)));
  }

  ResultEnvelope envelope;
  envelope.operation = "query";
  const bool stale = index.freshness == "stale";
  if (index.freshness == "current" && !stale && !unknown && !truncated &&
      !partial) {
    envelope.status = Status::Complete;
  } else if (!stale && !unknown && (truncated || partial)) {
    envelope.status = Status::Partial;
  } else {
    envelope.status = Status::Unknown;
  }
  envelope.identity.workspace = index.workspace;
  envelope.identity.index =
      "semantic-index/schema/" + std::to_string(index.schema_version);
  std::string fact_set;
  if (view == View::Symbol) {
    fact_set = "symbols";
  } else if (view == View::Entity) {
    fact_set = "entities";
  } else {
    fact_set = view_name(view);
  }
  envelope.identity.fact_sets = {std::move(fact_set)};
  envelope.identity.freshness = index.freshness;
  envelope.identity.source_revision = index.source_revision;
  envelope.identity.source_fingerprint = index.source_fingerprint;
  envelope.producer.package = "cidx";
  envelope.producer.version = std::string(version::kFullProductVersion);
  envelope.producer.backend = "cpp";
  envelope.producer.schema_version = kProtocolVersion;
  if (envelope.status == Status::Complete) {
    envelope.completeness.state = "complete";
  } else if (envelope.status == Status::Partial) {
    envelope.completeness.state = "partial";
  } else {
    envelope.completeness.state = "unknown";
  }
  envelope.completeness.truncated = truncated;
  envelope.completeness.stale = index.freshness == "stale";
  envelope.result = Value::obj(std::move(payload));
  envelope.evidence.push_back(
      protocol::EvidenceNode{.id = "queryplan",
                             .evidence_class = "derived",
                             .trust = "producer-verified",
                             .summary = "bounded QueryPlan execution",
                             .source = std::nullopt,
                             .children = {}});
  envelope.artifacts.push_back(protocol::ArtifactRef{
      .kind = "semantic-index",
      .id = envelope.identity.index,
      .schema_version = index.schema_version,
      .catalog_version = catalog::kCatalogVersion,
      .catalog_hash = std::string(catalog::kCatalogHash)});
  if (truncated) {
    envelope.diagnostics.push_back(protocol::Diagnostic{
        .code = "truncated_budget",
        .severity = "warning",
        .message = "result was bounded by the QueryPlan execution budget",
        .next_action = "narrow the query or provide an explicit limit"});
  }
  if (unknown) {
    envelope.diagnostics.push_back(protocol::Diagnostic{
        .code = "unknown",
        .severity = "warning",
        .message = "result contains unresolved relation provenance",
        .next_action = "inspect evidence and index coverage before relying on "
                       "this result"});
  }
  if (index.freshness == "stale") {
    envelope.diagnostics.push_back(protocol::Diagnostic{
        .code = "stale_input",
        .severity = "error",
        .message = "index contents are stale for the workspace",
        .next_action =
            "re-index the affected sources before relying on this result"});
  } else if (index.freshness != "current") {
    envelope.diagnostics.push_back(protocol::Diagnostic{
        .code = "unknown",
        .severity = "warning",
        .message = "index freshness could not be verified",
        .next_action =
            "stamp or re-index the workspace before relying on this result"});
  }
  return envelope;
}

Result Executor::run(const Plan &plan) {
  const Plan normalized = validate(plan);
  Exec exec(read_);
  Stream st = exec.run_plan(normalized);
  Result res = exec.finish(std::move(st));
  res.index = read_.index_identity();
  return res;
}

namespace {

void collect_relation_names(const Pred &p, std::set<std::string> &names) {
  switch (p.op) {
  case PredOp::AllOf:
  case PredOp::AnyOf:
    for (const auto &kid : p.kids) {
      collect_relation_names(kid, names);
    }
    return;
  case PredOp::Not:
    collect_relation_names(p.kids[0], names);
    return;
  case PredOp::Exists:
  case PredOp::None:
  case PredOp::All:
  case PredOp::AtLeast:
  case PredOp::Exactly:
    names.insert(p.relation);
    if (p.target) {
      collect_relation_names(*p.target, names);
    }
    return;
  case PredOp::Eq:
  case PredOp::Ne:
  case PredOp::Glob:
  case PredOp::In:
    return;
  }
}

// reverse_type_use() does not walk a single catalogued relation -- it climbs
// type_edge and type_node.canonical_id, then reads four owner-fact tables --
// but every one of those inputs is either a catalogued relation already or,
// for the canonical_id (cv/sugar desugaring) climb, a name with no catalog
// entry to resolve against. This is that fixed input set, with the
// completeness the synthetic (non-catalogued) entry cannot look up itself.
const std::vector<std::string> &reverse_type_use_input_relations() {
  static const std::vector<std::string> names = {"type.has_type_edge",
                                                 "type.canonical_id",
                                                 "symbol.of_type",
                                                 "parameter.of_type",
                                                 "template_parameter.of_type",
                                                 "template_argument.of_type"};
  return names;
}

const std::map<std::string, std::string> &synthetic_relation_completeness() {
  // type_node.canonical_id desugaring has no RelationDesc catalog entry;
  // treat it as partial, matching type.has_type_edge's own completeness.
  static const std::map<std::string, std::string> table = {
      {"type.canonical_id", "partial"}};
  return table;
}

// Every relation a normalized plan touches (traversals and quantifier
// predicates; path()'s relation is catalogued, reverse_type_use()'s fixed
// input set above is not), including one level into
// union()/intersect()/except()/path() "to" operand plans.
void collect_stage_relations(const Stage &stage, std::set<std::string> &names) {
  if (stage.op == StageOp::Out || stage.op == StageOp::In ||
      stage.op == StageOp::Path) {
    names.insert(stage.relation);
  } else if (stage.op == StageOp::ReverseTypeUse) {
    const auto &fixed = reverse_type_use_input_relations();
    names.insert(fixed.begin(), fixed.end());
  }
  if (stage.pred) {
    collect_relation_names(*stage.pred, names);
  }
  if (stage.operand) {
    for (const auto &sub : stage.operand->stages) {
      collect_stage_relations(sub, names);
    }
  }
}

} // namespace

json_out::Value Executor::explain(const Plan &plan) {
  using namespace json_out;
  const Plan normalized = validate(plan);
  Object o;
  o.emplace_back("plan", plan_to_json(normalized));
  o.emplace_back("index", index_identity_json(read_.index_identity()));
  o.emplace_back("execution_shape",
                 Value::of(shape_name(final_shape(normalized))));

  Object budgets;
  budgets.emplace_back("traverse_node_budget", Value::of(kTraverseNodeBudget));
  budgets.emplace_back("enumerate_budget", Value::of(kEnumerateBudget));
  budgets.emplace_back("path_node_budget", Value::of(kPathNodeBudget));
  budgets.emplace_back("default_result_cap", Value::of(kDefaultResultCap));
  o.emplace_back("budgets", Value::obj(std::move(budgets)));

  std::set<std::string> relation_names;
  for (const auto &stage : normalized.stages) {
    collect_stage_relations(stage, relation_names);
  }
  Array relations;
  bool partial_inputs = false;
  bool unknown_capable_inputs = false;
  for (const auto &name : relation_names) {
    const RelationDesc *rel = resolve_qualified_relation(name);
    Object ro;
    ro.emplace_back("relation", Value::of(name));
    std::string completeness;
    if (rel != nullptr) {
      completeness = rel->completeness;
    } else if (const auto it = synthetic_relation_completeness().find(name);
               it != synthetic_relation_completeness().end()) {
      completeness = it->second;
    } else {
      completeness = "unknown";
    }
    ro.emplace_back("completeness", Value::of(completeness));
    partial_inputs = partial_inputs || completeness == "partial";
    unknown_capable_inputs =
        unknown_capable_inputs || completeness != "complete";
    relations.push_back(Value::obj(std::move(ro)));
  }
  o.emplace_back("input_relations", Value::arr(std::move(relations)));
  o.emplace_back("partial_inputs", Value::of(partial_inputs));
  o.emplace_back("unknown_capable_inputs", Value::of(unknown_capable_inputs));
  return Value::obj(std::move(o));
}

} // namespace cidx::query
