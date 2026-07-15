#!/usr/bin/env python3
"""Example 09 — CXQ QueryPlan advanced: views, depth windows, set algebra.

Builds on example 08 (build/inspect/run). This one covers the parts of the
DSL that answer architecture-level questions:

    * codebase() + nodes(pred) ....... enumerate a whole domain, filtered
    * the two VIEWS .................. symbol (declarations) vs entity
                                       (Layer-1 design types) and how a
                                       traversal retargets the view
    * depth windows .................. out(rel, min, max) path-length window
    * set algebra .................... union_ / intersect / except_
    * scalars and shaping ............ count / distinct / order_by / limit
    * budgets ........................ the `truncated` flag

Run (from the repo root — uses the checked-in self-index `index.db`):
    uv run --project python python python/examples/09_queryplan_advanced.py
    # any other index:  CIDX_DB=/path/to/index.db uv run ... (same command)
"""

from __future__ import annotations

import os
import sys

from indexer.storage import Storage
from indexer import queryplan as qp
from indexer.queryplan import (
    Executor,
    start, codebase, symbol, entity,
    nodes, view, out, in_, intersect, except_,
    select, count, distinct, order_by, limit,
    eq, glob, in_list, all_of, not_,
)

_REPO_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", ".."))
DB_PATH = os.environ.get("CIDX_DB", os.path.join(_REPO_ROOT, "index.db"))


def main() -> None:
    if not os.path.exists(DB_PATH):
        sys.exit(f"no index at {DB_PATH} — set CIDX_DB or build one")
    db = Storage(DB_PATH)
    try:
        run(Executor(db))
    finally:
        db.close()


def run(ex: Executor) -> None:
    # ----------------------------------------------------------------------- #
    # 1. ENUMERATE the codebase — codebase() must be opened with nodes()
    # ----------------------------------------------------------------------- #
    # codebase() is a lazy source: nodes(pred) enumerates the CURRENT view's
    # domain (here: all symbols) with the predicate pushed into SQL. Ending a
    # plan (or filtering) before nodes() is an E_STAGE error — enumeration is
    # always explicit.
    res = ex.run((
        start(codebase())
        | nodes(all_of([eq("kind", "class"), eq("is_definition", True)]))
        | count()
    ).plan)
    print(f"class definitions in the index: {res.to_dict()['count']}")

    # ----------------------------------------------------------------------- #
    # 2. The ENTITY view — Layer-1 design types over the same ids
    # ----------------------------------------------------------------------- #
    # view(entity) retypes the stream: ids WITHOUT an entity_node row are
    # dropped (never mapped). `entity_type` is the design classification —
    # a separate field from `kind`, so an abstract struct is
    # kind=struct AND entity_type=abstract_class.
    res = ex.run((
        start(codebase())
        | view(qp.ENTITY_VIEW)
        | nodes(in_list("entity_type", ["abstract_class", "interface"]))
        | select(["name", "kind", "entity_type"])
        | order_by(["name"])
    ).plan)
    print("\n== abstract classes / interfaces (entity view) ==")
    for row in res.to_dict()["rows"]:
        print(f"  {row['entity_type']:<15} kind={row['kind']:<8} {row['name']}")

    # ----------------------------------------------------------------------- #
    # 3. ENTITY sources and entity relations
    # ----------------------------------------------------------------------- #
    # entity(ref) seeds from Layer-1 directly. Relations are namespaced per
    # view: bare names resolve in the ACTIVE view ("uses" here is
    # entity.uses); the qualified forms symbol.uses / entity.uses always
    # work. A traversal retargets the stream to its relation's layer.
    res = ex.run((
        start(entity("cidx::Storage"))
        | in_("uses")                       # entity.uses, inbound = users
        | select(["name", "entity_type"])
        | order_by(["name"])
        | limit(8)
    ).plan)
    print("\n== design-level users of cidx::Storage (entity.uses) ==")
    for row in res.to_dict()["rows"]:
        print(f"  {row['entity_type'] or '-':<15} {row['name']}")

    # ----------------------------------------------------------------------- #
    # 4. DEPTH WINDOWS — out(rel, min, max) is a path-length window
    # ----------------------------------------------------------------------- #
    # A node is emitted iff SOME path of length d in [min, max] reaches it —
    # not only its shortest-path depth (in a diamond A→B, A→C→B, out(r, 2, 2)
    # still emits B). max is required and capped at 32.
    direct = ex.run((start(symbol("cidx::query::validate"))
                     | out("calls") | count()).plan)
    within3 = ex.run((start(symbol("cidx::query::validate"))
                      | out("calls", 1, 3) | count()).plan)
    only23 = ex.run((start(symbol("cidx::query::validate"))
                     | out("calls", 2, 3) | count()).plan)
    print("\n== callee closure of validate ==")
    print(f"  depth 1 (direct) : {direct.to_dict()['count']}")
    print(f"  depth 1..3       : {within3.to_dict()['count']}")
    print(f"  depth 2..3 only  : {only23.to_dict()['count']}")

    # ----------------------------------------------------------------------- #
    # 5. SET ALGEBRA — combine whole sub-plans
    # ----------------------------------------------------------------------- #
    # union_/intersect/except_ take another Query whose plan must end as a
    # node stream in the SAME view. All three are true set ops over deduped
    # id sets. Here: which functions are reachable (depth<=3) from BOTH
    # entry points?
    shared = ex.run((
        start(symbol("cidx::query::canonical_json")) | out("calls", 1, 3)
        | intersect(start(symbol("cidx::query::validate"))
                    | out("calls", 1, 3))
        | select(["name"]) | order_by(["name"]) | limit(10)
    ).plan)
    print("\n== shared callees of canonical_json and validate (depth<=3) ==")
    for row in shared.to_dict()["rows"]:
        print(f"  {row['name']}")

    # ... and: callees of validate that canonical_json can NOT reach.
    only = ex.run((
        start(symbol("cidx::query::validate")) | out("calls", 1, 2)
        | except_(start(symbol("cidx::query::canonical_json"))
                  | out("calls", 1, 3))
        | count()
    ).plan)
    print(f"\nvalidate-only callees (except_): {only.to_dict()['count']}")

    # ----------------------------------------------------------------------- #
    # 6. ROW shaping — distinct() after select()
    # ----------------------------------------------------------------------- #
    # select() may project columns that repeat (two methods on one file);
    # distinct() dedups whole row tuples. Node streams are always deduped.
    res = ex.run((
        start(codebase())
        | nodes(all_of([eq("kind", "function"),
                        glob("name", "cidx::query::*"),
                        not_(glob("name", "*detail*"))]))
        | select(["kind"]) | distinct()
    ).plan)
    print(f"\ndistinct kinds under cidx::query::*: "
          f"{[r['kind'] for r in res.to_dict()['rows']]}")

    # ----------------------------------------------------------------------- #
    # 7. BUDGETS — truncation is reported, never silent
    # ----------------------------------------------------------------------- #
    # Enumeration and traversal are budgeted (10 000 states) and results are
    # capped at 1 000 rows unless a later limit() is in effect. Hitting any
    # budget sets truncated=True on the result — check it when a number
    # must be exact. count() ignores the result cap (it counts the full
    # budget-bounded stream).
    res = ex.run((start(codebase()) | nodes()).plan)
    d = res.to_dict()
    print(f"\nall symbols, no limit(): count={d['count']} "
          f"truncated={d['truncated']}  (default result cap at work)")


if __name__ == "__main__":
    main()
