// query/exec.cpp -- SQLite executor for validated CXQ plans.
// Contract: docs/query-plan.md (v1). The Python twin is
// python/indexer/queryplan.py: both build the same SQL shapes over the same
// tables, so semantics stay identical by construction.

#include "query/exec.hpp"

#include "catalogs/generated_catalog.hpp"
#include "cli/version.hpp"
#include "graph/query.hpp"

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
  }
  return "scalar";
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

std::vector<int64_t> fetch_ids(Storage &db, const std::string &sql,
                               const std::vector<SqlValue> &args) {
  auto st = db.raw_db().prepare(sql);
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

  View view = View::Symbol;
  Shape shape = Shape::Nodes;
  std::vector<int64_t> ids;     // nodes shape; ascending, deduped
  std::vector<LogicalKey> keys; // typed logical rows; never SQLite row ids
  std::vector<std::string> fields;
  std::vector<std::vector<Cell>> rows; // rows shape
  std::vector<int64_t> row_ids;        // per-row id (order_by tie-break)
  bool truncated = false;
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
  explicit Exec(Storage &db) : db_(db) {}

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
      }
      if (st.shape == Shape::Scalar) {
        break; // count() is terminal
      }
    }
    reject_ambiguous_ungrouped(st);
    return st;
  }

private:
  Storage &db_;
  std::map<int64_t, std::optional<std::string>> file_paths_;

  bool ambiguous_ungrouped_file(int64_t file_id) {
    auto file =
        db_.raw_db().prepare("SELECT c.name,c.path,r.name,r.remote_url,su.key "
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
    auto components = db_.raw_db().prepare(
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
    if (st.view != View::CallArgument && st.view != View::Evidence) {
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
      auto ids = fetch_ids(db_, sql, {SqlValue(src.ref)});
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
        auto part = fetch_ids(db_, sql, args);
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
      case View::CallArgument:
        sql = "SELECT edge_id,file_id,line,col,position FROM call_arg ORDER BY "
              "edge_id,file_id,line,col,position";
        break;
      case View::Edge:
        sql = "SELECT id FROM edge ORDER BY id";
        break;
      case View::Evidence:
        sql = "SELECT edge_id,file_id,COALESCE(line,0),COALESCE(col,0) "
              "FROM edge_site ORDER BY edge_id,file_id,line,col";
        break;
      case View::Type:
        sql = "SELECT id FROM type_node ORDER BY id";
        break;
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
          !fetch_ids(db_, sql + " IS NULL LIMIT 1", args).empty()) {
        throw PlanError("E_UNKNOWN: predicate evaluation is unknown");
      }
      append_unknown_policy(sql, unknown);
    }
    sql += " ORDER BY s.id LIMIT ?";
    args.emplace_back(kEnumerateBudget + 1);
    st.ids = fetch_ids(db_, sql, args);
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
          !fetch_ids(db_, sql + " IS NULL LIMIT 1", args).empty()) {
        throw PlanError("E_UNKNOWN: predicate evaluation is unknown");
      }
      append_unknown_policy(sql, unknown);
      sql += " ORDER BY s.id";
      auto part = fetch_ids(db_, sql, args);
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
    graph::GraphQuery graph(db_);
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
    auto query = db_.raw_db().prepare(sql);
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
      case View::CallArgument:
        key.a = query.col_int64(0);
        key.b = query.col_int64(1);
        key.c = query.col_int64(2);
        key.d = query.col_int64(3);
        key.e = query.col_int64(4);
        break;
      case View::Evidence:
        key.a = query.col_int64(0);
        key.b = query.col_int64(1);
        key.c = query.col_int64(2);
        key.d = query.col_int64(3);
        break;
      case View::Edge:
      case View::Type:
        key.a = query.col_int64(0);
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
    return fetch_ids(db_, sql, args);
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
        } else if (rel.name == "of_type") {
          add_ids("SELECT type_id FROM symbol_type WHERE symbol_id=? "
                  "ORDER BY type_id",
                  {SqlValue(owner)});
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
          auto query = db_.raw_db().prepare(
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
        add_keys(View::Evidence,
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
        auto part = fetch_ids(db_, sql, args);
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

  std::optional<std::string> file_path(int64_t file_id) {
    auto it = file_paths_.find(file_id);
    if (it == file_paths_.end()) {
      it = file_paths_.emplace(file_id, db_.file_abs_path(file_id)).first;
    }
    return it->second;
  }

  std::string portable_symbol(int64_t id) {
    auto query = db_.raw_db().prepare(
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
    auto query = db_.raw_db().prepare(
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
    auto query = db_.raw_db().prepare(
        "SELECT type_key,spelling FROM type_node WHERE id=?");
    query.bind(1, id);
    if (!query.step()) {
      return "missing-type:" + std::to_string(id);
    }
    const auto type_key = query.col_text(0);
    return type_key.empty() ? query.col_text(1) : type_key;
  }

  std::string portable_edge(int64_t id) {
    auto query =
        db_.raw_db().prepare("SELECT src_id,dst_id,kind FROM edge WHERE id=?");
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
    if (view == View::Edge) {
      return "edge:" + portable_edge(key.a);
    }
    if (view == View::Type) {
      return "type:" + portable_type(key.a);
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
      return "edge_site";
    case View::Type:
      return "type_node";
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
      if (view == View::Type) {
        return std::set<std::string>{"id",          "type_key", "spelling",
                                     "kind",        "is_const", "is_volatile",
                                     "is_restrict", "decl_usr", "decl_id",
                                     "canonical_id"};
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
    case View::Evidence:
      return {SqlValue(key.a), SqlValue(key.b), SqlValue(key.c),
              SqlValue(key.d)};
    case View::Edge:
    case View::Type:
      return {SqlValue(key.a)};
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
    case View::Evidence:
      return "edge_id=? AND file_id=? AND line=? AND col=?";
    case View::Edge:
    case View::Type:
      return "id=?";
    case View::Symbol:
    case View::Entity:
      return "1=0";
    }
    return "1=0";
  }

  std::map<LogicalKey, std::vector<Cell>>
  fetch_typed_cells(Stream &st, const std::vector<std::string> &fields) {
    std::map<LogicalKey, std::vector<Cell>> result;
    for (const auto &key : st.keys) {
      if (st.view == View::Evidence && key.tag == 1) {
        auto query = db_.raw_db().prepare(
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
          if (field == "identity_key") {
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
      auto query = db_.raw_db().prepare(sql);
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
                   field == "type_key" || field == "default_text" ||
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
      auto stq = db_.raw_db().prepare(sql);
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
                     f == "identity_key") {
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
      for (const auto &key : st.keys) {
        auto it = by_key.find(key);
        if (it != by_key.end()) {
          st.rows.push_back(it->second);
          st.row_ids.push_back(logical_row_id(st.view, key));
        }
      }
      st.keys.clear();
      return;
    }
    auto by_id = fetch_cells(st, fields);
    st.fields = fields;
    st.rows.clear();
    st.row_ids.clear();
    for (int64_t id : st.ids) {
      auto it = by_id.find(id);
      if (it != by_id.end()) {
        st.rows.push_back(it->second);
        st.row_ids.push_back(id);
      }
    }
    st.ids.clear();
  }

  static void apply_distinct(Stream &st) {
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
      }
    }
    st.rows = std::move(rows);
    st.row_ids = std::move(row_ids);
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
    rows.reserve(idx.size());
    row_ids.reserve(idx.size());
    for (size_t i : idx) {
      rows.push_back(std::move(st.rows[i]));
      row_ids.push_back(st.row_ids[i]);
    }
    st.rows = std::move(rows);
    st.row_ids = std::move(row_ids);
  }

  static void apply_limit(Stream &st, int64_t n) {
    st.limit_in_effect = true;
    if (st.shape == Shape::Nodes) {
      if (!st.keys.empty()) {
        if (std::cmp_greater(st.keys.size(), n))
          st.keys.resize(n);
      } else if (std::cmp_greater(st.ids.size(), n)) {
        st.ids.resize(n);
      }
    } else if (std::cmp_greater(st.rows.size(), n)) {
      st.rows.resize(n);
      st.row_ids.resize(n);
    }
  }

public:
  // Finish: default cap, default node fields.
  Result finish(Stream st) {
    Result res;
    res.view = st.view;
    res.truncated = st.truncated;
    reject_ambiguous_ungrouped(st);
    if (st.shape == Shape::Scalar) {
      res.shape = Shape::Scalar;
      // count() after select carries rows; otherwise ids hold the stream.
      res.scalar = static_cast<int64_t>(
          st.rows.empty() ? (st.keys.empty() ? st.ids.size() : st.keys.size())
                          : st.rows.size());
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
      st.truncated = true;
    }
    res.shape = st.shape;
    res.truncated = st.truncated;
    res.fields = st.fields;
    res.rows = std::move(st.rows);
    return res;
  }
};

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
  } else {
    payload.emplace_back("count", Value::of(static_cast<int64_t>(rows.size())));
  }
  payload.emplace_back("truncated", Value::of(truncated));
  if (shape != Shape::Scalar) {
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
  if (index.freshness == "stale") {
    envelope.status = Status::Unknown;
  } else if (truncated) {
    envelope.status = Status::Partial;
  } else if (index.freshness == "current") {
    envelope.status = Status::Complete;
  } else {
    envelope.status = Status::Unknown;
  }
  envelope.identity.workspace = index.workspace;
  envelope.identity.index =
      "semantic-index/schema/" + std::to_string(index.schema_version);
  envelope.identity.fact_sets = {view == View::Symbol ? "symbols" : "entities"};
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
  Exec exec(db_);
  Stream st = exec.run_plan(normalized);
  Result res = exec.finish(std::move(st));
  res.index = db_.index_identity();
  return res;
}

json_out::Value Executor::explain(const Plan &plan) {
  const Plan normalized = validate(plan);
  json_out::Object o;
  o.emplace_back("plan", plan_to_json(normalized));
  o.emplace_back("index", index_identity_json(db_.index_identity()));
  return json_out::Value::obj(std::move(o));
}

} // namespace cidx::query
