"""CXQ QueryPlan tier: IR, relation catalog, pipeline builder, SQLite executor.

Contract: docs/query-plan.md (v1). This is the Python twin of C++
``src/query/plan.{hpp,cpp}`` + ``src/query/exec.cpp``: both builders produce
the same immutable plan tree, ``canonical_json()`` must be byte-identical to
the C++ side (pinned by the shared golden ``tests/golden/cxq_plans.txt``), and
the executor mirrors the C++ SQL shapes so results match by construction.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field, replace
from enum import Enum
from typing import Any, NoReturn, Optional, Sequence

from .storage import (
    SYMBOL_KIND_IDS, SYMBOL_KIND_NAMES, SYMBOL_KINDS, Storage,
)

__all__ = [
    "PlanError", "TraversalMode", "Pred", "Stage", "Source", "Plan", "Query", "Result",
    "Executor", "start", "codebase", "symbol", "entity",
    "all_of", "any_of", "not_", "eq", "ne", "glob", "in_list",
    "nodes", "view", "where", "out", "in_", "union_", "intersect", "except_",
    "select", "count", "distinct", "order_by", "limit",
    "validate", "canonical_json", "relation_catalog", "resolve_relation",
]

# ---- Budgets (docs/query-plan.md "Execution semantics") ----------------------

TRAVERSE_NODE_BUDGET = 10000
ENUMERATE_BUDGET = 10000
DEFAULT_RESULT_CAP = 1000
ID_CHUNK = 400


class PlanError(Exception):
    """Validation failure; the message starts with the stable E_* code."""


def _fail(code: str, what: str) -> NoReturn:
    raise PlanError(f"{code}: {what}")


class TraversalMode(str, Enum):
    """Execution policy for a typed graph traversal stage."""

    STATIC = "static"
    DEVIRTUALIZED = "devirtualized"


# ---- Views --------------------------------------------------------------------

SYMBOL_VIEW = "symbol"
ENTITY_VIEW = "entity"

# ---- Relation catalog -----------------------------------------------------------
# (name, layer, kind_id) -- same rows as C++ relation_catalog().

RELATION_CATALOG: tuple[tuple[str, str, int], ...] = (
    # Layer-0 edge kinds (edge_kind seed).
    ("calls", SYMBOL_VIEW, 1),
    ("inherits", SYMBOL_VIEW, 2),
    ("contains", SYMBOL_VIEW, 3),
    ("specializes", SYMBOL_VIEW, 4),
    ("instantiates", SYMBOL_VIEW, 5),
    ("overrides", SYMBOL_VIEW, 6),
    ("uses", SYMBOL_VIEW, 7),
    ("field_of", SYMBOL_VIEW, 8),
    ("method_of", SYMBOL_VIEW, 9),
    ("construct-value", SYMBOL_VIEW, 10),
    ("construct-temp", SYMBOL_VIEW, 11),
    ("construct-heap", SYMBOL_VIEW, 12),
    ("construct-copy", SYMBOL_VIEW, 13),
    ("construct-move", SYMBOL_VIEW, 14),
    ("factory-construct", SYMBOL_VIEW, 15),
    ("destroy", SYMBOL_VIEW, 16),
    ("friend", SYMBOL_VIEW, 17),
    ("dispatch_calls", SYMBOL_VIEW, 18),
    # Layer-1 entity_edge kinds (entity_edge_kind seed).
    ("generalizes", ENTITY_VIEW, 1),
    ("implements", ENTITY_VIEW, 2),
    ("specializes", ENTITY_VIEW, 3),
    ("composes", ENTITY_VIEW, 4),
    ("aggregates", ENTITY_VIEW, 5),
    ("associates", ENTITY_VIEW, 6),
    ("creates", ENTITY_VIEW, 7),
    ("uses", ENTITY_VIEW, 8),
    ("destroys", ENTITY_VIEW, 9),
    ("befriends", ENTITY_VIEW, 10),
    ("instantiates", ENTITY_VIEW, 11),
    ("declares", ENTITY_VIEW, 12),
)

#: entity_kind seed names; index == stored id.
ENTITY_KIND_NAMES = (
    "other", "class", "abstract_class", "interface", "union", "enum",
    "class_template", "abstract_class_template", "interface_template",
    "namespace",
)


def relation_catalog() -> tuple[tuple[str, str, int], ...]:
    return RELATION_CATALOG


def resolve_relation(name: str, active: str) -> Optional[tuple[str, str, int]]:
    """Resolve a bare or 'symbol.'/'entity.'-qualified relation name."""
    bare, layer = name, active
    if name.startswith("symbol."):
        bare, layer = name[7:], SYMBOL_VIEW
    elif name.startswith("entity."):
        bare, layer = name[7:], ENTITY_VIEW
    for row in RELATION_CATALOG:
        if row[0] == bare and row[1] == layer:
            return row
    return None


# ---- Field catalog ---------------------------------------------------------------
# name -> (filterable, is_string). `kind` is ALWAYS the C++ declaration kind
# (symbol_kind names); `entity_type` is ALWAYS the Layer-1 classification
# (entity_kind names, null for non-entities) -- separate fields so
# `kind in [class, struct]` keeps its declaration-kind meaning (PR #20 review).

_FIELDS: dict[str, tuple[bool, bool]] = {
    "id": (True, False),
    "usr": (True, True),
    "name": (True, True),
    "spelling": (True, True),
    "qual_name": (True, True),
    "kind": (True, True),
    "entity_type": (True, True),
    "is_definition": (True, False),
    "is_pure": (True, False),
    "is_static": (True, False),
    "file": (False, True),
    "line": (False, False),
    "col": (False, False),
}

# ---- Predicates ---------------------------------------------------------------


@dataclass(frozen=True)
class Pred:
    op: str  # all_of | any_of | not | eq | ne | glob | in
    kids: tuple["Pred", ...] = ()
    field: str = ""
    str_values: tuple[str, ...] = ()
    int_value: Optional[int] = None


def all_of(preds: Sequence[Pred]) -> Pred:
    return Pred(op="all_of", kids=tuple(preds))


def any_of(preds: Sequence[Pred]) -> Pred:
    return Pred(op="any_of", kids=tuple(preds))


def not_(pred: Pred) -> Pred:
    return Pred(op="not", kids=(pred,))


def eq(field_name: str, value: Any) -> Pred:
    if isinstance(value, bool):
        return Pred(op="eq", field=field_name, int_value=1 if value else 0)
    if isinstance(value, int):
        return Pred(op="eq", field=field_name, int_value=value)
    return Pred(op="eq", field=field_name, str_values=(value,))


def ne(field_name: str, value: str) -> Pred:
    return Pred(op="ne", field=field_name, str_values=(value,))


def glob(field_name: str, pattern: str) -> Pred:
    return Pred(op="glob", field=field_name, str_values=(pattern,))


def in_list(field_name: str, values: Sequence[str]) -> Pred:
    return Pred(op="in", field=field_name, str_values=tuple(values))


# ---- Stages / Source / Plan --------------------------------------------------------


@dataclass(frozen=True)
class Stage:
    op: str  # nodes | view | where | out | in | union | intersect | except
    #        # | select | count | distinct | order_by | limit
    pred: Optional[Pred] = None
    level: str = SYMBOL_VIEW
    relation: str = ""
    mode: str = TraversalMode.STATIC.value
    min_depth: int = 1
    max_depth: int = 1
    operand: Optional["Plan"] = None
    fields: tuple[str, ...] = ()
    n: int = 0


@dataclass(frozen=True)
class Source:
    kind: str  # codebase | symbol | entity
    ref: str = ""


@dataclass(frozen=True)
class Plan:
    source: Source
    stages: tuple[Stage, ...] = ()


class Query:
    """Immutable pipeline builder: ``start(symbol("f")) | out("calls")``."""

    def __init__(self, source: Source, stages: tuple[Stage, ...] = ()):
        self._plan = Plan(source=source, stages=stages)

    @property
    def plan(self) -> Plan:
        return self._plan

    def __or__(self, stage: Stage) -> "Query":
        return Query(self._plan.source, self._plan.stages + (stage,))


def start(source: Source) -> Query:
    return Query(source)


def codebase() -> Source:
    return Source(kind="codebase")


def symbol(ref: str) -> Source:
    return Source(kind="symbol", ref=ref)


def entity(ref: str) -> Source:
    return Source(kind="entity", ref=ref)


def nodes(pred: Optional[Pred] = None) -> Stage:
    return Stage(op="nodes", pred=pred)


def view(level: str) -> Stage:
    return Stage(op="view", level=level)


def where(pred: Pred) -> Stage:
    return Stage(op="where", pred=pred)


def out(
    relation: str,
    min_depth: int = 1,
    max_depth: int = 1,
    mode: TraversalMode | str = TraversalMode.STATIC,
) -> Stage:
    mode_value = mode.value if isinstance(mode, TraversalMode) else mode
    return Stage(op="out", relation=relation, mode=mode_value,
                 min_depth=min_depth, max_depth=max_depth)


def in_(relation: str, min_depth: int = 1, max_depth: int = 1) -> Stage:
    return Stage(op="in", relation=relation, min_depth=min_depth,
                 max_depth=max_depth)


def union_(operand: Query) -> Stage:
    return Stage(op="union", operand=operand.plan)


def intersect(operand: Query) -> Stage:
    return Stage(op="intersect", operand=operand.plan)


def except_(operand: Query) -> Stage:
    return Stage(op="except", operand=operand.plan)


def select(fields: Sequence[str]) -> Stage:
    return Stage(op="select", fields=tuple(fields))


def count() -> Stage:
    return Stage(op="count")


def distinct() -> Stage:
    return Stage(op="distinct")


def order_by(fields: Sequence[str]) -> Stage:
    return Stage(op="order_by", fields=tuple(fields))


def limit(n: int) -> Stage:
    return Stage(op="limit", n=n)


# ---- Validation / normalization -----------------------------------------------------


def _check_cmp(p: Pred) -> None:
    desc = _FIELDS.get(p.field)
    if desc is None:
        _fail("E_FIELD", f"unknown field '{p.field}'")
    filterable, is_string = desc
    if not filterable:
        _fail("E_FIELD", f"field '{p.field}' is select-only")
    if is_string:
        if p.int_value is not None:
            _fail("E_FIELD", f"field '{p.field}' takes a string value")
        bad_arity = (len(p.str_values) == 0 if p.op == "in"
                     else len(p.str_values) != 1)
        if bad_arity:
            _fail("E_FIELD", f"bad value arity for field '{p.field}'")
    else:
        if p.op in ("glob", "in"):
            _fail("E_FIELD", f"field '{p.field}' supports eq/ne only")
        if p.int_value is None:
            _fail("E_FIELD", f"field '{p.field}' takes an integer value")
    if p.field in ("kind", "entity_type") and p.op == "glob":
        _fail("E_FIELD", f"field '{p.field}' does not support glob")
    if p.field == "kind":
        for v in p.str_values:
            if v not in SYMBOL_KINDS:
                _fail("E_KIND", f"unknown symbol kind '{v}'")
    if p.field == "entity_type":
        for v in p.str_values:
            if v not in ENTITY_KIND_NAMES:
                _fail("E_KIND", f"unknown entity_type '{v}'")


def _norm_pred(p: Pred) -> Pred:
    if p.op in ("all_of", "any_of"):
        if not p.kids:
            _fail("E_FIELD", "empty boolean combinator")
        kids: list[Pred] = []
        for k in p.kids:
            nk = _norm_pred(k)
            if nk.op == p.op:
                kids.extend(nk.kids)
            else:
                kids.append(nk)
        if len(kids) == 1:
            return kids[0]
        return Pred(op=p.op, kids=tuple(kids))
    if p.op == "not":
        if len(p.kids) != 1:
            _fail("E_FIELD", "not() takes exactly one predicate")
        nk = _norm_pred(p.kids[0])
        if nk.op == "not":
            return nk.kids[0]  # not(not(p)) -> p
        return Pred(op="not", kids=(nk,))
    if p.op in ("eq", "ne", "glob", "in"):
        _check_cmp(p)
        return p
    _fail("E_FIELD", "bad predicate")


class _WalkState:
    def __init__(self) -> None:
        self.active = SYMBOL_VIEW
        self.shape = "nodes"  # nodes | rows | scalar
        self.codebase_unenumerated = False
        self.selected: tuple[str, ...] = ()


def _validate_walk(plan: Plan, st: _WalkState) -> Plan:
    if plan.source.kind == "codebase":
        st.codebase_unenumerated = True
    elif not plan.source.ref:
        _fail("E_SOURCE", "empty source ref")
    st.active = ENTITY_VIEW if plan.source.kind == "entity" else SYMBOL_VIEW

    def consume() -> None:
        if st.codebase_unenumerated:
            _fail("E_STAGE", "codebase() must be enumerated with nodes() first")

    out_stages: list[Stage] = []
    for stage in plan.stages:
        ns = stage
        if st.shape == "scalar":
            _fail("E_STAGE", "no stage may follow count()")
        if stage.op == "nodes":
            if not st.codebase_unenumerated:
                _fail("E_STAGE",
                      "nodes() requires an unenumerated codebase() source")
            if stage.pred is not None:
                ns = replace(stage, pred=_norm_pred(stage.pred))
            st.codebase_unenumerated = False
        elif stage.op == "view":
            if st.shape != "nodes":
                _fail("E_STAGE", "view() applies to a node stream")
            if stage.level not in (SYMBOL_VIEW, ENTITY_VIEW):
                _fail("E_VIEW", f"unknown view '{stage.level}'")
            st.active = stage.level
        elif stage.op == "where":
            if st.shape != "nodes":
                _fail("E_STAGE", "where() applies to a node stream")
            if stage.pred is None:
                _fail("E_FIELD", "where() requires a predicate")
            consume()
            ns = replace(stage, pred=_norm_pred(stage.pred))
        elif stage.op in ("out", "in"):
            consume()
            if st.shape != "nodes":
                _fail("E_STAGE", "traversal applies to a node stream")
            rel = resolve_relation(stage.relation, st.active)
            if rel is None:
                _fail("E_RELATION",
                      f"unknown relation '{stage.relation}' in "
                      f"{st.active} view")
            if not 1 <= stage.min_depth <= stage.max_depth <= 32:
                _fail("E_DEPTH", "depth bounds must satisfy 1 <= min <= max <= 32")
            if stage.mode not in (
                TraversalMode.STATIC.value,
                TraversalMode.DEVIRTUALIZED.value,
            ):
                _fail("E_STAGE", f"unknown traversal mode '{stage.mode}'")
            if stage.mode == TraversalMode.DEVIRTUALIZED.value and not (
                stage.op == "out" and rel[1] == SYMBOL_VIEW and rel[0] == "calls"
            ):
                _fail(
                    "E_STAGE",
                    "devirtualized mode requires an outbound symbol.calls traversal",
                )
            ns = replace(stage, relation=f"{rel[1]}.{rel[0]}")
            # Traversal targets live in the relation's layer: the stream view
            # (and later bare-relation resolution) follows it.
            st.active = rel[1]
        elif stage.op in ("union", "intersect", "except"):
            consume()
            if st.shape != "nodes":
                _fail("E_STAGE", "set operations apply to node streams")
            if stage.operand is None:
                _fail("E_SETOP", "missing operand plan")
            sub = _WalkState()
            nop = _validate_walk(stage.operand, sub)
            if sub.shape != "nodes":
                _fail("E_SETOP", "operand must yield a node stream")
            if sub.active != st.active:
                _fail("E_SETOP", "operand view mismatch")
            ns = replace(stage, operand=nop)
        elif stage.op == "select":
            consume()
            if st.shape != "nodes":
                _fail("E_STAGE", "select() applies to a node stream")
            if not stage.fields:
                _fail("E_FIELD", "select() requires at least one field")
            for f in stage.fields:
                if f not in _FIELDS:
                    _fail("E_FIELD", f"unknown field '{f}'")
            st.shape = "rows"
            st.selected = stage.fields
        elif stage.op == "count":
            consume()
            st.shape = "scalar"
        elif stage.op == "distinct":
            consume()
        elif stage.op == "order_by":
            consume()
            if not stage.fields:
                _fail("E_FIELD", "order_by() requires at least one field")
            for f in stage.fields:
                if f not in _FIELDS:
                    _fail("E_FIELD", f"unknown field '{f}'")
                if st.shape == "rows" and f not in st.selected:
                    _fail("E_FIELD", f"order_by field '{f}' is not selected")
        elif stage.op == "limit":
            consume()
            if stage.n < 1:
                _fail("E_LIMIT", "limit must be >= 1")
        else:
            _fail("E_STAGE", f"unknown stage '{stage.op}'")
        out_stages.append(ns)
    if st.codebase_unenumerated:
        _fail("E_STAGE", "codebase() must be enumerated with nodes() first")
    return Plan(source=plan.source, stages=tuple(out_stages))


def validate(plan: Plan) -> Plan:
    return _validate_walk(plan, _WalkState())


# ---- Canonical JSON -------------------------------------------------------------------


def _pred_to_dict(p: Pred) -> dict[str, Any]:
    if p.op in ("all_of", "any_of"):
        return {"op": p.op, "preds": [_pred_to_dict(k) for k in p.kids]}
    if p.op == "not":
        return {"op": "not", "pred": _pred_to_dict(p.kids[0])}
    if p.op == "in":
        return {"op": "in", "field": p.field, "values": list(p.str_values)}
    value: Any = p.int_value if p.int_value is not None else p.str_values[0]
    return {"op": p.op, "field": p.field, "value": value}


def _plan_to_dict(plan: Plan) -> dict[str, Any]:
    src: dict[str, Any] = {"kind": plan.source.kind}
    if plan.source.kind != "codebase":
        src["ref"] = plan.source.ref
    stages: list[dict[str, Any]] = []
    for s in plan.stages:
        o: dict[str, Any] = {"op": s.op}
        if s.op == "nodes":
            if s.pred is not None:
                o["pred"] = _pred_to_dict(s.pred)
        elif s.op == "view":
            o["level"] = s.level
        elif s.op == "where":
            o["pred"] = _pred_to_dict(s.pred)  # type: ignore[arg-type]
        elif s.op in ("out", "in"):
            o["relation"] = s.relation
            if s.mode != TraversalMode.STATIC.value:
                o["mode"] = s.mode
            o["min_depth"] = s.min_depth
            o["max_depth"] = s.max_depth
        elif s.op in ("union", "intersect", "except"):
            o["plan"] = _plan_to_dict(s.operand)  # type: ignore[arg-type]
        elif s.op in ("select", "order_by"):
            o["fields"] = list(s.fields)
        elif s.op == "limit":
            o["n"] = s.n
        stages.append(o)
    return {"cxq": 1, "source": src, "stages": stages}


def plan_to_dict(plan: Plan) -> dict[str, Any]:
    return _plan_to_dict(validate(plan))


def canonical_json(plan: Plan) -> str:
    return json.dumps(plan_to_dict(plan), indent=2)


# ---- Executor -----------------------------------------------------------------------


@dataclass
class Result:
    shape: str  # nodes | rows | scalar
    view: str
    truncated: bool = False
    scalar: int = 0
    fields: tuple[str, ...] = ()
    rows: list[tuple[Any, ...]] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        if self.shape == "scalar":
            return {"shape": "scalar", "view": self.view, "count": self.scalar,
                    "truncated": self.truncated}
        return {
            "shape": self.shape,
            "view": self.view,
            "count": len(self.rows),
            "truncated": self.truncated,
            "rows": [dict(zip(self.fields, row)) for row in self.rows],
        }


def _col_expr(field_name: str) -> str:
    if field_name == "id":
        return "s.id"
    if field_name == "usr":
        return "s.usr"
    if field_name == "name":
        return "COALESCE(s.qual_name, s.spelling)"
    if field_name == "spelling":
        return "s.spelling"
    if field_name == "qual_name":
        return "s.qual_name"
    if field_name == "kind":
        return "s.kind"
    if field_name == "entity_type":
        return "en.kind"
    if field_name == "is_definition":
        return "s.is_definition"
    if field_name == "is_pure":
        return "s.is_pure"
    if field_name == "is_static":
        return "s.is_static"
    if field_name == "file":
        return "s.file_id"
    if field_name == "line":
        return "s.line"
    if field_name == "col":
        return "s.col"
    raise PlanError(f"E_FIELD: unknown field '{field_name}'")


def _kind_value_id(field_name: str, name: str) -> int:
    if field_name == "entity_type":
        try:
            return ENTITY_KIND_NAMES.index(name)
        except ValueError:
            return -1
    return SYMBOL_KIND_IDS.get(name, -1)


def _entity_type_name(raw: int) -> Optional[str]:
    return ENTITY_KIND_NAMES[raw] if 0 <= raw <= 9 else None


def _pred_sql(p: Pred, sql: list[str], args: list[Any]) -> None:
    if p.op in ("all_of", "any_of"):
        joiner = " AND " if p.op == "all_of" else " OR "
        sql.append("(")
        for i, k in enumerate(p.kids):
            if i:
                sql.append(joiner)
            _pred_sql(k, sql, args)
        sql.append(")")
        return
    if p.op == "not":
        sql.append("NOT (")
        _pred_sql(p.kids[0], sql, args)
        sql.append(")")
        return
    col = _col_expr(p.field)
    is_kind = p.field in ("kind", "entity_type")
    if p.op in ("eq", "ne"):
        sql.append(col + (" = ?" if p.op == "eq" else " != ?"))
        if p.int_value is not None:
            args.append(p.int_value)
        elif is_kind:
            args.append(_kind_value_id(p.field, p.str_values[0]))
        else:
            args.append(p.str_values[0])
        return
    if p.op == "glob":
        sql.append(col + " GLOB ?")
        args.append(p.str_values[0])
        return
    # in
    sql.append(col + " IN (" + ",".join("?" * len(p.str_values)) + ")")
    for v in p.str_values:
        args.append(_kind_value_id(p.field, v) if is_kind else v)


def _pred_uses_entity_type(p: Pred) -> bool:
    if p.field == "entity_type":
        return True
    return any(_pred_uses_entity_type(k) for k in p.kids)


def _cell_key(c: Any) -> tuple:
    """Deterministic Cell ordering: ints < strings < null (C++ cell_rank)."""
    if c is None:
        return (2, 0)
    if isinstance(c, int):
        return (0, c)
    return (1, c)


class _Stream:
    def __init__(self) -> None:
        self.view = SYMBOL_VIEW
        self.shape = "nodes"
        self.ids: list[int] = []
        self.fields: tuple[str, ...] = ()
        self.rows: list[tuple[Any, ...]] = []
        self.row_ids: list[int] = []
        self.truncated = False
        # True only while a limit() is in effect with NO cardinality-expanding
        # stage (nodes/out/in/union) after it -- otherwise _finish()
        # re-applies the default result cap (PR #20 review).
        self.limit_in_effect = False


class Executor:
    """Run validated plans against a cidx index via an open Storage."""

    def __init__(self, db: Storage):
        self._db = db
        self._conn = db._conn  # noqa: SLF001 -- same-package read access
        self._file_paths: dict[int, Optional[str]] = {}

    # -- public -------------------------------------------------------------

    def run(self, plan: Plan) -> Result:
        normalized = validate(plan)
        st = self._run_plan(normalized)
        return self._finish(st)

    # -- plan walk ------------------------------------------------------------

    def _run_plan(self, plan: Plan) -> _Stream:
        st = _Stream()
        st.view = ENTITY_VIEW if plan.source.kind == "entity" else SYMBOL_VIEW
        if plan.source.kind != "codebase":
            st.ids = self._resolve_source(plan.source)
        for stage in plan.stages:
            if stage.op == "nodes":
                self._enumerate(st, stage.pred)
                st.limit_in_effect = False
            elif stage.op == "view":
                self._change_view(st, stage.level)
            elif stage.op == "where":
                self._filter(st, stage.pred)  # type: ignore[arg-type]
            elif stage.op in ("out", "in"):
                if stage.mode == TraversalMode.DEVIRTUALIZED.value:
                    self._traverse_devirtualized(st, stage)
                else:
                    self._traverse(st, stage)
                st.limit_in_effect = False
            elif stage.op in ("union", "intersect", "except"):
                self._set_op(st, stage)
                if stage.op == "union":
                    st.limit_in_effect = False
            elif stage.op == "select":
                self._materialize(st, stage.fields)
                st.shape = "rows"
            elif stage.op == "count":
                st.shape = "scalar"
            elif stage.op == "distinct":
                self._apply_distinct(st)
            elif stage.op == "order_by":
                self._apply_order(st, stage.fields)
            elif stage.op == "limit":
                self._apply_limit(st, stage.n)
            if st.shape == "scalar":
                break  # count() is terminal
        return st

    # -- stages ----------------------------------------------------------------

    @staticmethod
    def _join_clause(need_entity: bool) -> str:
        return " LEFT JOIN entity_node en ON en.id = s.id" if need_entity \
            else ""

    def _resolve_source(self, src: Source) -> list[int]:
        join = (" JOIN entity_node en ON en.id = s.id"
                if src.kind == "entity" else "")
        for col in ("s.usr", "s.qual_name", "s.spelling"):
            rows = self._conn.execute(
                f"SELECT s.id FROM symbol s{join} WHERE {col} = ? "
                "ORDER BY s.id",
                (src.ref,),
            ).fetchall()
            if rows:
                return [r["id"] for r in rows]
        return []

    def _change_view(self, st: _Stream, level: str) -> None:
        """view(entity) enforces the typed-view invariant: ids without an
        entity_node row are DROPPED, never surfaced as entity rows (PR #20
        review). view(symbol) is a pure relabel."""
        if level == ENTITY_VIEW and st.view != ENTITY_VIEW:
            kept: list[int] = []
            for at in range(0, len(st.ids), ID_CHUNK):
                chunk = st.ids[at:at + ID_CHUNK]
                sql = ("SELECT id FROM entity_node WHERE id IN ("
                       + ",".join("?" * len(chunk)) + ") ORDER BY id")
                kept.extend(r["id"] for r in self._conn.execute(sql, chunk))
            st.ids = sorted(set(kept))
        st.view = level

    def _enumerate(self, st: _Stream, pred: Optional[Pred]) -> None:
        sql = ["SELECT s.id FROM symbol s"]
        if st.view == ENTITY_VIEW:
            sql.append(" JOIN entity_node en ON en.id = s.id")
        elif pred is not None and _pred_uses_entity_type(pred):
            sql.append(self._join_clause(True))
        args: list[Any] = []
        if pred is not None:
            sql.append(" WHERE ")
            _pred_sql(pred, sql, args)
        sql.append(" ORDER BY s.id LIMIT ?")
        args.append(ENUMERATE_BUDGET + 1)
        st.ids = [r["id"] for r in self._conn.execute("".join(sql), args)]
        if len(st.ids) > ENUMERATE_BUDGET:
            del st.ids[ENUMERATE_BUDGET:]
            st.truncated = True

    def _filter(self, st: _Stream, pred: Pred) -> None:
        out_ids: list[int] = []
        join = self._join_clause(_pred_uses_entity_type(pred))
        for at in range(0, len(st.ids), ID_CHUNK):
            chunk = st.ids[at:at + ID_CHUNK]
            sql = [
                f"SELECT s.id FROM symbol s{join} WHERE s.id IN ("
                + ",".join("?" * len(chunk)) + ") AND ("
            ]
            args: list[Any] = list(chunk)
            _pred_sql(pred, sql, args)
            sql.append(") ORDER BY s.id")
            out_ids.extend(
                r["id"] for r in self._conn.execute("".join(sql), args))
        st.ids = sorted(set(out_ids))

    def _traverse(self, st: _Stream, stage: Stage) -> None:
        """Path-length-window BFS (PR #20 review): a node is emitted iff SOME
        path of length d in [min_depth, max_depth] reaches it -- not only its
        shortest first-discovery depth. No cross-level visited set;
        termination comes from the finite max_depth (<= 32) and the state
        budget (cumulative level sizes). The stream view follows the
        relation's layer."""
        rel = resolve_relation(stage.relation, st.view)
        assert rel is not None  # validated
        entity_layer = rel[1] == ENTITY_VIEW
        table = "entity_edge" if entity_layer else "edge"
        outward = stage.op == "out"
        from_col = "src_id" if outward else "dst_id"
        to_col = "dst_id" if outward else "src_id"

        frontier = sorted(set(st.ids))
        emitted: set[int] = set()
        states = 0  # cumulative level sizes, bounded by the budget
        depth = 1
        while depth <= stage.max_depth and frontier:
            level: list[int] = []
            for at in range(0, len(frontier), ID_CHUNK):
                chunk = frontier[at:at + ID_CHUNK]
                sql = (
                    f"SELECT DISTINCT {to_col} FROM {table} "
                    f"WHERE kind = ? AND {from_col} IN ("
                    + ",".join("?" * len(chunk)) + ") ORDER BY 1"
                )
                level.extend(
                    r[0] for r in self._conn.execute(sql, [rel[2], *chunk]))
            level = sorted(set(level))
            if states + len(level) > TRAVERSE_NODE_BUDGET:
                del level[TRAVERSE_NODE_BUDGET - states:]
                st.truncated = True
            states += len(level)
            if depth >= stage.min_depth:
                emitted.update(level)
            if st.truncated:
                break
            frontier = level
            depth += 1
        st.ids = sorted(emitted)
        st.view = rel[1]

    def _traverse_devirtualized(self, st: _Stream, stage: Stage) -> None:
        """Run receiver-aware calls through the public compatibility model.

        Unknown receivers retain the conservative target set; exact by-value
        receivers use the same Gamma propagation as ``Callable`` callers.
        """
        from .model import Callable, CodeBase
        from .query import GraphQuery

        graph = GraphQuery.from_connection(self._conn, "<queryplan>")
        codebase = CodeBase(graph)
        emitted: set[int] = set()
        states = 0
        for root_id in sorted(set(st.ids)):
            root = codebase.wrap(graph.get(root_id))
            if not isinstance(root, Callable):
                continue
            for step in root.devirtualized_callgraph(
                depth=stage.max_depth,
                prune=True,
            ):
                states += 1
                if states > TRAVERSE_NODE_BUDGET:
                    st.truncated = True
                    break
                if step.depth >= stage.min_depth:
                    emitted.add(step.callee.id)
            if st.truncated:
                break
        st.ids = sorted(emitted)
        st.view = SYMBOL_VIEW

    def _set_op(self, st: _Stream, stage: Stage) -> None:
        sub = self._run_plan(stage.operand)  # type: ignore[arg-type]
        st.truncated = st.truncated or sub.truncated
        a, b = set(st.ids), set(sub.ids)
        # All three are SET operations (PR #20 review: union must not
        # double-count overlapping ids).
        if stage.op == "union":
            st.ids = sorted(a | b)
        elif stage.op == "intersect":
            st.ids = sorted(a & b)
        else:
            st.ids = sorted(a - b)

    def _file_path(self, file_id: int) -> Optional[str]:
        if file_id not in self._file_paths:
            self._file_paths[file_id] = self._db.file_abs_path(file_id)
        return self._file_paths[file_id]

    def _fetch_cells(
        self, st: _Stream, fields: Sequence[str]
    ) -> dict[int, tuple[Any, ...]]:
        join = self._join_clause("entity_type" in fields)
        uniq = sorted(set(st.ids))
        cols = "".join(", " + _col_expr(f) for f in fields)
        by_id: dict[int, tuple[Any, ...]] = {}
        for at in range(0, len(uniq), ID_CHUNK):
            chunk = uniq[at:at + ID_CHUNK]
            sql = (
                f"SELECT s.id{cols} FROM symbol s{join} WHERE s.id IN ("
                + ",".join("?" * len(chunk)) + ")"
            )
            for row in self._conn.execute(sql, chunk):
                cells: list[Any] = []
                for i, f in enumerate(fields):
                    raw = row[i + 1]
                    if raw is None:
                        cells.append(None)
                    elif f == "kind":
                        cells.append(SYMBOL_KIND_NAMES.get(raw, str(raw)))
                    elif f == "entity_type":
                        cells.append(_entity_type_name(raw))
                    elif f == "file":
                        cells.append(self._file_path(raw))
                    else:
                        cells.append(raw)
                by_id[row[0]] = tuple(cells)
        return by_id

    def _materialize(self, st: _Stream, fields: Sequence[str]) -> None:
        by_id = self._fetch_cells(st, fields)
        st.fields = tuple(fields)
        st.rows = []
        st.row_ids = []
        for nid in st.ids:
            if nid in by_id:
                st.rows.append(by_id[nid])
                st.row_ids.append(nid)
        st.ids = []

    @staticmethod
    def _apply_distinct(st: _Stream) -> None:
        if st.shape == "nodes":
            st.ids = sorted(set(st.ids))
            return
        rows: list[tuple[Any, ...]] = []
        row_ids: list[int] = []
        seen: set[tuple[Any, ...]] = set()
        for row, rid in zip(st.rows, st.row_ids):
            if row not in seen:
                seen.add(row)
                rows.append(row)
                row_ids.append(rid)
        st.rows = rows
        st.row_ids = row_ids

    def _apply_order(self, st: _Stream, fields: Sequence[str]) -> None:
        if st.shape == "nodes":
            by_id = self._fetch_cells(st, fields)
            st.ids.sort(
                key=lambda nid: (
                    tuple(_cell_key(c) for c in by_id[nid]), nid))
            return
        pos = [st.fields.index(f) for f in fields]
        order = sorted(
            range(len(st.rows)),
            key=lambda i: (
                tuple(_cell_key(st.rows[i][p]) for p in pos), st.row_ids[i]))
        st.rows = [st.rows[i] for i in order]
        st.row_ids = [st.row_ids[i] for i in order]

    @staticmethod
    def _apply_limit(st: _Stream, n: int) -> None:
        st.limit_in_effect = True
        if st.shape == "nodes":
            del st.ids[n:]
        else:
            del st.rows[n:]
            del st.row_ids[n:]

    # -- finish --------------------------------------------------------------------

    def _finish(self, st: _Stream) -> Result:
        if st.shape == "scalar":
            return Result(
                shape="scalar", view=st.view, truncated=st.truncated,
                scalar=len(st.rows) if st.rows else len(st.ids))
        if st.shape == "nodes":
            self._materialize(st, ("id", "usr", "name", "kind"))
        if not st.limit_in_effect and len(st.rows) > DEFAULT_RESULT_CAP:
            del st.rows[DEFAULT_RESULT_CAP:]
            del st.row_ids[DEFAULT_RESULT_CAP:]
            st.truncated = True
        return Result(
            shape=st.shape, view=st.view, truncated=st.truncated,
            fields=st.fields, rows=st.rows)
