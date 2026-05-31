# v7 schema coverage example

A single C++ translation unit that exercises every **new v7 node kind** and **edge
relation**, the **real graph it emits**, and a **query per relation**.

- **Fixture:** [`examples/v7_schema_coverage.cpp`](../../examples/v7_schema_coverage.cpp)
- **Output below is real**, captured by running the fixture through the actual
  visitor (`visit_tu` in `src/visit/shallow.rs`) under Apple Clang 17 / macOS arm64,
  parsed as C++20. Totals: **46 nodes, 84 edges, 16 node kinds, 21 edge kinds.**

## Reproduce

The graph was produced by feeding the fixture to the emission path the same way the
test harness does (`tests/visit/v7_schema_emission.rs::visit_snippet`):

```rust
let src = include_str!("../../examples/v7_schema_coverage.cpp");
let (nodes, edges) = visit_snippet(src, "c++20").await;   // -> real NodeRecord/EdgeRecord
```

Or index it through the full pipeline:

```bash
LIBCLANG_PATH=… DYLD_LIBRARY_PATH=… \
  cxg-index --repo . --output /tmp/cxg   # then query_graphdb / Cypher against the sink
```

## New v7 node kinds — coverage

| Node kind   | Emitted | Real example from the run |
|-------------|---------|---------------------------|
| `Type`      | ✅ (12) | `double`, `Length *`, `T &`, `const Circle &`, `T[N]`, `void`, … |
| `Parameter` | ✅ (6)  | value params `i [int]`, `r [Length]`, `c [const Circle &]`, `factor [Length]`; template params `T [type]`, `N [non_type]` |
| `Enumerator`| ✅ (3)  | `Red [enum_value=0]`, `Green [enum_value=5]`, `Blue [enum_value=6]` |
| `Concept`   | ❌ stub | `concept Number` produces **no** `Concept` node — see caveats |

(Plus pre-existing kinds: `Class`, `Method`, `Function`, `Field`, `Enum`,
`TemplateDef`, `Specialization`, `Typedef`, `Namespace`, `GlobalVariable`, `Module`.)

## New v7 edge relations — coverage

| Edge            | Emitted | Real example from the run |
|-----------------|---------|---------------------------|
| `RETURNS`       | ✅ (7)  | `Shape::area → double`, `Vec::at → T &`, `run → void` |
| `HAS_PARAM`     | ✅ (4)  | `scale → param:…:0 [idx=0]`, `scale → param:…:1 [idx=1]` (ordered) |
| `OF_TYPE`       | ✅ (8)  | `param scale:0 → const Circle &`, `field radius_ → Length` |
| `POINTS_TO`     | ✅ (2)  | `Length * → Length`, `T * → T` |
| `REFERS_TO`     | ✅ (2)  | `T & → T`, `const Circle & → const Circle` |
| `ALIAS_OF`      | ✅ (2)  | `Length → double`, `LengthPtr → Length *` |
| `TEMPLATE_PARAM`| ✅ (2)  | `Vec → tparam:…:0 [idx=0]`, `…:1 [idx=1]` (ordered) |
| `TEMPLATE_ARG`  | ✅ (2)  | `Vec<T,0> → targ:…:0/:1 [idx]` (from the partial specialization) |
| `CONSTRAINED_BY`| ❌ stub | concept constraint not modelled — see caveats |
| `USES_NAMESPACE`| ✅ (1)  | `app → geo` (`using namespace geo;`) |
| `USES_DECLARATION`| ✅ (1)| `app → geo::scale` (`using geo::scale;`) |
| `ENUMERATOR_OF` | ✅ (3)  | `Color::Red/Green/Blue → Color` |
| `UNDERLYING_TYPE`| ✅ (1) | `Color → int` (see caveat re: `uint8_t`) |

(Plus pre-existing: `INHERITS` `[access=public virtual=false]`, `OVERRIDES`
(`Circle::area → Shape::area`), `INSTANTIATES`, `SPECIALIZES`
(`Vec<T,0> → Vec`), `CALLS`, `HAS_METHOD`/`HAS_FIELD` with `access`, `CONTAINS`, `USES`.)

## A query per new relation (Cypher)

Relationship types are the upper-snake edge kinds; node labels are the node kinds.
These run against the Neo4j sink; the IndraDB sink answers the same shapes via its
verb subset (ordered edges use `edge_index`).

```cypher
// RETURNS — return type of a function (Q4)
MATCH (f:Method {name:'area'})-[:RETURNS]->(t:Type) RETURN t.type_spelling;

// HAS_PARAM + OF_TYPE — full signature of a function, ordered (Q3)
MATCH (f:Function {name:'scale'})-[hp:HAS_PARAM]->(p:Parameter)-[:OF_TYPE]->(t:Type)
RETURN p.name, t.type_spelling ORDER BY hp.edge_index;

// POINTS_TO — pointee of a pointer type
MATCH (t:Type)-[:POINTS_TO]->(pointee:Type) RETURN t.type_spelling, pointee.type_spelling;

// REFERS_TO — referent of a reference type
MATCH (t:Type)-[:REFERS_TO]->(referent:Type) RETURN t.type_spelling, referent.type_spelling;

// ALIAS_OF — chase a typedef/using chain
MATCH (a:Typedef {name:'Length'})-[:ALIAS_OF]->(t:Type) RETURN t.type_spelling;

// TEMPLATE_PARAM — parameters of a template, ordered
MATCH (d:TemplateDef {name:'Vec'})-[tp:TEMPLATE_PARAM]->(p:Parameter)
RETURN p.name, p.param_kind ORDER BY tp.edge_index;

// TEMPLATE_ARG — args of a specialization/instantiation, ordered (Q8)
MATCH (s:Specialization)-[ta:TEMPLATE_ARG]->(arg) RETURN arg ORDER BY ta.edge_index;

// CONSTRAINED_BY — concept constraining a template param (see caveat: not yet emitted)
MATCH (p:Parameter)-[:CONSTRAINED_BY]->(c:Concept) RETURN p.name, c.name;

// USES_NAMESPACE / USES_DECLARATION
MATCH (s)-[:USES_NAMESPACE]->(ns:Namespace) RETURN s, ns;
MATCH (s)-[:USES_DECLARATION]->(sym) RETURN s, sym;

// ENUMERATOR_OF + UNDERLYING_TYPE
MATCH (e:Enumerator)-[:ENUMERATOR_OF]->(en:Enum) RETURN en.name, e.name, e.enum_value;
MATCH (en:Enum {name:'Color'})-[:UNDERLYING_TYPE]->(t:Type) RETURN t.type_spelling;
```

## Honest fidelity caveats (observed in this run)

These are consistent with `SCHEMA.md` §Binding-gap; the example surfaces them rather
than hiding them:

1. **`Concept` / `CONSTRAINED_BY` not emitted.** `clang-rs 2.0.0` lacks
   `EntityKind::ConceptDecl`, so `concept Number` yields no `Concept` node and no
   `CONSTRAINED_BY` edge; the constraint appears instead as a pseudo
   `INSTANTIATES → Number` reference.
2. **`UNDERLYING_TYPE` reported `int`, not `uint8_t`.** Despite the explicit
   `enum class Color : std::uint8_t`, the underlying type resolved to `int` (the
   `std::uint8_t` alias comes from a skipped system header). `UNDERLYING_TYPE` is
   emitted for *all* enums, including the compiler-default case.
3. **Concrete-instantiation `TEMPLATE_ARG` not captured.** `scale`'s return
   `Vec<double,3>` did not produce a concrete instantiation with `double`/`3` args —
   its return type even fell back to `int`. `TEMPLATE_ARG` is demonstrated here via
   the **partial specialization** `Vec<T,0>`, whose cursor *is* surfaced. Concrete
   `ClassTemplatePartialSpecialization`/instantiation cursors are not reliably
   surfaced on Apple Clang 17; may differ under libclang 18.
4. **Six bool props ship always-`false`** (e.g. `is_override` on `Circle::area`,
   even though the `OVERRIDES` edge is correctly emitted). See `SCHEMA.md`.
