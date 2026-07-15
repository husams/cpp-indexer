# CXQ QueryPlan contract (v1)

Status: implemented (v1 slice) — C++ `src/query/`, Python `python/indexer/queryplan.py`.
Design source: wiki `pages/planning/cidx-query-language` (Lang-2 unified declarative query API).

The stable product is the normalized **QueryPlan IR** plus its relation catalog and
result shapes. The C++ builder, the Python builder, and (later) the textual CXQ
parser all produce the same IR; execution is a read-only SQLite compiler over
`index.db`. Both languages must emit **byte-identical canonical JSON** for the
same plan and semantically identical, deterministic results.

## Views (v1)

| View | Node domain | Relation namespace |
|---|---|---|
| `symbol` | `symbol` rows | `edge` kinds (`symbol.calls`, `symbol.uses`, ...) |
| `entity` | `entity_node` rows (ids are symbol ids) | `entity_edge` kinds (`entity.uses`, ...) |

`view(entity)` / `view(symbol)` mid-pipeline changes only the relation namespace
(ids are shared — `entity_node.id` references `symbol.id`); it never filters or
maps the current node set. From a `codebase()` source, `nodes(...)` enumerates
the **current view's** domain. Reserved for later slices: `codebase`, `edge`,
`site`, `template_parameter`, `template_argument`, `type`, `concept`, `path`.

## Relation catalog

Relations are data, not methods. v1 catalog = the 18 Layer-0 `edge_kind` names
under the `symbol` layer and the 12 `entity_edge_kind` names under the `entity`
layer. A bare relation name resolves in the current view's namespace; the
qualified forms `symbol.<name>` / `entity.<name>` are always accepted.
Normalization stores the qualified name in canonical JSON. `callers()` is
`in(calls)`; `callees()` is `out(calls)`; `bases()` is `out(inherits)`;
`subclasses()` is `in(inherits)`.

## Plan IR

```
Plan   := { source: Source, stages: [Stage...] }
Source := codebase() | symbol(ref) | entity(ref)
Stage  := nodes(pred?) | view(level) | where(pred)
        | out(relation, depth=a..b) | in(relation, depth=a..b)
        | union(plan) | intersect(plan) | except(plan)
        | select(fields) | count() | distinct() | order_by(fields) | limit(n)
Pred   := all_of([p...]) | any_of([p...]) | not(p)
        | cmp(field, op, value)  op ∈ {eq, ne, glob, in}
```

Fields (v1): `id`, `usr`, `name` (COALESCE(qual_name, spelling)), `spelling`,
`qual_name`, `kind`, `file`, `line`, `col`, `is_definition`, `is_pure`,
`is_static`. `file`/`line`/`col` are select-only; the rest are also filterable.
`kind` values are symbol-kind names in the symbol view and `entity_kind` names
(`class`, `abstract_class`, `interface`, `union`, `enum`, `class_template`,
`abstract_class_template`, `interface_template`, `namespace`, `other`) in the
entity view.

`symbol(ref)` / `entity(ref)` resolve `ref` against `usr`, then `qual_name`,
then `spelling` (exact matches, all hits, ordered by id); `entity(ref)`
restricts hits to `entity_node` ids.

## Canonical JSON

`canonical_json(plan)` = normalized plan serialized exactly like CPython
`json.dumps(obj, indent=2)` (the existing `json_out` contract). Top-level key
order: `cxq` (format version, `1`), `source`, `stages`. Stage key order:
`op` first, then that op's fields in the documented order (`relation`,
`min_depth`, `max_depth` for traversals; `pred` for filters; `fields`,
`n`, `level`, `plan` as applicable). Predicate key order: `op`, then
`preds` / `pred` / (`field`, `values`|`value`).

Normalization: relation names become layer-qualified; nested `all_of` within
`all_of` (and `any_of` within `any_of`) are flattened; `not(not(p))` reduces to
`p`. Nothing else is rewritten — user ordering is preserved.

## Validation (before execution; error identity is the leading `E_*` code)

- `E_SOURCE` empty source ref
- `E_VIEW` unknown view level (v1: `symbol`, `entity`)
- `E_RELATION` unknown relation in the active view's namespace
- `E_DEPTH` closure bounds: `1 <= min <= max <= 32`; a finite max is ALWAYS
  required (`1..*` is rejected — resolves wiki open question 2 conservatively)
- `E_FIELD` unknown field, filter on a select-only field, or `order_by` on a
  field not in the active `select`
- `E_KIND` unknown kind name for the active view
- `E_LIMIT` limit < 1
- `E_SETOP` operand plan does not yield a node stream in the same view
- `E_STAGE` stage illegal for the current stream shape (nodes → rows → scalar;
  e.g. `out` after `select`, anything after `count`, `nodes` on a non-codebase
  source, or any consuming stage — including plan end — on a `codebase()`
  stream not yet enumerated by `nodes()`)

## Execution semantics

- Read-only, parameterized SQL only; no arbitrary SQL, no mutation.
- The node stream is kept **ordered ascending by id** after every stage.
- `out`/`in`: level-by-level frontier expansion (`SELECT DISTINCT ... WHERE
  src_id IN (chunk) AND kind = ?`, chunk size 400), visited-set semantics; a
  node is emitted iff its first-discovery depth ∈ [min, max]. Self (depth 0)
  is never emitted.
- `union` preserves multiplicity (sorted merge); `intersect`/`except` are set
  semantics (deduped).
- `distinct()` dedups (ids, or full row tuples after `select`).
- `order_by` sorts rows by the named fields (nulls last, ties by id); default
  ordering is by id.
- Budgets: per-traversal node budget 10 000; enumeration (`codebase` +
  `nodes`) budget 10 000; default result cap 1 000 when no `limit` stage is
  present. Hitting any budget sets `truncated: true` in the result — never
  silently treated as complete.
- `count()` counts the full (budget-bounded) stream; the default result cap
  does not apply to it.

## Result shape

```
{ "shape": "nodes" | "rows" | "scalar",
  "view": "symbol" | "entity",
  "count": <int>,          // scalar value for shape=scalar
  "truncated": <bool>,
  "rows": [ {field: value, ...} ... ] }   // absent for shape=scalar
```

Node streams without `select` emit the default fields `id`, `usr`, `name`,
`kind`. Row objects preserve `select` field order.

## Compatibility

This is a read/query-layer addition: no schema bump, no reindex, and the
existing `GraphQuery` / `EntityQuery` surfaces are untouched (adapter rewrites
are migration step 4, a later slice). Deferred to later slices: CXQ text
parser, semantic predicate macros (`inherits_from`, `has_method`, ...),
quantifiers, three-valued `unknown` handling, `sites()`, `path()`, `rank()`,
edge/site/type/template views, `cidx query` CLI, agent tool surface.
