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

enum class View : std::uint8_t {
  Symbol,
  Entity,
  Parameter,
  TemplateParameter,
  TemplateArgument,
  SignatureSlot,
  CallArgument,
  Edge,
  Site,
  Evidence,
  Type,
  TypeLayer,
};

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
  bool virtual_relation = false;
  View target_view = View::Symbol;
};

// All catalogued relations (18 symbol-layer + 12 entity-layer).
const std::vector<RelationDesc> &relation_catalog();
const std::array<catalog::ExtensionRelation,
                 catalog::kExtensionRelations.size()> &
extension_relation_catalog();

// Resolve `name` (bare or view-qualified) in `active` view.
// Returns nullptr when unknown.
const RelationDesc *resolve_relation(const std::string &name, View active,
                                     bool inbound = false);

// Resolve an already view-qualified relation name (e.g. "symbol.calls", as
// produced by canonical_json()/plan_to_json()) without an active-view
// context. Used by explain() to report every relation a normalized plan
// touches. Returns nullptr when unknown.
const RelationDesc *resolve_qualified_relation(const std::string &qualified);

// entity_kind name -> id (entity_kind seed values 0..9; -1 when unknown).
int64_t entity_kind_id(const std::string &name);

// ---- Predicates
// ---------------------------------------------------------------

enum class UnknownPolicy : std::uint8_t { Exclude, Include, Error };
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
  enum class Kind : std::uint8_t { Any, All, None } kind = Kind::Any;
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
  Sites,
  Union,
  Intersect,
  Except,
  Select,
  Count,
  Distinct,
  OrderBy,
  Limit,
  Path,           // bounded ordered witness path(s) to an operand node set
  Rank,           // deterministic re-rank of a Path stream (shortest-first)
  ReverseTypeUse, // typed reverse type-use witness (Type/TypeLayer -> owner)
};

const char *stage_op_name(StageOp op);

enum class TraversalMode : std::uint8_t { Static, Devirtualized };

const char *traversal_mode_name(TraversalMode mode);

struct Plan; // fwd

struct Stage {
  StageOp op = StageOp::Where;
  std::optional<Pred> pred;  // Nodes (optional) / Where
  View level = View::Symbol; // ChangeView
  std::string relation;      // Out / In / Path (normalized: qualified)
  TraversalMode mode = TraversalMode::Static; // Out / In
  int64_t min_depth = 1, max_depth = 1; // Out / In / Path / ReverseTypeUse
  std::shared_ptr<Plan> operand;   // Union / Intersect / Except / Path ("to")
  std::vector<std::string> fields; // Select / OrderBy
  int64_t n = 0; // Limit / Path ("shortest" cap) / Rank ("top_n" cap)
  UnknownPolicy unknown = UnknownPolicy::Exclude;
  bool inbound = false; // Path traversal direction
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
// Path: a stream of bounded ordered witness paths (see path()/
// reverse_type_use() below and docs/query-plan.md "Path result shape").
enum class Shape { Nodes, Rows, Scalar, Path };
// The view and shape after all stages (validated plans only).
View final_view(const Plan &plan);
Shape final_shape(const Plan &plan);

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
  Query &operator=(const Query &) = default;
  Query(Query &&) noexcept = default;
  Query &operator=(Query &&) noexcept = default;

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
Stage sites();
Stage union_(const Query &operand);
Stage intersect(const Query &operand);
Stage except_(const Query &operand);
Stage select(std::vector<std::string> fields);
Stage count();
Stage distinct();
Stage order_by(std::vector<std::string> fields);
Stage limit(int64_t n);

// Bounded deterministic shortest witness path(s) from the current node stream
// to `to`'s node stream, over one symbol/entity-view relation (docs/
// query-plan.md "Path result shape"). `shortest` caps the number of witnesses
// kept after ranking (0 = keep every minimal-depth witness up to the default
// result cap). Terminal for row-shaping stages: only rank()/count()/
// distinct()/limit() may follow.
Stage path(const Query &to, const std::string &relation, int64_t min_depth = 1,
           int64_t max_depth = 8, int64_t shortest = 0, bool inbound = false);

// Deterministic re-rank of a Path stream: shortest-first, ties broken by the
// lexicographic ascending node-id sequence. `top_n` (0 = unbounded within the
// default result cap) keeps only the first `top_n` witnesses of that order.
Stage rank(int64_t top_n = 0);

// First-class reverse type-use: from a `type`/`type_layer` node stream,
// returns one witness per owner (symbol/parameter/template_parameter/
// template_argument/call_argument/signature_slot) that uses this type,
// directly or nested inside pointer/reference/array/function/member-pointer/
// alias layers, retaining every intermediate typed layer as a `through` step.
Stage reverse_type_use(int64_t max_depth = 8);

} // namespace cidx::query
