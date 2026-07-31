# CXQ QueryPlan DSL — usage guide

How to *use* the CXQ query DSL from C++ and Python. The normative spec
(IR grammar, canonical JSON bytes, validation rules, execution semantics) is
[query-plan.md](query-plan.md) — this page is the tutorial companion.
Runnable samples: [`examples/queryplan/`](../examples/queryplan/README.md)
(C++) and [`python/examples/08–09_queryplan_*.py`](../python/examples/README.md)
(Python).

## One idea

A query is an **immutable pipeline value** built once and executed separately:

```
start(<source>) | <stage> | <stage> | ...
```

Both builders — C++ (`src/query/plan.hpp`) and Python
(`python/indexer/queryplan.py`) — produce the same **QueryPlan IR**, and
`canonical_json(plan)` emits byte-identical JSON in both languages. The plan
is data: print it, diff it, log it, hand it across a process boundary.
Execution (`src/query/exec.hpp` / `queryplan.Executor`) is read-only,
parameterized SQL over `index.db` — a plan can never mutate an index.

## Hello, callers

Who calls `cidx::query::resolve_relation`?

C++:

```cpp
#include "query/exec.hpp"
#include "query/plan.hpp"
using namespace cidx::query;

cidx::Storage db("index.db");
Executor ex(db);

Query q = start(symbol("cidx::query::resolve_relation"))
        | in_("calls")
        | select({"name", "file", "line"})
        | order_by({"name"});
Result r = ex.run(q.plan());
```

Python:

```python
from indexer.storage import Storage
from indexer.queryplan import Executor, start, symbol, in_, select, order_by

db = Storage("index.db")
ex = Executor(db)

q = (start(symbol("cidx::query::resolve_relation"))
     | in_("calls")
     | select(["name", "file", "line"])
     | order_by(["name"]))
res = ex.run(q.plan)
```

Every fragment below works identically in both languages (Python spells list
literals `[...]`, C++ spells them `{...}`; the plan accessor is `q.plan()` in
C++ and `q.plan` in Python).

## Sources — where a pipeline starts

| Source | Seeds the stream with |
|---|---|
| `symbol(ref)` | symbols matching `ref` — resolved against `usr`, then `qual_name`, then `spelling` (exact matches, all hits) |
| `entity(ref)` | same lookup, restricted to Layer-1 design entities |
| `codebase()` | nothing yet — it is lazy and **must** be enumerated with `nodes(pred?)` before anything consumes it |

## Stages — the vocabulary

Traversal and filtering (node stream in, node stream out):

- `out(relation, min, max)` / `in_(relation, min, max)` — walk edges forward /
  backward. Depth defaults to `1, 1`; `out("calls", 1, 3)` is a
  **path-length window**: a node is emitted iff *some* path of length
  `d ∈ [min, max]` reaches it. A finite `max ≤ 32` is always required.
- `where(pred)` — keep nodes matching a predicate.
- `view(level)` — retype the stream between the `symbol` and `entity` views
  (below).
- `union_(q)` / `intersect(q)` / `except_(q)` — set algebra over another
  pipeline's node stream (same view required; all three are true set
  operations over deduped id sets).
- `nodes(pred?)` — enumerate the current view's domain (only on a
  `codebase()` source).

Shaping (ends the graph part of the pipeline):

- `select(fields)` — node stream → rows with exactly these fields, in order.
  Without a `select`, a node stream still materializes with the default
  fields `id`, `usr`, `name`, `kind`.
- `order_by(fields)` — sort (fields must be in the active `select`; ties
  break by id; default order is by id).
- `distinct()` — dedup row tuples (node streams are always deduped).
- `limit(n)` — keep the first `n`.
- `count()` — terminal scalar; nothing may follow it.

Predicates compose as values:

```python
where(all_of([eq("kind", "function"),
              any_of([glob("name", "*Storage*"), eq("is_static", True)]),
              not_(glob("name", "*detail*"))]))
```

`eq`, `ne`, `glob` (SQLite GLOB syntax), and `in_list` compare one field.
Booleans use Python `True`/`False` or C++ `true`/`false` literals.

## Fields

`id`, `usr`, `name` (qualified name, else spelling), `spelling`, `qual_name`,
`kind`, `entity_type`, `is_definition`, `is_pure`, `is_static` are filterable
and selectable; `file`, `line`, `col` are **select-only**.

Two classification fields, deliberately separate:

- `kind` — always the C++ *declaration* kind (`function`, `class`, `struct`,
  `method`, ...).
- `entity_type` — always the Layer-1 *design* classification
  (`class`, `abstract_class`, `interface`, `union`, `enum`,
  `class_template`, `abstract_class_template`, `interface_template`,
  `namespace`, `other`; `null` for non-entities).

An abstract struct is `kind = struct` **and** `entity_type = abstract_class`.

## The two views

| View | Nodes | Relations (namespace) |
|---|---|---|
| `symbol` | every symbol row | the 20 Layer-0 edge kinds: `calls`, `inherits`, `contains`, `specializes`, `instantiates`, `overrides`, `uses`, `field_of`, `method_of`, `construct-*`, `factory-construct`, `destroy`, `friend`, `dispatch_calls`, `alias_of`, `of_type` |
| `entity` | Layer-1 design entities (ids are symbol ids) | the 12 entity edge kinds: `generalizes`, `implements`, `specializes`, `composes`, `aggregates`, `associates`, `creates`, `uses`, `destroys`, `befriends`, `instantiates`, `declares` |

Rules of thumb:

- A bare relation name resolves in the **active** view; `symbol.uses` /
  `entity.uses` are always accepted and disambiguate the two `uses`.
- A traversal **retargets** the stream to its relation's layer:
  `out("entity.uses")` from a symbol stream yields an entity-view stream.
- `view(entity)` **drops** ids that have no `entity_node` row (it never maps
  them); `view(symbol)` is a pure relabel.
- Convenience synonyms from the old API map directly: callers = `in_("calls")`,
  callees = `out("calls")`, bases = `out("inherits")`, subclasses =
  `in_("inherits")`.

## Results

`Executor.run()` returns a `Result`; `to_json()` (C++) / `to_dict()` (Python)
give the stable document:

```json
{ "shape": "nodes" | "rows" | "scalar",
  "view": "symbol" | "entity",
  "count": 2,
  "truncated": false,
  "rows": [ { "name": "...", "file": "...", "line": 361 } ] }
```

Determinism is part of the contract: node streams stay ordered ascending by
id after every stage, and row field order is the `select` order.

## Errors and budgets

Validation happens **before** any SQL runs and fails with a stable code —
`PlanError` whose message starts with `E_SOURCE`, `E_VIEW`, `E_RELATION`,
`E_DEPTH`, `E_FIELD`, `E_KIND`, `E_LIMIT`, `E_SETOP`, or `E_STAGE`
(see [query-plan.md](query-plan.md#validation-before-execution-error-identity-is-the-leading-e_-code)
for the full table). Typical trip-wires: an unbounded depth (`max` is
required), `order_by` on a field not in the `select`, consuming a
`codebase()` source before `nodes()`, and anything after `count()`.

Execution is budgeted: 10 000 states per traversal, 10 000 enumerated nodes,
and a default cap of 1 000 result rows (skipped only while an explicit
`limit()` is still in effect; any later cardinality-expanding stage re-arms
it). Hitting any budget sets `truncated: true` — check it whenever a number
must be exact. `count()` ignores the result cap.

## Bounded witness paths, ranking, and reverse type-use

`path()` returns bounded, deterministic shortest witness path(s) between the
current node stream and a target subquery over one symbol/entity-view
relation; a start whose search is cut off by `max_depth` while its frontier
is still expandable sets `truncated: true` (a finite-depth exhaustion is not
a proven negative). `rank()` re-applies that deterministic order
(shortest-first, ties broken over each step's full typed-step identity) and
optionally caps the count.
`reverse_type_use()` is a first-class typed relation from a `type`/
`type_layer` node stream: it returns one witness per owner using that type,
directly or nested inside pointer/reference/array/member-pointer/alias
layers, retaining every intermediate typed layer as an ordered `through`
step — see
[query-plan.md#path-result-shape-path--reverse_type_use](query-plan.md#path-result-shape-path--reverse_type_use)
for the exact witness shape, budgets, and the `call_argument` scope trim.
`explain()` additionally reports `execution_shape`, the execution `budgets`,
and every `input_relations` entry with its catalogued completeness.

## Not in v1

The textual CXQ language (a hand-written `parse_cxq()` exists for the stages
it already covers, but is not yet extended for `path()`/`rank()`/
`reverse_type_use()`), semantic predicate macros beyond the ones already
exposed as builder helpers, and a `cidx query` CLI are deferred — see the
[compatibility section](query-plan.md#compatibility). The existing
`GraphQuery`/`EntityQuery` APIs remain fully supported as compatibility
adapters. Declarative `EntityQuery` seeds, relation steps, and QueryPlan
predicates lower to the same canonical CXQ plan executed by the shared
read-only executor; callback filters, case-insensitive name matching, and edge
evidence retain their legacy compatibility behavior for this release window.
