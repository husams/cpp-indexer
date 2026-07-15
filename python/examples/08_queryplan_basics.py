#!/usr/bin/env python3
"""Example 08 — CXQ QueryPlan basics: build a plan, inspect it, run it.

The CXQ QueryPlan tier (`indexer.queryplan`, contract: docs/query-plan.md) is
the unified declarative query DSL. Instead of calling one graph verb at a time
(example 02/03), you compose a whole question as an immutable PIPELINE:

    plan = start(<source>) | <stage> | <stage> | ...

and hand it to an Executor. Three things make this worth learning:

    1. The plan is a VALUE (the QueryPlan IR). You can print it as canonical
       JSON, diff it, ship it across a process boundary — the C++ builder
       (src/query/plan.hpp) produces byte-identical JSON for the same plan.
    2. Validation is up-front and typed: a bad plan fails with a stable E_*
       code BEFORE any SQL runs.
    3. Execution is read-only, deterministic (ids ascending, stable field
       order) and budgeted (`truncated` is reported, never silently dropped).

Run (from the repo root — uses the checked-in self-index `index.db`):
    uv run --project python python python/examples/08_queryplan_basics.py
    # any other index:  CIDX_DB=/path/to/index.db uv run ... (same command)
"""

from __future__ import annotations

import os
import sys

from indexer.storage import Storage
from indexer.queryplan import (
    Executor, PlanError,
    # sources
    start, symbol,
    # stages
    in_, out, where, select, order_by, limit,
    # predicates
    eq, glob, all_of,
    # plan-as-value helpers
    canonical_json,
)

# The examples default to the repo's checked-in self-index (cidx indexing its
# own sources) so they run out of the box; CIDX_DB points them elsewhere.
_REPO_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", ".."))
DB_PATH = os.environ.get("CIDX_DB", os.path.join(_REPO_ROOT, "index.db"))


def main() -> None:
    if not os.path.exists(DB_PATH):
        sys.exit(f"no index at {DB_PATH} — set CIDX_DB or build one")

    # ----------------------------------------------------------------------- #
    # 1. BUILD a plan — who calls resolve_relation()?
    # ----------------------------------------------------------------------- #
    # start(source) opens the pipeline; each `| stage` returns a NEW immutable
    # Query (nothing mutates, prefixes are reusable).
    #
    #   symbol(ref)   resolves ref against usr, then qual_name, then spelling
    #                 (exact matches; all hits become the seed node stream).
    #   in_("calls")  walks `calls` edges BACKWARDS one level = the callers.
    #                 (out("calls") would be the callees. `in_` has a trailing
    #                 underscore because `in` is a Python keyword.)
    #   select([...]) turns the node stream into rows with exactly these
    #                 fields, in this order.
    #   order_by(...) sorts rows (fields must be in the select; ties by id).
    q = (
        start(symbol("cidx::query::resolve_relation"))
        | in_("calls")
        | select(["name", "file", "line"])
        | order_by(["name"])
    )

    # ----------------------------------------------------------------------- #
    # 2. INSPECT it — the plan is data, not behavior
    # ----------------------------------------------------------------------- #
    # canonical_json() validates + normalizes first (relation names become
    # layer-qualified: "calls" -> "symbol.calls") and prints the exact bytes
    # the C++ builder would produce for the same plan.
    print("== canonical JSON (the QueryPlan IR) ==")
    print(canonical_json(q.plan))

    # ----------------------------------------------------------------------- #
    # 3. RUN it
    # ----------------------------------------------------------------------- #
    # The Executor runs validated plans over an open Storage. Execution is
    # read-only parameterized SQL — a plan can never mutate the index.
    db = Storage(DB_PATH)
    try:
        ex = Executor(db)
        res = ex.run(q.plan)

        # Result.to_dict() is the stable JSON shape (docs/query-plan.md):
        #   { shape: nodes|rows|scalar, view, count, truncated, rows: [...] }
        out_dict = res.to_dict()
        print("== callers of resolve_relation ==")
        print(f"  shape={out_dict['shape']}  view={out_dict['view']}  "
              f"count={out_dict['count']}  truncated={out_dict['truncated']}")
        for row in out_dict["rows"]:
            print(f"  {row['name']:<40} {row['file']}:{row['line']}")

        # ------------------------------------------------------------------- #
        # 4. NO select? You still get rows — the default node fields
        # ------------------------------------------------------------------- #
        # A plan that ends as a node stream materializes with the default
        # fields id, usr, name, kind.
        res = ex.run((start(symbol("cidx::query::validate"))
                      | out("calls")).plan)
        print("\n== direct callees of validate (default node fields) ==")
        for row in res.to_dict()["rows"][:5]:
            print(f"  #{row['id']:<6} {row['kind']:<10} {row['name']}")

        # ------------------------------------------------------------------- #
        # 5. FILTER with where() — predicates are composable values too
        # ------------------------------------------------------------------- #
        # eq/ne/glob/in_list compare one field; all_of/any_of/not_ combine.
        # `kind` is always the C++ DECLARATION kind (function, class, method,
        # ...); entity classification is the separate `entity_type` field
        # (example 09).
        res = ex.run((
            start(symbol("cidx::query::validate"))
            | out("calls", 1, 2)                       # callees, depth 1..2
            | where(all_of([eq("kind", "function"),
                            glob("name", "*pred*")]))  # SQLite GLOB pattern
            | select(["name", "kind"])
            | limit(10)
        ).plan)
        print("\n== depth<=2 callees of validate whose name GLOBs '*pred*' ==")
        for row in res.to_dict()["rows"]:
            print(f"  {row['kind']:<10} {row['name']}")

        # ------------------------------------------------------------------- #
        # 6. INVALID plans fail fast — stable E_* codes, no SQL runs
        # ------------------------------------------------------------------- #
        print("\n== validation errors (PlanError, E_* identity) ==")
        for bad in (
            start(symbol("f")) | out("no_such_relation"),   # E_RELATION
            start(symbol("f")) | out("calls", 1, 99),       # E_DEPTH (max 32)
            start(symbol("f")) | select(["name"]) | out("calls"),  # E_STAGE
        ):
            try:
                ex.run(bad.plan)
            except PlanError as e:
                print(f"  {e}")
    finally:
        db.close()


if __name__ == "__main__":
    main()
