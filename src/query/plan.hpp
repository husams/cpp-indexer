// query/plan.hpp -- CXQ QueryPlan IR, relation catalog, and pipeline builder.
//
// Contract: docs/query-plan.md (v1). The IR is the stable product: the C++
// builder here, the Python builder (indexer/queryplan.py), and the future CXQ
// text parser all produce this tree, and both languages must emit
// byte-identical canonical JSON (json_out / json.dumps(indent=2)) for the
// same plan. Plans are immutable values: every builder stage copies.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "catalogs/generated_extensions.hpp"
#include "cli/json_out.hpp"

namespace cidx::query {

// ---- Errors -----------------------------------------------------------------
// Validation failure. The message starts with the stable "E_*: " code from
// docs/query-plan.md; the code (not the prose) is the compatibility surface.
class PlanError : public std::runtime_error {
public:
  explicit PlanError(const std::string &msg) : std::runtime_error(msg) {}
};

// ---- Views ------------------------------------------------------------------

enum class View { Symbol, Entity };

const char *view_name(View v);

// ---- Relation catalog
// --------------------------------------------------------- Relations are data,
// not methods: one descriptor per edge_kind / entity_edge_kind name. `layer` is
// the view whose namespace owns the name.

struct RelationDesc {
  std::string name; // bare name ("calls", "uses", ...)
  View layer = View::Symbol;
  int64_t kind_id = 0; // edge_kind.id or entity_edge_kind.id
  std::string source;
  std::string target;
  std::string inverse;
  std::string traversal;
  std::string evidence;
  std::string evidence_capabilities;
  std::string completeness;
};

// All catalogued relations (18 symbol-layer + 12 entity-layer).
const std::vector<RelationDesc> &relation_catalog();
const std::array<catalog::ExtensionRelation,
                 catalog::kExtensionRelations.size()> &
extension_relation_catalog();

// Resolve `name` (bare or "symbol."/"entity."-qualified) in `active` view.
// Returns nullptr when unknown.
const RelationDesc *resolve_relation(const std::string &name, View active);

// entity_kind name -> id (entity_kind seed values 0..9; -1 when unknown).
int64_t entity_kind_id(const std::string &name);

// ---- Predicates
// ---------------------------------------------------------------

enum class UnknownPolicy { Exclude, Include, Error };
const char *unknown_policy_name(UnknownPolicy policy);

enum class PredOp {
  AllOf,
  AnyOf,
  Not,
  Eq,
  Ne,
  Glob,
  In,
  Exists,
  None,
  All,
  AtLeast,
  Exactly,
};

// One predicate-tree node. Cmp ops use field + value/values; boolean ops use
// kids. Values are strings ("true"/"false" for booleans is NOT used -- boolean
// fields compare against int 0/1 via `int_value`).
struct Pred {
  PredOp op = PredOp::Eq;
  // AllOf / AnyOf / Not
  std::vector<Pred> kids;
  // Eq / Ne / Glob / In
  std::string field;
  std::vector<std::string> str_values; // Eq/Ne/Glob use [0]; In uses all
  std::optional<int64_t> int_value;    // Eq/Ne on boolean/int fields
  // Relationship quantifiers. The target predicate is evaluated against each
  // reached node; a missing target means "any target".
  std::string relation;
  std::shared_ptr<Pred> target;
  int64_t min_depth = 1;
  int64_t max_depth = 1;
  int64_t threshold = 0;
  bool inbound = false;
};

struct TargetSet {
  enum class Kind { Any, All, None } kind = Kind::Any;
  std::vector<std::string> refs;
};

// Builders (portable programmatic boolean form -- and/or/not can't overload).
Pred all_of(std::vector<Pred> preds);
Pred any_of(std::vector<Pred> preds);
Pred not_(Pred inner);
Pred eq(const std::string &field, const std::string &value);
// A string literal would otherwise prefer the bool overload (pointer->bool is
// a standard conversion; const char* -> std::string is user-defined).
Pred eq(const std::string &field, const char *value);
Pred eq(const std::string &field, int64_t value);
Pred eq(const std::string &field, bool value);
Pred ne(const std::string &field, const std::string &value);
Pred glob(const std::string &field, const std::string &pattern);
Pred in_list(const std::string &field, std::vector<std::string> values);

// Relationship quantifiers. These are QueryPlan primitives; the semantic
// helpers below lower to these nodes and ordinary field predicates.
Pred exists(const std::string &relation, std::optional<Pred> target = {},
            int64_t min_depth = 1, int64_t max_depth = 1, bool inbound = false);
Pred none(const std::string &relation, std::optional<Pred> target = {},
          int64_t min_depth = 1, int64_t max_depth = 1, bool inbound = false);
Pred all(const std::string &relation, std::optional<Pred> target = {},
         int64_t min_depth = 1, int64_t max_depth = 1, bool inbound = false);
Pred at_least(int64_t threshold, const std::string &relation,
              std::optional<Pred> target = {}, int64_t min_depth = 1,
              int64_t max_depth = 1, bool inbound = false);
Pred exactly(int64_t threshold, const std::string &relation,
             std::optional<Pred> target = {}, int64_t min_depth = 1,
             int64_t max_depth = 1, bool inbound = false);

TargetSet any_target(std::vector<std::string> refs);
TargetSet all_targets(std::vector<std::string> refs);
TargetSet no_targets(std::vector<std::string> refs);

Pred inherits_from(const std::string &target, bool transitive = false);
Pred inherits_from(const TargetSet &targets, bool transitive = false);
Pred implements(const std::string &target);
Pred implements(const TargetSet &targets);
Pred has_ancestor(const std::string &target, bool transitive = true);
Pred has_member(std::optional<Pred> target = {});
Pred has_method(std::optional<Pred> target = {});
Pred has_field(std::optional<Pred> target = {});
Pred has_nested(std::optional<Pred> target = {});
Pred has_template_arg(std::optional<Pred> target = {});
Pred is_specialization_of(const std::string &target);
Pred is_instantiation_of(const std::string &target);
Pred calls(std::optional<Pred> target = {});
Pred called_by(std::optional<Pred> target = {});
Pred uses(std::optional<Pred> target = {});
Pred used_by(std::optional<Pred> target = {});

Pred is_abstract();
Pred is_interface();
Pred is_pure();
Pred is_static();
Pred is_template();
Pred is_instance();

// ---- Stages
// -------------------------------------------------------------------

enum class StageOp {
  Nodes, // enumerate current view's domain (codebase source only)
  ChangeView,
  Where,
  Out,
  In,
  Union,
  Intersect,
  Except,
  Select,
  Count,
  Distinct,
  OrderBy,
  Limit,
};

const char *stage_op_name(StageOp op);

enum class TraversalMode : std::uint8_t { Static, Devirtualized };

const char *traversal_mode_name(TraversalMode mode);

struct Plan; // fwd

struct Stage {
  StageOp op = StageOp::Where;
  std::optional<Pred> pred;  // Nodes (optional) / Where
  View level = View::Symbol; // ChangeView
  std::string relation;      // Out / In (normalized: qualified)
  TraversalMode mode = TraversalMode::Static; // Out / In
  int64_t min_depth = 1, max_depth = 1;       // Out / In
  std::shared_ptr<Plan> operand;              // Union / Intersect / Except
  std::vector<std::string> fields;            // Select / OrderBy
  int64_t n = 0;                              // Limit
  UnknownPolicy unknown = UnknownPolicy::Exclude;
};

// ---- Source / Plan
// --------------------------------------------------------------

enum class SourceKind { Codebase, Symbol, Entity };

struct Source {
  SourceKind kind = SourceKind::Codebase;
  std::string ref; // symbol()/entity() lookup key; empty for codebase
};

struct Plan {
  Source source;
  std::vector<Stage> stages;
};

// ---- Validation / normalization / canonical JSON
// -------------------------------- validate() type-checks the plan
// (docs/query-plan.md "Validation") and returns the normalized copy: relation
// names layer-qualified, nested AllOf/AnyOf flattened, not(not(p)) reduced.
// Throws PlanError.
Plan validate(const Plan &plan);

// Normalized-plan canonical JSON (validates first). Byte-identical to the
// Python side's queryplan.canonical_json().
std::string canonical_json(const Plan &plan);
json_out::Value plan_to_json(const Plan &plan);

// Stream-shape inference used by validate(); exposed for the executor.
enum class Shape { Nodes, Rows, Scalar };
// The view and shape after all stages (validated plans only).
View final_view(const Plan &plan);

// ---- Pipeline builder
// ------------------------------------------------------------ auto q =
// start(symbol("ns::f")) | out("calls") | where(eq("kind","function"))
//        | select({"name","usr"}) | limit(100);
// Query is an immutable value: operator| copies. plan() hands back the IR.

Source codebase();
Source symbol(const std::string &ref);
Source entity(const std::string &ref);

class Query {
public:
  explicit Query(Source src) { plan_.source = std::move(src); }
  Query(const Query &) = default;

  [[nodiscard]] const Plan &plan() const { return plan_; }

  friend Query operator|(Query q, Stage s) {
    q.plan_.stages.push_back(std::move(s));
    return q;
  }

private:
  Plan plan_;
};

inline Query start(Source src) { return Query(std::move(src)); }

// Stage factories (names mirror the CXQ surface; `in_` because Python's `in`
// is reserved and the two builders keep one vocabulary).
Stage nodes();
Stage nodes(Pred pred, UnknownPolicy unknown = UnknownPolicy::Exclude);
Stage view(View level);
Stage where(Pred pred, UnknownPolicy unknown = UnknownPolicy::Exclude);
Stage out(const std::string &relation, int64_t min_depth = 1,
          int64_t max_depth = 1, TraversalMode mode = TraversalMode::Static);
Stage in_(const std::string &relation, int64_t min_depth = 1,
          int64_t max_depth = 1);
Stage union_(const Query &operand);
Stage intersect(const Query &operand);
Stage except_(const Query &operand);
Stage select(std::vector<std::string> fields);
Stage count();
Stage distinct();
Stage order_by(std::vector<std::string> fields);
Stage limit(int64_t n);

} // namespace cidx::query
