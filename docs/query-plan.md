# CXQ QueryPlan contract (v1)

Status: implemented (v1 slice) — C++ `src/query/`, Python `python/indexer/queryplan.py`.
Design source: wiki `pages/planning/cidx-query-language` (Lang-2 unified declarative query API).
Usage guide with runnable samples: [query-dsl.md](query-dsl.md), `examples/queryplan/`.

The stable product is the normalized **QueryPlan IR** plus its relation catalog and
result shapes. The C++ builder, the Python builder, and (later) the textual CXQ
parser all produce the same IR; execution is a read-only SQLite compiler over
`index.db`. Both languages must emit **byte-identical canonical JSON** for the
same plan and semantically identical, deterministic results.

## Views

| View | Node domain | Relation namespace |
|---|---|---|
| `symbol` | `symbol` rows | `edge` kinds (`symbol.calls`, `symbol.uses`, ...) |
| `entity` | `entity_node` rows (ids are symbol ids) | `entity_edge` kinds (`entity.uses`, ...) |
| `parameter` | ordered callable parameter slots | virtual relations compiled to `parameter` |
| `template_parameter` | ordered template parameter slots | virtual relations compiled to `template_param` |
| `template_argument` | ordered template arguments and pack elements | virtual relations compiled to `template_arg` |
| `call_argument` | arguments for one call occurrence | virtual relations compiled to `call_arg` |
| `edge` | physical edge facts with logical identity | virtual relations compiled to `edge`/`edge_site` |
| `site` | deterministic source-location evidence sites | virtual relations compiled to `edge_site` |
| `evidence` | bounded source occurrence facts | virtual relations compiled to `edge_site` |
| `type` | normalized type nodes | virtual relations compiled to `type_node`/`type_edge` |

Views are typed: an id may appear in an entity-view stream only when it has an
`entity_node` row. `view(entity)` therefore **drops** ids without one (it never
maps them — moving from a method to its record still traverses `method_of`);
`view(symbol)` is a pure relabel (every entity id is a symbol id). A traversal
retargets the stream view to its relation's target domain. From a `codebase()`
source, `nodes(...)` enumerates the current view's domain. Every typed row has
a stable logical identity derived from its natural key; physical SQLite row ids
are implementation details. Pack-bearing views retain both the outer
`position` and the element `pack_index`.

## Relation catalog

Relations are data, not methods. The catalog contains the 18 Layer-0
`edge_kind` names, the 12 `entity_edge_kind` names, and typed virtual
relations such as `has_parameter`, `has_template_argument`, `has_argument`,
`of_type`, `has_evidence`, and `has_site`. Virtual relations compile to their dedicated
physical tables; they never duplicate rows into a generic edge table. A bare
relation resolves at the active endpoint; qualified forms are accepted for all
logical views.
Normalization stores the qualified name in canonical JSON. `callers()` is
`in(calls)`; `callees()` is `out(calls)`; `bases()` is `out(inherits)`;
`subclasses()` is `in(inherits)`.

## Plan IR

```
Plan   := { source: Source, stages: [Stage...] }
Source := codebase() | symbol(ref) | entity(ref)
Stage  := nodes(pred?) | view(level) | where(pred)
        | out(relation, depth=a..b, mode=static|devirtualized)
        | in(relation, depth=a..b)
        | sites()
        | union(plan) | intersect(plan) | except(plan)
        | select(fields) | count() | distinct() | order_by(fields) | limit(n)
        | path(to=plan, relation, depth=a..b, shortest=n, direction=out|in)
        | rank(top_n=n)
        | reverse_type_use(max_depth=n)
Pred   := all_of([p...]) | any_of([p...]) | not(p)
        | cmp(field, op, value)  op ∈ {eq, ne, glob, in}
        | exists(relation, pred?) | none(relation, pred?)
        | all(relation, pred?) | at_least(n, relation, pred?)
        | exactly(n, relation, pred?)
```

Fields: `id`, `usr`, `name` (COALESCE(qual_name, spelling)), `spelling`,
`qual_name`, `kind`, `entity_type`, `file`, `line`, `col`, `is_definition`,
`is_pure`, `is_static`, `semantic_universe`, `identity_key`.
`file`/`line`/`col` are select-only; the rest are also
filterable. Declaration kind and entity classification are SEPARATE fields in
every view: `kind` is always the C++ declaration kind (symbol-kind names, so
`kind in [class, struct]` keeps its current-API meaning) and `entity_type` is
always the Layer-1 classification (`class`, `abstract_class`, `interface`,
`union`, `enum`, `class_template`, `abstract_class_template`,
`interface_template`, `namespace`, `other`; `null` for non-entity ids — an
abstract struct is `kind = struct` AND `entity_type = abstract_class`).

`symbol(ref)` / `entity(ref)` resolve `ref` against `usr`, then `qual_name`,
then `spelling` (exact matches, all hits, ordered by id); `entity(ref)`
restricts hits to `entity_node` ids.

## Canonical JSON

`canonical_json(plan)` = normalized plan serialized exactly like CPython
`json.dumps(obj, indent=2)` (the existing `json_out` contract). Top-level key
order: `cxq` (format version, `1`), `source`, `stages`. Stage key order:
`op` first, then that op's fields in the documented order (`relation`,
optional non-static `mode`, `min_depth`, `max_depth` for traversals; `pred` for
filters; `fields`,
`n`, `level`, `plan` as applicable). Predicate key order: `op`, then
`preds` / `pred` / (`field`, `values`|`value`).

Normalization: relation names become layer-qualified; nested `all_of` within
`all_of` (and `any_of` within `any_of`) are flattened; `not(not(p))` reduces to
`p`. Nothing else is rewritten — user ordering is preserved.

## Validation (before execution; error identity is the leading `E_*` code)

- `E_SOURCE` empty source ref
- `E_VIEW` unknown view level or invalid typed view transition; `sites()` requires
  an edge node stream
- `E_RELATION` unknown relation in the active view's namespace
- `E_DEPTH` closure bounds: `1 <= min <= max <= 32`; a finite max is ALWAYS
  required (`1..*` is rejected — resolves wiki open question 2 conservatively)
- `E_FIELD` unknown field, filter on a select-only field, or `order_by` on a
  field not in the active `select`
- `E_KIND` unknown `kind` (symbol-kind) or `entity_type` (entity-kind) name
- `E_LIMIT` limit < 1
- `E_SETOP` operand plan does not yield a node stream in the same view
- `E_STAGE` stage illegal for the current stream shape (nodes → rows → scalar;
  nodes → path via `path()`/`reverse_type_use()`; e.g. `out` after `select`,
  anything after `count`, `nodes` on a non-codebase source, `rank()`/
  `order_by()` outside their required shape, or any consuming stage —
  including plan end — on a `codebase()` stream not yet enumerated by
  `nodes()`)

Semantic helpers are builder-only macros. Trait helpers lower to field
comparisons; ancestry, member, template, call, and use helpers lower to the
relationship quantifiers above; and `any_target`, `all_targets`, and
`no_targets` lower to boolean combinations of `exists`/`none`. The normalized
plan contains only these QueryPlan primitives, so `explain()` exposes the
expanded quantifier tree.

## Execution semantics

- Read-only, parameterized SQL only; no arbitrary SQL, no mutation.
- The node stream is kept **ordered ascending by id, deduped** after every
  stage.
- `out`/`in`: level-by-level frontier expansion (`SELECT DISTINCT ... WHERE
  src_id IN (chunk) AND kind = ?`, chunk size 400) with **path-length-window
  semantics**: a node is emitted iff SOME path of length d ∈ [min, max]
  reaches it — not only its shortest first-discovery depth (in a diamond
  `A→B`, `A→C→B`, `out(r, 2..2)` emits `B`). There is no cross-level visited
  set; termination comes from the finite max depth (≤ 32) and the state
  budget. Self (depth 0) is never emitted, but a start node reached again
  through a cycle of length ≥ 1 is.
- `out(symbol.calls, mode=devirtualized)` preserves receiver types across
  inherited method bodies and narrows virtual calls when the receiver is
  exact. Unknown receivers retain every possible dispatch target. This mode
  is rejected for inbound and non-`symbol.calls` traversals.
- `union`, `intersect`, and `except` are all SET operations over deduped id
  sets — `union` never double-counts an id reached by both operands.
- `distinct()` dedups row tuples after `select` (node streams are already
  deduped).
- `order_by` sorts rows by the named fields (nulls last, ties by id); default
  ordering is by id.
- Budgets: per-traversal state budget 10 000 (cumulative level sizes);
  enumeration (`codebase` + `nodes`) budget 10 000; default result cap 1 000.
  The cap is skipped only when a `limit` stage is still in effect at the end
  of the plan — any later cardinality-expanding stage (`nodes`, `out`, `in`,
  `union`) re-arms it, so an early `limit` can never disable the final safety
  cap. Hitting any budget sets `truncated: true` in the result — never
  silently treated as complete.
- `count()` counts the full (budget-bounded) stream; the default result cap
  does not apply to it.
- Relationship quantifiers use Kleene true/false/unknown evaluation. A
  relation catalogued as `partial` or `unknown` yields unknown when the
  requested witness is absent; it never yields a proven negative. `where()`
  and `nodes()` default to `unknown=exclude`; `unknown=include` retains true or
  unknown rows, and `unknown=error` raises `E_UNKNOWN` on an unknown result.
  `all`/`none`/`at_least`/`exactly` preserve the same rule, including vacuous
  truth only for complete relations.

## Result shape

```
{ "shape": "nodes" | "rows" | "scalar" | "path",
  "view": "symbol" | "entity" | "parameter" | "template_parameter" |
           "template_argument" | "call_argument" | "edge" | "site" | "evidence" | "type",
  "count": <int>,          // scalar value for shape=scalar; witness count for shape=path
  "truncated": <bool>,
  "index": {
    "schema_version": <int>,
    "source_revision": <string|null>,
    "source_fingerprint": <string|null>,
    "index_config": <string|null>,
    "index_config_fingerprint": <string|null>,
    "freshness": "current" | "stale" | "unverifiable"
  },
  "rows": [ {field: value, ...} ... ],    // absent for shape=scalar/path
  "paths": [ Witness... ] }               // present only for shape=path
```

## Path result shape (`path()` / `reverse_type_use()`)

Both stages are terminal for row-shaping: only `rank()`, `count()`,
`distinct()`, and `limit()` may follow; `select()`/`order_by()`/further
traversal fail `E_STAGE`. Each result witness:

```
Witness := { "length": <int>, "status": "complete" | "partial",
             "steps": [ Step... ] }
Step    := { "id": <int>, "domain": "symbol" | "entity" | "type" |
                       "parameter" | "template_parameter" |
                       "template_argument",
             "through": <string>,   // relation/type_edge_kind label into this
                                    // node; "" for the start step
             "direction"?: "in",    // present only for an inbound hop
             "status": "complete" | "partial",
             "sites": [ {file_id, line, col, conditional}... ] }
```

- **`path(to, relation, min_depth=1, max_depth=8, shortest=0, inbound=false)`**
  requires a `symbol`/`entity` node stream and a non-typed same-view
  `relation`; `to` is a subquery yielding the target node set (`E_SETOP` on
  view mismatch, same rule as `union`/`intersect`/`except`). It runs a
  multi-source BFS that tracks every predecessor reaching each node at each
  depth (a shortest-path DAG), so every minimal-depth witness — not only
  one — is reconstructed once the target set is first reached at a depth in
  `[min_depth, max_depth]`. A start node with no reachable target in that
  window contributes no witness (not an error). "Shortest" is per-start, not
  per-(start, target): the search stops at the first depth in the window at
  which *any* target is reached and reconstructs every minimal-depth witness
  from that one start at that depth, even when a different (start, target)
  pair would have a shorter path at another depth — this is an intentional,
  documented contract, not a bug. `shortest` caps the number of witnesses
  kept after the default ranking (`0` = keep every minimal-depth witness up
  to the result cap). Each hop's `status` is the relation's catalogued
  completeness; `sites()`-equivalent per-hop evidence is included directly on
  `through`-bearing steps. A start's search stopping at `max_depth` is a
  proven negative only when its frontier is also exhausted there (no further
  outgoing edges to expand); when the frontier at the depth limit is still
  expandable, "no witness from this start" is unknown, not proven, and sets
  `truncated: true` — this does not abort the search for other starts. A
  witness reconstruction cut short by the chain/witness cap is dropped
  entirely rather than serialized as a shorter, incomplete chain that does
  not start at the real source.
- **`reverse_type_use(max_depth=8)`** requires a `type`/`type_layer` node
  stream. From each seed type (or nested type-layer), it climbs `type_edge`
  (structural nesting: `pointee`/`element_type`/`return_type`/`param_type`/
  `template_argument_type`/`member_owner`/`member_component`) and
  `type_node.canonical_id` (cv/sugar desugaring) backward, up to
  `max_depth`, emitting one witness per owner found at every depth — direct
  use at depth 0 and every nested layer above it. Owners span the `symbol`
  (`symbol_type` returns/of_type/underlying_type), `parameter`,
  `template_parameter`, and `template_argument` domains; the final step's
  `domain`/`through` name that owner-fact table. This is a first-class typed
  relation executed directly (no manual per-symbol enumeration): every
  intermediate `type` step and its `through` label is retained, unlike the
  legacy flat `GraphQuery.type_users()` closure.
- Determinism: witnesses are ordered `length` ascending, ties broken
  lexicographically over each step's full logical typed-step identity —
  `(id, domain, through, position, pack_index)`, in that order — a total
  order even when two witnesses share the same node-id sequence but differ
  only by which relation/`type_edge` hop reached a node (e.g. `member_owner`
  vs `member_component` landing on the same node at the same position) or by
  which typed-view slot (e.g. parameter position) the final owner step
  names. `rank(top_n=0)` re-applies this order (a no-op unless the stream
  was mutated) and, when `top_n > 0`, keeps only the first `top_n`
  witnesses — the single documented stable tie-break for `rank()`/
  `shortest`.
- Budgets: the witness search shares the `path_node_budget` (10 000
  cumulative node/type expansions) independent of the traversal/enumerate
  budgets; the witness count itself is capped by the default result cap.
  Either budget sets `truncated: true`, never a silently complete result.

`source_fingerprint` is a SHA-1 digest of a deterministic, ordered manifest of
indexed file identities, current content MD5s, and indexed flags. The
`source_revision` is the content-addressed `content-sha1:<digest>` form. The
configuration digest covers each file's stored compile options and driver.
After a successful `index` pass the C++ and Python CLIs stamp these metadata
rows. Legacy databases, missing files, or unreadable files remain queryable but
report `unverifiable`; a changed source/configuration reports `stale`.

`Executor.explain(plan)` (C++ and Python) returns, without executing any
row-producing stage:

```
{ "plan": <canonical plan JSON>,
  "index": <same "index" object as Result::to_json()>,
  "execution_shape": "nodes" | "rows" | "scalar" | "path",
  "budgets": {
    "traverse_node_budget": 10000, "enumerate_budget": 10000,
    "path_node_budget": 10000, "default_result_cap": 1000
  },
  "input_relations": [ {"relation": <qualified name>,
                        "completeness": "complete" | "partial"} ... ],
  "partial_inputs": <bool>,          // any input_relations entry is partial
  "unknown_capable_inputs": <bool> } // any input_relations entry isn't complete
```

`input_relations` collects every relation the normalized plan touches:
`out`/`in`/`path` stages, and every relationship quantifier
(`exists`/`none`/`all`/`at_least`/`exactly`) inside `where`/`nodes`
predicates, including one level into `union`/`intersect`/`except`/`path`
`to=` operand plans. This surfaces partial or unknown-capable evidence
sources declaratively, before any query runs.

Node streams without `select` emit the default fields `id`, `usr`,
`semantic_universe`, `identity_key`, `name`, `kind`. The scope fields keep a
bare-USR result portable and unambiguous across universes and database
generations. Row objects preserve `select` field order.

## Compatibility

This is a read/query-layer addition: no schema bump. Existing `GraphQuery` /
`EntityQuery` surfaces remain compatibility adapters over the same physical
tables, while CXQ exposes canonical logical slot and evidence identities.
Evidence and site expansion are explicit and budgeted; truncated results remain
marked `truncated: true`. `path()`, `rank()`, `reverse_type_use()`, and the
`explain()` budgets/execution-shape/input-relations extension (CXQ-004) ship in
this slice; `reverse_type_use()`'s owner domains are `symbol`/`parameter`/
`template_parameter`/`template_argument` (call-site `call_argument` reverse-use
is not covered — a disclosed scope trim, since it is evidence/site-scoped
rather than declaration-level type use). Deferred to a later slice: the
`cidx query` agent tool / textual-CXQ front-end for `path()`/`rank()`/
`reverse_type_use()` (the hand-written `parse_cxq()` parser used by existing
tests is not yet extended for these three stages).
