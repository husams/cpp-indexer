// query/plan.cpp -- catalog data, builder factories, validate/normalize,
// canonical JSON. Contract: docs/query-plan.md (v1).

#include "query/plan.hpp"

#include <algorithm>

#include "catalogs/generated_catalog.hpp"
#include "storage/storage.hpp"

namespace cidx::query {

// ---- Views
// --------------------------------------------------------------------

const char *view_name(View v) {
  return v == View::Symbol ? "symbol" : "entity";
}

// ---- Relation catalog
// -----------------------------------------------------------

const std::vector<RelationDesc> &relation_catalog() {
  static const std::vector<RelationDesc> cat = [] {
    std::vector<RelationDesc> result;
    result.reserve(catalog::kRelations.size());
    for (const auto &relation : catalog::kRelations) {
      result.push_back({
          .name = std::string(relation.name),
          .layer = relation.layer == catalog::View::Symbol ? View::Symbol
                                                           : View::Entity,
          .kind_id = relation.id,
          .source = std::string(relation.source),
          .target = std::string(relation.target),
          .inverse = std::string(relation.inverse),
          .traversal = std::string(relation.traversal),
          .evidence = std::string(relation.evidence),
          .evidence_capabilities = std::string(relation.evidence_capabilities),
          .completeness = std::string(relation.completeness),
      });
    }
    return result;
  }();
  return cat;
}

const std::array<catalog::ExtensionRelation,
                 catalog::kExtensionRelations.size()> &
extension_relation_catalog() {
  return catalog::kExtensionRelations;
}

const RelationDesc *resolve_relation(const std::string &name, View active) {
  std::string bare = name;
  std::optional<View> forced;
  if (name.starts_with("symbol.")) {
    forced = View::Symbol;
    bare = name.substr(7);
  } else if (name.starts_with("entity.")) {
    forced = View::Entity;
    bare = name.substr(7);
  }
  const View layer = forced.value_or(active);
  for (const auto &r : relation_catalog()) {
    if (r.layer == layer && r.name == bare) {
      return &r;
    }
  }
  return nullptr;
}

// ---- entity_kind names (entity_kind seed)
// ----------------------------------------

namespace {

const std::vector<std::string> &entity_kind_names() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> result;
    result.reserve(catalog::kEntityKinds.size());
    for (const auto &kind : catalog::kEntityKinds) {
      result.emplace_back(kind.name);
    }
    return result;
  }();
  return names;
}

bool is_entity_kind(const std::string &name) {
  const auto &names = entity_kind_names();
  return std::ranges::find(names, name) != names.end();
}

// entity_kind name -> id (matches the entity_kind seed values 0..9).
int64_t entity_kind_id_of(const std::string &name) {
  const auto &names = entity_kind_names();
  auto it = std::ranges::find(names, name);
  return it == names.end() ? -1 : static_cast<int64_t>(it - names.begin());
}

// ---- field catalog
// ---------------------------------------------------------------

struct FieldDesc {
  std::string_view name;
  bool filterable; // usable in nodes()/where() predicates
  bool is_string;  // string comparisons (eq/ne/glob/in); else int/bool
};

const std::vector<FieldDesc> &field_catalog() {
  static const std::vector<FieldDesc> f = [] {
    std::vector<FieldDesc> result;
    result.reserve(catalog::kFields.size());
    for (const auto &field : catalog::kFields) {
      result.push_back({.name = field.name,
                        .filterable = field.filterable,
                        .is_string = field.is_string});
    }
    return result;
  }();
  return f;
}

const FieldDesc *field_desc(const std::string &name) {
  for (const auto &f : field_catalog()) {
    if (name == f.name) {
      return &f;
    }
  }
  return nullptr;
}

[[noreturn]] void fail(const std::string &code, const std::string &what) {
  throw PlanError(code + ": " + what);
}

} // namespace

int64_t entity_kind_id(const std::string &name) {
  return entity_kind_id_of(name);
}

// ---- Pred builders
// ---------------------------------------------------------------

Pred all_of(std::vector<Pred> preds) {
  Pred p;
  p.op = PredOp::AllOf;
  p.kids = std::move(preds);
  return p;
}

Pred any_of(std::vector<Pred> preds) {
  Pred p;
  p.op = PredOp::AnyOf;
  p.kids = std::move(preds);
  return p;
}

Pred not_(Pred inner) {
  Pred p;
  p.op = PredOp::Not;
  p.kids.push_back(std::move(inner));
  return p;
}

Pred eq(const std::string &field, const std::string &value) {
  Pred p;
  p.op = PredOp::Eq;
  p.field = field;
  p.str_values.push_back(value);
  return p;
}

Pred eq(const std::string &field, const char *value) {
  return eq(field, std::string(value));
}

Pred eq(const std::string &field, int64_t value) {
  Pred p;
  p.op = PredOp::Eq;
  p.field = field;
  p.int_value = value;
  return p;
}

Pred eq(const std::string &field, bool value) {
  return eq(field, static_cast<int64_t>(value ? 1 : 0));
}

Pred ne(const std::string &field, const std::string &value) {
  Pred p = eq(field, value);
  p.op = PredOp::Ne;
  return p;
}

Pred glob(const std::string &field, const std::string &pattern) {
  Pred p = eq(field, pattern);
  p.op = PredOp::Glob;
  return p;
}

Pred in_list(const std::string &field, std::vector<std::string> values) {
  Pred p;
  p.op = PredOp::In;
  p.field = field;
  p.str_values = std::move(values);
  return p;
}

// ---- Stage factories
// --------------------------------------------------------------

const char *stage_op_name(StageOp op) {
  switch (op) {
  case StageOp::Nodes:
    return "nodes";
  case StageOp::ChangeView:
    return "view";
  case StageOp::Where:
    return "where";
  case StageOp::Out:
    return "out";
  case StageOp::In:
    return "in";
  case StageOp::Union:
    return "union";
  case StageOp::Intersect:
    return "intersect";
  case StageOp::Except:
    return "except";
  case StageOp::Select:
    return "select";
  case StageOp::Count:
    return "count";
  case StageOp::Distinct:
    return "distinct";
  case StageOp::OrderBy:
    return "order_by";
  case StageOp::Limit:
    return "limit";
  }
  return "?";
}

const char *traversal_mode_name(TraversalMode mode) {
  return mode == TraversalMode::Devirtualized ? "devirtualized" : "static";
}

Source codebase() { return Source{.kind = SourceKind::Codebase, .ref = ""}; }
Source symbol(const std::string &ref) {
  return Source{.kind = SourceKind::Symbol, .ref = ref};
}
Source entity(const std::string &ref) {
  return Source{.kind = SourceKind::Entity, .ref = ref};
}

Stage nodes() {
  Stage s;
  s.op = StageOp::Nodes;
  return s;
}

Stage nodes(Pred pred) {
  Stage s;
  s.op = StageOp::Nodes;
  s.pred = std::move(pred);
  return s;
}

Stage view(View level) {
  Stage s;
  s.op = StageOp::ChangeView;
  s.level = level;
  return s;
}

Stage where(Pred pred) {
  Stage s;
  s.op = StageOp::Where;
  s.pred = std::move(pred);
  return s;
}

Stage out(const std::string &relation, int64_t min_depth, int64_t max_depth,
          TraversalMode mode) {
  Stage s;
  s.op = StageOp::Out;
  s.relation = relation;
  s.mode = mode;
  s.min_depth = min_depth;
  s.max_depth = max_depth;
  return s;
}

Stage in_(const std::string &relation, int64_t min_depth, int64_t max_depth) {
  Stage s = out(relation, min_depth, max_depth);
  s.op = StageOp::In;
  return s;
}

namespace {
Stage set_stage(StageOp op, const Query &operand) {
  Stage s;
  s.op = op;
  s.operand = std::make_shared<Plan>(operand.plan());
  return s;
}
} // namespace

Stage union_(const Query &operand) {
  return set_stage(StageOp::Union, operand);
}
Stage intersect(const Query &operand) {
  return set_stage(StageOp::Intersect, operand);
}
Stage except_(const Query &operand) {
  return set_stage(StageOp::Except, operand);
}

Stage select(std::vector<std::string> fields) {
  Stage s;
  s.op = StageOp::Select;
  s.fields = std::move(fields);
  return s;
}

Stage count() {
  Stage s;
  s.op = StageOp::Count;
  return s;
}

Stage distinct() {
  Stage s;
  s.op = StageOp::Distinct;
  return s;
}

Stage order_by(std::vector<std::string> fields) {
  Stage s;
  s.op = StageOp::OrderBy;
  s.fields = std::move(fields);
  return s;
}

Stage limit(int64_t n) {
  Stage s;
  s.op = StageOp::Limit;
  s.n = n;
  return s;
}

// ---- Predicate validation + normalization
// -----------------------------------------

namespace {

// Validate one comparison leaf against the field catalog. `kind` always takes
// C++ declaration-kind names (symbol_kind) and `entity_type` always takes
// entity-classification names (entity_kind) -- view-independent, so an
// `abstract struct` is `kind = struct` AND `entity_type = abstract_class`.
// Throws PlanError.
void check_cmp(const Pred &p) {
  const FieldDesc *f = field_desc(p.field);
  if (f == nullptr) {
    fail("E_FIELD", "unknown field '" + p.field + "'");
  }
  if (!f->filterable) {
    fail("E_FIELD", "field '" + p.field + "' is select-only");
  }
  if (f->is_string) {
    if (p.int_value.has_value()) {
      fail("E_FIELD", "field '" + p.field + "' takes a string value");
    }
    if (p.op == PredOp::In ? p.str_values.empty() : p.str_values.size() != 1) {
      fail("E_FIELD", "bad value arity for field '" + p.field + "'");
    }
  } else {
    if (p.op == PredOp::Glob || p.op == PredOp::In) {
      fail("E_FIELD", "field '" + p.field + "' supports eq/ne only");
    }
    if (!p.int_value.has_value()) {
      fail("E_FIELD", "field '" + p.field + "' takes an integer value");
    }
  }
  if ((p.field == "kind" || p.field == "entity_type") && p.op == PredOp::Glob) {
    fail("E_FIELD", "field '" + p.field + "' does not support glob");
  }
  if (p.field == "kind") {
    for (const auto &v : p.str_values) {
      if (!is_symbol_kind(v)) {
        fail("E_KIND", "unknown symbol kind '" + v + "'");
      }
    }
  }
  if (p.field == "entity_type") {
    for (const auto &v : p.str_values) {
      if (!is_entity_kind(v)) {
        fail("E_KIND", "unknown entity_type '" + v + "'");
      }
    }
  }
}

// Validate + normalize a predicate tree: flatten nested AllOf/AnyOf, reduce
// not(not(p)).
Pred norm_pred(const Pred &p) {
  switch (p.op) {
  case PredOp::AllOf:
  case PredOp::AnyOf: {
    if (p.kids.empty()) {
      fail("E_FIELD", "empty boolean combinator");
    }
    Pred out;
    out.op = p.op;
    for (const auto &k : p.kids) {
      Pred nk = norm_pred(k);
      if (nk.op == p.op) {
        for (auto &g : nk.kids) {
          out.kids.push_back(std::move(g));
        }
      } else {
        out.kids.push_back(std::move(nk));
      }
    }
    if (out.kids.size() == 1) {
      return out.kids[0];
    }
    return out;
  }
  case PredOp::Not: {
    if (p.kids.size() != 1) {
      fail("E_FIELD", "not() takes exactly one predicate");
    }
    Pred nk = norm_pred(p.kids[0]);
    if (nk.op == PredOp::Not) {
      return nk.kids[0]; // not(not(p)) -> p
    }
    Pred out;
    out.op = PredOp::Not;
    out.kids.push_back(std::move(nk));
    return out;
  }
  case PredOp::Eq:
  case PredOp::Ne:
  case PredOp::Glob:
  case PredOp::In:
    check_cmp(p);
    return p;
  }
  fail("E_FIELD", "bad predicate");
}

} // namespace

// ---- validate()
// --------------------------------------------------------------------

namespace {

struct WalkState {
  View active = View::Symbol;
  Shape shape = Shape::Nodes;
  bool codebase_unenumerated = false; // codebase() with only view() so far
  std::vector<std::string> selected;  // fields after Select
};

Plan validate_walk(const Plan &plan, WalkState &st) {
  Plan out;
  out.source = plan.source;
  // Consuming a codebase() stream before nodes() would be an implicit
  // whole-graph traversal -- rejected (docs/query-plan.md E_STAGE).
  const auto consume = [&st]() {
    if (st.codebase_unenumerated) {
      fail("E_STAGE", "codebase() must be enumerated with nodes() first");
    }
  };

  switch (plan.source.kind) {
  case SourceKind::Codebase:
    st.codebase_unenumerated = true;
    break;
  case SourceKind::Symbol:
  case SourceKind::Entity:
    if (plan.source.ref.empty()) {
      fail("E_SOURCE", "empty source ref");
    }
    break;
  }
  // entity(ref) starts in the entity view; symbol(ref)/codebase() in symbol.
  st.active =
      plan.source.kind == SourceKind::Entity ? View::Entity : View::Symbol;

  for (const auto &stage : plan.stages) {
    Stage ns = stage;
    if (st.shape == Shape::Scalar) {
      fail("E_STAGE", "no stage may follow count()");
    }
    switch (stage.op) {
    case StageOp::Nodes:
      if (!st.codebase_unenumerated) {
        fail("E_STAGE", "nodes() requires an unenumerated codebase() source");
      }
      if (stage.pred) {
        ns.pred = norm_pred(*stage.pred);
      }
      st.codebase_unenumerated = false;
      break;
    case StageOp::ChangeView:
      if (st.shape != Shape::Nodes) {
        fail("E_STAGE", "view() applies to a node stream");
      }
      st.active = stage.level;
      break;
    case StageOp::Where:
      if (st.shape != Shape::Nodes) {
        fail("E_STAGE", "where() applies to a node stream");
      }
      if (!stage.pred) {
        fail("E_FIELD", "where() requires a predicate");
      }
      consume();
      ns.pred = norm_pred(*stage.pred);
      break;
    case StageOp::Out:
    case StageOp::In: {
      consume();
      if (st.shape != Shape::Nodes) {
        fail("E_STAGE", "traversal applies to a node stream");
      }
      const RelationDesc *r = resolve_relation(stage.relation, st.active);
      if (r == nullptr) {
        fail("E_RELATION", "unknown relation '" + stage.relation + "' in " +
                               view_name(st.active) + " view");
      }
      if (stage.min_depth < 1 || stage.min_depth > stage.max_depth ||
          stage.max_depth > 32) {
        fail("E_DEPTH", "depth bounds must satisfy 1 <= min <= max <= 32");
      }
      if (stage.mode == TraversalMode::Devirtualized &&
          (stage.op != StageOp::Out || r->layer != View::Symbol ||
           r->name != "calls")) {
        fail("E_STAGE",
             "devirtualized mode requires an outbound symbol.calls traversal");
      }
      ns.relation = std::string(view_name(r->layer)) + "." + r->name;
      // Traversal targets live in the relation's layer: the stream view (and
      // therefore later bare-relation resolution) follows it.
      st.active = r->layer;
      break;
    }
    case StageOp::Union:
    case StageOp::Intersect:
    case StageOp::Except: {
      consume();
      if (st.shape != Shape::Nodes) {
        fail("E_STAGE", "set operations apply to node streams");
      }
      if (!stage.operand) {
        fail("E_SETOP", "missing operand plan");
      }
      WalkState sub;
      Plan nop = validate_walk(*stage.operand, sub);
      if (sub.shape != Shape::Nodes) {
        fail("E_SETOP", "operand must yield a node stream");
      }
      if (sub.active != st.active) {
        fail("E_SETOP", "operand view mismatch");
      }
      ns.operand = std::make_shared<Plan>(std::move(nop));
      break;
    }
    case StageOp::Select: {
      consume();
      if (st.shape != Shape::Nodes) {
        fail("E_STAGE", "select() applies to a node stream");
      }
      if (stage.fields.empty()) {
        fail("E_FIELD", "select() requires at least one field");
      }
      for (const auto &f : stage.fields) {
        if (field_desc(f) == nullptr) {
          fail("E_FIELD", "unknown field '" + f + "'");
        }
      }
      st.shape = Shape::Rows;
      st.selected = stage.fields;
      break;
    }
    case StageOp::Count:
      consume();
      st.shape = Shape::Scalar;
      break;
    case StageOp::Distinct:
      consume();
      break;
    case StageOp::OrderBy: {
      consume();
      if (stage.fields.empty()) {
        fail("E_FIELD", "order_by() requires at least one field");
      }
      for (const auto &f : stage.fields) {
        if (field_desc(f) == nullptr) {
          fail("E_FIELD", "unknown field '" + f + "'");
        }
        if (st.shape == Shape::Rows &&
            std::ranges::find(st.selected, f) == st.selected.end()) {
          fail("E_FIELD", "order_by field '" + f + "' is not selected");
        }
      }
      break;
    }
    case StageOp::Limit:
      consume();
      if (stage.n < 1) {
        fail("E_LIMIT", "limit must be >= 1");
      }
      break;
    }
    out.stages.push_back(std::move(ns));
  }
  if (st.codebase_unenumerated) {
    fail("E_STAGE", "codebase() must be enumerated with nodes() first");
  }
  return out;
}

} // namespace

Plan validate(const Plan &plan) {
  WalkState st;
  return validate_walk(plan, st);
}

View final_view(const Plan &plan) {
  WalkState st;
  (void)validate_walk(plan, st);
  return st.active;
}

// ---- canonical JSON
// ------------------------------------------------------------------

namespace {

json_out::Value pred_to_json(const Pred &p) {
  using namespace json_out;
  Object o;
  switch (p.op) {
  case PredOp::AllOf:
  case PredOp::AnyOf: {
    o.emplace_back("op", Value::of(std::string(
                             p.op == PredOp::AllOf ? "all_of" : "any_of")));
    Array kids;
    for (const auto &k : p.kids) {
      kids.push_back(pred_to_json(k));
    }
    o.emplace_back("preds", Value::arr(std::move(kids)));
    break;
  }
  case PredOp::Not:
    o.emplace_back("op", Value::of(std::string("not")));
    o.emplace_back("pred", pred_to_json(p.kids[0]));
    break;
  case PredOp::Eq:
  case PredOp::Ne:
  case PredOp::Glob: {
    const char *name = p.op == PredOp::Eq   ? "eq"
                       : p.op == PredOp::Ne ? "ne"
                                            : "glob";
    o.emplace_back("op", Value::of(std::string(name)));
    o.emplace_back("field", Value::of(p.field));
    if (p.int_value.has_value()) {
      o.emplace_back("value", Value::of(*p.int_value));
    } else {
      o.emplace_back("value", Value::of(p.str_values[0]));
    }
    break;
  }
  case PredOp::In: {
    o.emplace_back("op", Value::of(std::string("in")));
    o.emplace_back("field", Value::of(p.field));
    Array vals;
    for (const auto &v : p.str_values) {
      vals.push_back(Value::of(v));
    }
    o.emplace_back("values", Value::arr(std::move(vals)));
    break;
  }
  }
  return Value::obj(std::move(o));
}

json_out::Value plan_to_json_normalized(const Plan &plan) {
  using namespace json_out;
  Object root;
  root.emplace_back("cxq", Value::of(static_cast<int64_t>(1)));
  Object src;
  const char *skind = plan.source.kind == SourceKind::Codebase ? "codebase"
                      : plan.source.kind == SourceKind::Symbol ? "symbol"
                                                               : "entity";
  src.emplace_back("kind", Value::of(std::string(skind)));
  if (plan.source.kind != SourceKind::Codebase) {
    src.emplace_back("ref", Value::of(plan.source.ref));
  }
  root.emplace_back("source", Value::obj(std::move(src)));

  Array stages;
  for (const auto &s : plan.stages) {
    Object o;
    o.emplace_back("op", Value::of(std::string(stage_op_name(s.op))));
    switch (s.op) {
    case StageOp::Nodes:
      if (s.pred) {
        o.emplace_back("pred", pred_to_json(*s.pred));
      }
      break;
    case StageOp::ChangeView:
      o.emplace_back("level", Value::of(std::string(view_name(s.level))));
      break;
    case StageOp::Where:
      o.emplace_back("pred", pred_to_json(*s.pred));
      break;
    case StageOp::Out:
    case StageOp::In:
      o.emplace_back("relation", Value::of(s.relation));
      if (s.mode != TraversalMode::Static) {
        o.emplace_back("mode",
                       Value::of(std::string(traversal_mode_name(s.mode))));
      }
      o.emplace_back("min_depth", Value::of(s.min_depth));
      o.emplace_back("max_depth", Value::of(s.max_depth));
      break;
    case StageOp::Union:
    case StageOp::Intersect:
    case StageOp::Except:
      o.emplace_back("plan", plan_to_json_normalized(*s.operand));
      break;
    case StageOp::Select:
    case StageOp::OrderBy: {
      Array f;
      for (const auto &n : s.fields) {
        f.push_back(Value::of(n));
      }
      o.emplace_back("fields", Value::arr(std::move(f)));
      break;
    }
    case StageOp::Count:
    case StageOp::Distinct:
      break;
    case StageOp::Limit:
      o.emplace_back("n", Value::of(s.n));
      break;
    }
    stages.push_back(Value::obj(std::move(o)));
  }
  root.emplace_back("stages", Value::arr(std::move(stages)));
  return Value::obj(std::move(root));
}

} // namespace

json_out::Value plan_to_json(const Plan &plan) {
  return plan_to_json_normalized(validate(plan));
}

std::string canonical_json(const Plan &plan) {
  return json_out::dumps_indent2(plan_to_json(plan));
}

} // namespace cidx::query
