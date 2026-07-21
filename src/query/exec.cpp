// query/exec.cpp -- SQLite executor for validated CXQ plans.
// Contract: docs/query-plan.md (v1). The Python twin is
// python/indexer/queryplan.py: both build the same SQL shapes over the same
// tables, so semantics stay identical by construction.

#include "query/exec.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace cidx::query {

namespace {

// ---- field -> SQL column expression -----------------------------------------
// `kind` is ALWAYS the C++ declaration kind (symbol.kind); `entity_type` is
// ALWAYS the Layer-1 classification (entity_node.kind, NULL for non-entities).
// The two are separate fields so `kind in [class, struct]` keeps its
// declaration-kind meaning while `entity_type = abstract_class` filters on
// classification (PR #20 review).

std::string col_expr(const std::string &field) {
  if (field == "id") {
    return "s.id";
  }
  if (field == "usr") {
    return "s.usr";
  }
  if (field == "name") {
    return "COALESCE(s.qual_name, s.spelling)";
  }
  if (field == "spelling") {
    return "s.spelling";
  }
  if (field == "qual_name") {
    return "s.qual_name";
  }
  if (field == "kind") {
    return "s.kind";
  }
  if (field == "entity_type") {
    return "en.kind";
  }
  if (field == "is_definition") {
    return "s.is_definition";
  }
  if (field == "is_pure") {
    return "s.is_pure";
  }
  if (field == "is_static") {
    return "s.is_static";
  }
  if (field == "file") {
    return "s.file_id";
  }
  if (field == "line") {
    return "s.line";
  }
  if (field == "col") {
    return "s.col";
  }
  throw PlanError("E_FIELD: unknown field '" + field + "'");
}

// entity_node.kind -> display name (entity_kind seed), null when out of range.
Cell entity_type_name_cell(int64_t raw) {
  static const char *names[] = {
      "other",          "class",
      "abstract_class", "interface",
      "union",          "enum",
      "class_template", "abstract_class_template",
      "interface_template", "namespace",
  };
  if (raw >= 0 && raw <= 9) {
    return Cell(std::string(names[raw]));
  }
  return Cell(nullptr);
}

// kind/entity_type predicate value: name -> stored int, by FIELD (not view).
int64_t kind_value_id(const std::string &field, const std::string &name) {
  return field == "entity_type" ? entity_kind_id(name) : symbol_kind_id(name);
}

// ---- predicate -> SQL ---------------------------------------------------------

bool is_kind_field(const std::string &field) {
  return field == "kind" || field == "entity_type";
}

void pred_sql(const Pred &p, std::string &sql, std::vector<SqlValue> &args) {
  switch (p.op) {
  case PredOp::AllOf:
  case PredOp::AnyOf: {
    const char *joiner = p.op == PredOp::AllOf ? " AND " : " OR ";
    sql += "(";
    for (size_t i = 0; i < p.kids.size(); ++i) {
      if (i != 0) {
        sql += joiner;
      }
      pred_sql(p.kids[i], sql, args);
    }
    sql += ")";
    return;
  }
  case PredOp::Not:
    sql += "NOT (";
    pred_sql(p.kids[0], sql, args);
    sql += ")";
    return;
  case PredOp::Eq:
  case PredOp::Ne: {
    sql += col_expr(p.field);
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
    sql += col_expr(p.field);
    sql += " GLOB ?";
    args.emplace_back(p.str_values[0]);
    return;
  case PredOp::In: {
    sql += col_expr(p.field);
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
  }
}

// ---- small SQL helpers ----------------------------------------------------------

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

// ---- executor state --------------------------------------------------------------

struct Stream {
  View view = View::Symbol;
  Shape shape = Shape::Nodes;
  std::vector<int64_t> ids; // nodes shape; ascending, deduped
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

class Exec {
public:
  explicit Exec(Storage &db) : db_(db) {}

  Stream run_plan(const Plan &plan) {
    Stream st;
    st.view = plan.source.kind == SourceKind::Entity ? View::Entity
                                                     : View::Symbol;
    if (plan.source.kind != SourceKind::Codebase) {
      st.ids = resolve_source(plan.source);
    }

    for (const auto &stage : plan.stages) {
      switch (stage.op) {
      case StageOp::Nodes:
        enumerate(st, stage.pred);
        st.limit_in_effect = false;
        break;
      case StageOp::ChangeView:
        change_view(st, stage.level);
        break;
      case StageOp::Where:
        filter(st, *stage.pred);
        break;
      case StageOp::Out:
      case StageOp::In:
        traverse(st, stage);
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
    return st;
  }

private:
  Storage &db_;
  std::map<int64_t, std::optional<std::string>> file_paths_;

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
    return false;
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

  void enumerate(Stream &st, const std::optional<Pred> &pred) {
    std::string sql = "SELECT s.id FROM symbol s";
    if (st.view == View::Entity) {
      sql += " JOIN entity_node en ON en.id = s.id";
    } else if (pred && pred_uses_entity_type(*pred)) {
      sql += join_clause(true);
    }
    std::vector<SqlValue> args;
    if (pred) {
      sql += " WHERE ";
      pred_sql(*pred, sql, args);
    }
    sql += " ORDER BY s.id LIMIT ?";
    args.emplace_back(kEnumerateBudget + 1);
    st.ids = fetch_ids(db_, sql, args);
    if (st.ids.size() > static_cast<size_t>(kEnumerateBudget)) {
      st.ids.resize(kEnumerateBudget);
      st.truncated = true;
    }
  }

  void filter(Stream &st, const Pred &pred) {
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
      pred_sql(pred, sql, args);
      sql += ") ORDER BY s.id";
      auto part = fetch_ids(db_, sql, args);
      out.insert(out.end(), part.begin(), part.end());
    }
    std::ranges::sort(out);
    out.erase(std::ranges::unique(out).begin(), out.end());
    st.ids = std::move(out);
  }

  // Path-length-window BFS (PR #20 review): a node is emitted iff SOME path
  // of length d in [min_depth, max_depth] reaches it -- NOT only its shortest
  // first-discovery depth. There is no cross-level visited set; termination
  // is guaranteed by the finite max_depth (<= 32) and the state budget
  // (cumulative level sizes). In a diamond A->B, A->C->B, out(r, 2, 2)
  // therefore DOES emit B. The stream view follows the relation's layer.
  void traverse(Stream &st, const Stage &stage) {
    const RelationDesc *rel = resolve_relation(stage.relation, st.view);
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
        std::string sql = "SELECT DISTINCT " + to_col + " FROM " + table +
                          " WHERE kind = ? AND " + from_col + " IN (" +
                          placeholders(n) + ") ORDER BY 1";
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
      if (states + static_cast<int64_t>(level.size()) >
          kTraverseNodeBudget) {
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
      cols += ", " + col_expr(f);
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
                     f == "qual_name") {
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
      std::ranges::sort(st.ids);
      st.ids.erase(std::ranges::unique(st.ids).begin(), st.ids.end());
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
      if (std::cmp_greater(st.ids.size(), n)) {
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
    if (st.shape == Shape::Scalar) {
      res.shape = Shape::Scalar;
      // count() after select carries rows; otherwise ids hold the stream.
      res.scalar = static_cast<int64_t>(st.rows.empty() ? st.ids.size()
                                                        : st.rows.size());
      return res;
    }
    if (st.shape == Shape::Nodes) {
      materialize(st, {"id", "usr", "name", "kind"});
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
  o.emplace_back("shape",
                 Value::of(std::string(shape == Shape::Nodes  ? "nodes"
                                       : shape == Shape::Rows ? "rows"
                                                              : "scalar")));
  o.emplace_back("view", Value::of(std::string(view_name(view))));
  if (shape == Shape::Scalar) {
    o.emplace_back("count", Value::of(scalar));
    o.emplace_back("truncated", Value::of(truncated));
    return Value::obj(std::move(o));
  }
  o.emplace_back("count", Value::of(static_cast<int64_t>(rows.size())));
  o.emplace_back("truncated", Value::of(truncated));
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

Result Executor::run(const Plan &plan) {
  const Plan normalized = validate(plan);
  Exec exec(db_);
  Stream st = exec.run_plan(normalized);
  Result res = exec.finish(std::move(st));
  return res;
}

} // namespace cidx::query
