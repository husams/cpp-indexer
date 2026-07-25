// query/plan.cpp -- catalog data, builder factories, validate/normalize,
// canonical JSON. Contract: docs/query-plan.md (v1).

#include "query/plan.hpp"

#include <algorithm>
#include <array>

#include "catalogs/generated_catalog.hpp"
#include "storage/storage.hpp"

namespace cidx::query {

// ---- Views
// --------------------------------------------------------------------

const char *view_name(View v) {
  switch (v) {
  case View::Symbol:
    return "symbol";
  case View::Entity:
    return "entity";
  case View::Parameter:
    return "parameter";
  case View::TemplateParameter:
    return "template_parameter";
  case View::TemplateArgument:
    return "template_argument";
  case View::SignatureSlot:
    return "signature_slot";
  case View::CallArgument:
    return "call_argument";
  case View::Edge:
    return "edge";
  case View::Site:
    return "site";
  case View::Evidence:
    return "evidence";
  case View::Type:
    return "type";
  case View::TypeLayer:
    return "type_layer";
  }
  return "symbol";
}

namespace {

View view_from_domain(std::string_view domain) {
  const auto dot = domain.find('.');
  const auto name = domain.substr(0, dot);
  for (const auto view :
       {View::Symbol, View::Entity, View::Parameter, View::TemplateParameter,
        View::TemplateArgument, View::SignatureSlot, View::CallArgument,
        View::Edge, View::Site, View::Evidence, View::Type, View::TypeLayer}) {
    if (name == view_name(view)) {
      return view;
    }
  }
  return View::Symbol;
}

View catalog_view(catalog::View view) {
  switch (view) {
  case catalog::View::Symbol:
    return View::Symbol;
  case catalog::View::Entity:
    return View::Entity;
  case catalog::View::Parameter:
    return View::Parameter;
  case catalog::View::TemplateParameter:
    return View::TemplateParameter;
  case catalog::View::TemplateArgument:
    return View::TemplateArgument;
  case catalog::View::SignatureSlot:
    return View::SignatureSlot;
  case catalog::View::CallArgument:
    return View::CallArgument;
  case catalog::View::Edge:
    return View::Edge;
  case catalog::View::Site:
    return View::Site;
  case catalog::View::Evidence:
    return View::Evidence;
  case catalog::View::Type:
    return View::Type;
  case catalog::View::TypeLayer:
    return View::TypeLayer;
  }
  return View::Symbol;
}

} // namespace

// ---- Relation catalog
// -----------------------------------------------------------

const std::vector<RelationDesc> &relation_catalog() {
  static const std::vector<RelationDesc> cat = [] {
    std::vector<RelationDesc> result;
    result.reserve(catalog::kRelations.size());
    for (const auto &relation : catalog::kRelations) {
      result.push_back({
          .name = std::string(relation.name),
          .layer = catalog_view(relation.layer),
          .kind_id = relation.id,
          .source = std::string(relation.source),
          .target = std::string(relation.target),
          .inverse = std::string(relation.inverse),
          .traversal = std::string(relation.traversal),
          .evidence = std::string(relation.evidence),
          .evidence_capabilities = std::string(relation.evidence_capabilities),
          .completeness = std::string(relation.completeness),
          .virtual_relation = relation.virtual_relation,
          .target_view = view_from_domain(relation.target),
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

const RelationDesc *resolve_relation(const std::string &name, View active,
                                     bool inbound) {
  std::string bare = name;
  std::optional<View> forced;
  if (name.starts_with("symbol.")) {
    forced = View::Symbol;
    bare = name.substr(7);
  } else if (name.starts_with("entity.")) {
    forced = View::Entity;
    bare = name.substr(7);
  } else {
    for (const auto view :
         {View::Parameter, View::TemplateParameter, View::TemplateArgument,
          View::SignatureSlot, View::CallArgument, View::Edge, View::Site,
          View::Evidence, View::Type, View::TypeLayer}) {
      const std::string prefix = std::string(view_name(view)) + ".";
      if (name.starts_with(prefix)) {
        forced = view;
        bare = name.substr(prefix.size());
        break;
      }
    }
  }
  for (const auto &r : relation_catalog()) {
    const bool matches = inbound && forced
                             ? (r.layer == *forced && r.target_view == active)
                         : inbound ? r.target_view == active
                                   : r.layer == forced.value_or(active);
    if (matches && r.name == bare) {
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

namespace {

Pred relation_pred(PredOp op, const std::string &relation,
                   std::optional<Pred> target, int64_t min_depth,
                   int64_t max_depth, bool inbound, int64_t threshold = 0) {
  Pred p;
  p.op = op;
  p.relation = relation;
  p.min_depth = min_depth;
  p.max_depth = max_depth;
  p.inbound = inbound;
  p.threshold = threshold;
  if (target) {
    p.target = std::make_shared<Pred>(std::move(*target));
  }
  return p;
}

Pred target_ref(const std::string &ref) {
  return any_of({eq("usr", ref), eq("qual_name", ref), eq("spelling", ref)});
}

Pred target_set_pred(const std::string &relation, const TargetSet &targets,
                     bool inbound, int64_t min_depth, int64_t max_depth) {
  if (targets.refs.empty()) {
    return targets.kind == TargetSet::Kind::Any ? any_of({}) : all_of({});
  }
  std::vector<Pred> parts;
  parts.reserve(targets.refs.size());
  for (const auto &ref : targets.refs) {
    parts.push_back(
        exists(relation, target_ref(ref), min_depth, max_depth, inbound));
  }
  if (targets.kind == TargetSet::Kind::Any) {
    return parts.size() == 1 ? parts.front() : any_of(std::move(parts));
  }
  if (targets.kind == TargetSet::Kind::All) {
    return parts.size() == 1 ? parts.front() : all_of(std::move(parts));
  }
  std::vector<Pred> targets_pred;
  targets_pred.reserve(targets.refs.size());
  for (const auto &ref : targets.refs) {
    targets_pred.push_back(target_ref(ref));
  }
  return none(relation, any_of(std::move(targets_pred)), min_depth, max_depth,
              inbound);
}

Pred relation_target(const std::string &relation, const std::string &target,
                     bool inbound = false, int64_t min_depth = 1,
                     int64_t max_depth = 1) {
  return exists(relation, target_ref(target), min_depth, max_depth, inbound);
}

} // namespace

Pred exists(const std::string &relation, std::optional<Pred> target,
            int64_t min_depth, int64_t max_depth, bool inbound) {
  return relation_pred(PredOp::Exists, relation, std::move(target), min_depth,
                       max_depth, inbound);
}

Pred none(const std::string &relation, std::optional<Pred> target,
          int64_t min_depth, int64_t max_depth, bool inbound) {
  return relation_pred(PredOp::None, relation, std::move(target), min_depth,
                       max_depth, inbound);
}

Pred all(const std::string &relation, std::optional<Pred> target,
         int64_t min_depth, int64_t max_depth, bool inbound) {
  return relation_pred(PredOp::All, relation, std::move(target), min_depth,
                       max_depth, inbound);
}

Pred at_least(int64_t threshold, const std::string &relation,
              std::optional<Pred> target, int64_t min_depth, int64_t max_depth,
              bool inbound) {
  return relation_pred(PredOp::AtLeast, relation, std::move(target), min_depth,
                       max_depth, inbound, threshold);
}

Pred exactly(int64_t threshold, const std::string &relation,
             std::optional<Pred> target, int64_t min_depth, int64_t max_depth,
             bool inbound) {
  return relation_pred(PredOp::Exactly, relation, std::move(target), min_depth,
                       max_depth, inbound, threshold);
}

TargetSet any_target(std::vector<std::string> refs) {
  return TargetSet{.kind = TargetSet::Kind::Any, .refs = std::move(refs)};
}

TargetSet all_targets(std::vector<std::string> refs) {
  return TargetSet{.kind = TargetSet::Kind::All, .refs = std::move(refs)};
}

TargetSet no_targets(std::vector<std::string> refs) {
  return TargetSet{.kind = TargetSet::Kind::None, .refs = std::move(refs)};
}

Pred inherits_from(const std::string &target, bool transitive) {
  return relation_target("inherits", target, false, 1, transitive ? 32 : 1);
}

Pred inherits_from(const TargetSet &targets, bool transitive) {
  return target_set_pred("inherits", targets, false, 1, transitive ? 32 : 1);
}

Pred implements(const std::string &target) {
  return relation_target("implements", target);
}

Pred implements(const TargetSet &targets) {
  return target_set_pred("implements", targets, false, 1, 1);
}

Pred has_ancestor(const std::string &target, bool transitive) {
  return inherits_from(target, transitive);
}

Pred has_member(std::optional<Pred> target) {
  return exists("field_of", std::move(target), 1, 1, true);
}

Pred has_method(std::optional<Pred> target) {
  return exists("method_of", std::move(target), 1, 1, true);
}

Pred has_field(std::optional<Pred> target) {
  return has_member(std::move(target));
}

Pred has_nested(std::optional<Pred> target) {
  return exists("contains", std::move(target));
}

Pred has_template_arg(std::optional<Pred> target) {
  return exists("instantiates", std::move(target));
}

Pred is_specialization_of(const std::string &target) {
  return relation_target("specializes", target);
}

Pred is_instantiation_of(const std::string &target) {
  return relation_target("instantiates", target);
}

Pred calls(std::optional<Pred> target) {
  return exists("calls", std::move(target));
}

Pred called_by(std::optional<Pred> target) {
  return exists("calls", std::move(target), 1, 1, true);
}

Pred uses(std::optional<Pred> target) {
  return exists("uses", std::move(target));
}

Pred used_by(std::optional<Pred> target) {
  return exists("uses", std::move(target), 1, 1, true);
}

Pred is_abstract() {
  return in_list("entity_type", {"abstract_class", "abstract_class_template"});
}

Pred is_interface() {
  return in_list("entity_type", {"interface", "interface_template"});
}

Pred is_pure() { return eq("is_pure", true); }

Pred is_static() { return eq("is_static", true); }

Pred is_template() {
  return in_list("kind", {"class-template", "function-template"});
}

Pred is_instance() { return exists("instantiates"); }

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
  case StageOp::Sites:
    return "sites";
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

const char *unknown_policy_name(UnknownPolicy policy) {
  switch (policy) {
  case UnknownPolicy::Exclude:
    return "exclude";
  case UnknownPolicy::Include:
    return "include";
  case UnknownPolicy::Error:
    return "error";
  }
  return "exclude";
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

Stage nodes(Pred pred, UnknownPolicy unknown) {
  Stage s;
  s.op = StageOp::Nodes;
  s.pred = std::move(pred);
  s.unknown = unknown;
  return s;
}

Stage view(View level) {
  Stage s;
  s.op = StageOp::ChangeView;
  s.level = level;
  return s;
}

Stage where(Pred pred, UnknownPolicy unknown) {
  Stage s;
  s.op = StageOp::Where;
  s.pred = std::move(pred);
  s.unknown = unknown;
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

Stage sites() {
  Stage s;
  s.op = StageOp::Sites;
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
bool field_available(View view, const std::string &name) {
  if (view == View::Symbol || view == View::Entity) {
    return field_desc(name) != nullptr;
  }
  if (name == "id" || name == "identity_key") {
    return true;
  }
  const auto has = [&name](const auto &names) {
    return std::ranges::find(names, name) != names.end();
  };
  switch (view) {
  case View::Parameter:
    return has(std::array{"owner_id", "position", "pack_index", "name",
                          "type_id", "declared_type_id", "adjusted_type_id",
                          "default_text", "default_origin",
                          "reference_semantics", "file_id", "line", "col"});
  case View::TemplateParameter:
    return has(std::array{"owner_id", "position", "param_kind", "name",
                          "default_txt", "type_id", "default_type_id",
                          "default_ref_id"});
  case View::TemplateArgument:
    return has(std::array{"owner_id", "position", "pack_index", "arg_kind",
                          "ref_id", "literal", "type_id"});
  case View::CallArgument:
    return has(std::array{"edge_id", "file_id", "line", "col", "position",
                          "src_kind", "type_usr", "decl_usr", "callee_usr",
                          "type_id", "decl_id", "callee_id", "type_is_value"});
  case View::Edge:
    return has(std::array{"src_id", "dst_id", "kind", "count", "base_access",
                          "is_virtual", "vtable_slot", "relation", "source",
                          "target", "evidence", "status", "partial", "unknown"});
  case View::Site:
    return has(std::array{"edge_id", "file_id", "file", "line", "col",
                          "relation", "source", "target", "evidence", "status",
                          "partial", "unknown"});
  case View::Evidence:
    return has(std::array{"owner_id", "position", "default_txt",
                          "default_type_id", "default_ref_id", "edge_id",
                          "file_id", "line", "col", "conditional", "args_sig",
                          "recv_src_kind", "recv_type_usr", "recv_decl_usr",
                          "recv_type_id", "recv_decl_id", "recv_param_pos",
                          "recv_type_is_value", "relation", "source", "target",
                          "evidence", "status", "partial", "unknown"});
  case View::Type:
    return has(std::array{"type_key", "spelling", "kind", "is_const",
                          "is_volatile", "is_restrict", "cv_qualifiers",
                          "decl_usr", "decl_id", "canonical_id", "extent"});
  case View::SignatureSlot:
    return has(std::array{"owner_id", "position", "pack_index", "slot_kind",
                          "name", "type_id", "declared_type_id",
                          "adjusted_type_id", "default_text", "default_origin",
                          "reference_semantics", "mode", "value_kind",
                          "named_decl"});
  case View::TypeLayer:
    return has(std::array{"root_id", "path", "relation", "position", "depth",
                          "status", "type_id", "spelling", "kind", "extent",
                          "element_type", "decl_usr", "canonical_id",
                          "is_const", "is_volatile", "is_restrict"});
  case View::Symbol:
  case View::Entity:
    break;
  }
  return false;
}

void check_cmp(const Pred &p, View active) {
  if (!field_available(active, p.field)) {
    fail("E_FIELD", "field '" + p.field + "' is unavailable in " +
                        std::string(view_name(active)) + " view");
  }
  if (active != View::Symbol && active != View::Entity &&
      p.op == PredOp::Glob) {
    fail("E_FIELD", "glob predicates are not supported for typed views");
  }
  if (active != View::Symbol && active != View::Entity) {
    constexpr std::array strings{"identity_key",  "name",
                                 "spelling",      "type_key",
                                 "default_text",  "default_origin",
                                 "default_txt",   "reference_semantics",
                                 "literal",       "src_kind",
                                 "type_usr",      "decl_usr",
                                 "callee_usr",    "args_sig",
                                 "recv_src_kind", "recv_type_usr",
                                 "recv_decl_usr", "slot_kind", "path",
                                 "relation",      "source", "target",
                                 "evidence",      "status", "extent", "kind",
                                 "mode", "value_kind", "named_decl"};
    const auto is_string = [&p, &strings] {
      return std::ranges::find(strings, p.field) != strings.end();
    };
    if (is_string()) {
      if (p.int_value.has_value()) {
        fail("E_FIELD", "field '" + p.field + "' takes a string value");
      }
      if (p.op == PredOp::In ? p.str_values.empty()
                             : p.str_values.size() != 1) {
        fail("E_FIELD", "bad value arity for field '" + p.field + "'");
      }
    } else {
      if (p.op == PredOp::In) {
        fail("E_FIELD", "field '" + p.field + "' supports eq/ne only");
      }
      if (!p.int_value.has_value()) {
        fail("E_FIELD", "field '" + p.field + "' takes an integer value");
      }
    }
    return;
  }
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
// not(not(p)), and qualify relationship quantifiers.
Pred norm_pred(const Pred &p, View active) {
  switch (p.op) {
  case PredOp::AllOf:
  case PredOp::AnyOf: {
    if (p.kids.empty()) {
      fail("E_FIELD", "empty boolean combinator");
    }
    Pred out;
    out.op = p.op;
    for (const auto &k : p.kids) {
      Pred nk = norm_pred(k, active);
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
    Pred nk = norm_pred(p.kids[0], active);
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
    check_cmp(p, active);
    return p;
  case PredOp::Exists:
  case PredOp::None:
  case PredOp::All:
  case PredOp::AtLeast:
  case PredOp::Exactly: {
    const RelationDesc *r = resolve_relation(p.relation, active);
    if (r == nullptr) {
      fail("E_RELATION", "unknown relation '" + p.relation + "' in " +
                             view_name(active) + " view");
    }
    if (p.min_depth < 1 || p.min_depth > p.max_depth || p.max_depth > 32) {
      fail("E_DEPTH", "depth bounds must satisfy 1 <= min <= max <= 32");
    }
    if ((p.op == PredOp::AtLeast || p.op == PredOp::Exactly) &&
        p.threshold < 0) {
      fail("E_LIMIT", "quantifier threshold must be >= 0");
    }
    if (p.target) {
      Pred out = p;
      out.relation = std::string(view_name(r->layer)) + "." + r->name;
      out.target = std::make_shared<Pred>(norm_pred(*p.target, r->layer));
      return out;
    }
    Pred out = p;
    out.relation = std::string(view_name(r->layer)) + "." + r->name;
    return out;
  }
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

bool is_known_view(View view) {
  return view == View::Symbol || view == View::Entity ||
         view == View::Parameter || view == View::TemplateParameter ||
         view == View::TemplateArgument || view == View::SignatureSlot ||
         view == View::CallArgument || view == View::Edge || view == View::Site ||
         view == View::Evidence || view == View::Type || view == View::TypeLayer;
}

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
        ns.pred = norm_pred(*stage.pred, st.active);
      }
      st.codebase_unenumerated = false;
      break;
    case StageOp::ChangeView: {
      if (st.shape != Shape::Nodes) {
        fail("E_STAGE", "view() applies to a node stream");
      }
      if (!is_known_view(stage.level)) {
        fail("E_VIEW",
             "unknown view '" + std::string(view_name(stage.level)) + "'");
      }
      const bool logical_transition =
          (st.active == View::Symbol || st.active == View::Entity) &&
          (stage.level == View::Symbol || stage.level == View::Entity);
      if (!st.codebase_unenumerated && stage.level != st.active &&
          !logical_transition) {
        fail("E_VIEW", "cannot change view from " +
                           std::string(view_name(st.active)) + " to " +
                           std::string(view_name(stage.level)));
      }
      st.active = stage.level;
      break;
    }
    case StageOp::Where:
      if (st.shape != Shape::Nodes) {
        fail("E_STAGE", "where() applies to a node stream");
      }
      if (!stage.pred) {
        fail("E_FIELD", "where() requires a predicate");
      }
      consume();
      ns.pred = norm_pred(*stage.pred, st.active);
      break;
    case StageOp::Out:
    case StageOp::In: {
      consume();
      if (st.shape != Shape::Nodes) {
        fail("E_STAGE", "traversal applies to a node stream");
      }
      const bool inbound = stage.op == StageOp::In;
      const RelationDesc *r =
          resolve_relation(stage.relation, st.active, inbound);
      if (r == nullptr) {
        fail("E_RELATION", "unknown relation '" + stage.relation + "' in " +
                               view_name(st.active) + " view");
      }
      if (stage.min_depth < 1 || stage.min_depth > stage.max_depth ||
          stage.max_depth > 32) {
        fail("E_DEPTH", "depth bounds must satisfy 1 <= min <= max <= 32");
      }
      const bool typed =
          r->virtual_relation ||
          (st.active != View::Symbol && st.active != View::Entity) ||
          (r->target_view != View::Symbol && r->target_view != View::Entity);
      if (typed && (stage.min_depth != 1 || stage.max_depth != 1)) {
        fail("E_DEPTH", "typed traversal currently supports only depth 1..1");
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
      st.active = inbound ? r->layer : r->target_view;
      break;
    }
    case StageOp::Sites:
      consume();
      if (st.shape != Shape::Nodes) {
        fail("E_STAGE", "sites() applies to a node stream");
      }
      if (st.active != View::Edge) {
        fail("E_VIEW", "sites() requires an edge node stream");
      }
      st.active = View::Site;
      break;
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
        if (!field_available(st.active, f)) {
          fail("E_FIELD", "field '" + f + "' is unavailable in " +
                              std::string(view_name(st.active)) + " view");
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
        if (!field_available(st.active, f)) {
          fail("E_FIELD", "field '" + f + "' is unavailable in " +
                              std::string(view_name(st.active)) + " view");
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
    const char *name = "glob";
    if (p.op == PredOp::Eq) {
      name = "eq";
    } else if (p.op == PredOp::Ne) {
      name = "ne";
    }
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
  case PredOp::Exists:
  case PredOp::None:
  case PredOp::All:
  case PredOp::AtLeast:
  case PredOp::Exactly: {
    const char *name = "exactly";
    if (p.op == PredOp::Exists) {
      name = "exists";
    } else if (p.op == PredOp::None) {
      name = "none";
    } else if (p.op == PredOp::All) {
      name = "all";
    } else if (p.op == PredOp::AtLeast) {
      name = "at_least";
    }
    o.emplace_back("op", Value::of(std::string(name)));
    o.emplace_back("relation", Value::of(p.relation));
    if (p.inbound) {
      o.emplace_back("direction", Value::of(std::string("in")));
    }
    o.emplace_back("min_depth", Value::of(p.min_depth));
    o.emplace_back("max_depth", Value::of(p.max_depth));
    if (p.op == PredOp::AtLeast || p.op == PredOp::Exactly) {
      o.emplace_back("threshold", Value::of(p.threshold));
    }
    if (p.target) {
      o.emplace_back("pred", pred_to_json(*p.target));
    }
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
      if (s.unknown != UnknownPolicy::Exclude) {
        o.emplace_back("unknown",
                       Value::of(std::string(unknown_policy_name(s.unknown))));
      }
      break;
    case StageOp::ChangeView:
      o.emplace_back("level", Value::of(std::string(view_name(s.level))));
      break;
    case StageOp::Where:
      o.emplace_back("pred", pred_to_json(*s.pred));
      if (s.unknown != UnknownPolicy::Exclude) {
        o.emplace_back("unknown",
                       Value::of(std::string(unknown_policy_name(s.unknown))));
      }
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
    case StageOp::Sites:
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
