"""CXQ QueryPlan tier: IR, relation catalog, pipeline builder, SQLite executor.

Contract: docs/query-plan.md (v1). This is the Python twin of C++
``src/query/plan.{hpp,cpp}`` + ``src/query/exec.cpp``: both builders produce
the same immutable plan tree, ``canonical_json()`` must be byte-identical to
the C++ side (pinned by the shared golden ``tests/golden/cxq_plans.txt``), and
the executor mirrors the C++ SQL shapes so results match by construction.
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, field, replace
from enum import Enum
from typing import Any, NoReturn, Optional, Sequence

from .storage import (
    IndexIdentity, SYMBOL_KIND_IDS, SYMBOL_KIND_NAMES, SYMBOL_KINDS, Storage,
)
from .generated_catalog import (
    CatalogView as _CatalogView,
    ENTITY_KIND_NAMES as _GENERATED_ENTITY_KIND_NAMES,
    FIELD_CATALOG as _GENERATED_FIELD_CATALOG,
    RELATION_CATALOG as _GENERATED_RELATION_CATALOG,
    RELATION_METADATA as _GENERATED_RELATION_METADATA,
)
from .generated_extensions import EXTENSION_RELATIONS as _GENERATED_EXTENSION_RELATIONS

__all__ = [
    "PlanError", "TraversalMode", "UnknownPolicy", "Pred", "TargetSet", "Stage", "Source", "Plan", "Query", "Result",
    "Executor", "start", "codebase", "symbol", "entity",
    "parse_cxq",
    "all_of", "any_of", "not_", "eq", "ne", "glob", "in_list",
    "exists", "none", "all", "at_least", "exactly", "any_target", "all_targets", "no_targets",
    "inherits_from", "implements", "has_ancestor", "has_member", "has_method", "has_field", "has_nested",
    "has_template_arg", "is_specialization_of", "is_instantiation_of", "calls", "called_by", "uses", "used_by",
    "is_abstract", "is_interface", "is_pure", "is_static", "is_template", "is_instance",
    "nodes", "view", "where", "out", "in_", "sites", "union_", "intersect", "except_",
    "select", "count", "distinct", "order_by", "limit",
    "validate", "canonical_json", "relation_catalog", "relation_metadata", "resolve_relation",
    "extension_relation_catalog", "extension_relation_metadata",
]


def _component_owner(repository: str, remote_url: str, semantic_universe: str) -> str:
    if remote_url:
        return f"remote:{remote_url}"
    if repository:
        return f"repo:{repository}"
    return f"universe:{semantic_universe or 'legacy'}"

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


class UnknownPolicy(str, Enum):
    EXCLUDE = "exclude"
    INCLUDE = "include"
    ERROR = "error"


# ---- Views --------------------------------------------------------------------

SYMBOL_VIEW = "symbol"
ENTITY_VIEW = "entity"
LOGICAL_VIEWS = tuple(view.value for view in _CatalogView)

# Generated relation and entity-kind identifiers are the shared query contract.
RELATION_CATALOG = tuple(_GENERATED_RELATION_CATALOG)
RELATION_METADATA = dict(_GENERATED_RELATION_METADATA)
EXTENSION_RELATIONS = dict(_GENERATED_EXTENSION_RELATIONS)
ENTITY_KIND_NAMES = tuple(_GENERATED_ENTITY_KIND_NAMES)


def relation_catalog() -> tuple[tuple[str, str, int], ...]:
    return RELATION_CATALOG


def relation_metadata(name: str, active: str) -> Optional[dict[str, str]]:
    relation = resolve_relation(name, active)
    if relation is None:
        return None
    return RELATION_METADATA.get(relation)


def extension_relation_catalog() -> tuple[tuple[str, dict[str, str]], ...]:
    return tuple(EXTENSION_RELATIONS.items())


def extension_relation_metadata(name: str) -> Optional[dict[str, str]]:
    return EXTENSION_RELATIONS.get(name)


def _relation_view(domain: str) -> str:
    return domain.split(".", 1)[0]


def resolve_relation(name: str, active: str, inbound: bool = False) -> Optional[tuple[str, str, int]]:
    """Resolve a bare or view-qualified relation name at either endpoint."""
    bare, layer = name, active
    forced: Optional[str] = None
    for candidate in LOGICAL_VIEWS:
        prefix = candidate + "."
        if name.startswith(prefix):
            bare, layer, forced = name[len(prefix):], candidate, candidate
            break
    for row in RELATION_CATALOG:
        metadata = RELATION_METADATA.get(row, {})
        source = row[1]
        target = _relation_view(metadata.get("target", row[1]))
        if forced:
            matches = source == forced and (not inbound or target == active)
        else:
            matches = target == active if inbound else source == active
        if row[0] == bare and matches:
            return row
    return None


# ---- Field catalog ---------------------------------------------------------------
# name -> (filterable, is_string). `kind` is ALWAYS the C++ declaration kind
# (symbol_kind names); `entity_type` is ALWAYS the Layer-1 classification
# (entity_kind names, null for non-entities) -- separate fields so
# `kind in [class, struct]` keeps its declaration-kind meaning (PR #20 review).

_FIELDS = {name: (filterable, is_string) for name, filterable, is_string in _GENERATED_FIELD_CATALOG}

_TYPED_FIELDS = {
    "parameter": {"id", "identity_key", "owner_id", "position", "pack_index", "name", "type_id", "declared_type_id", "adjusted_type_id", "default_text", "default_origin", "reference_semantics", "file_id", "line", "col"},
    "template_parameter": {"id", "identity_key", "owner_id", "position", "param_kind", "name", "default_txt", "type_id", "default_type_id", "default_ref_id"},
    "template_argument": {"id", "identity_key", "owner_id", "position", "pack_index", "arg_kind", "ref_id", "literal", "type_id"},
    "call_argument": {"id", "identity_key", "edge_id", "file_id", "line", "col", "position", "src_kind", "type_usr", "decl_usr", "callee_usr", "type_id", "decl_id", "callee_id", "type_is_value"},
    "edge": {"id", "identity_key", "src_id", "dst_id", "kind", "count", "base_access", "is_virtual", "vtable_slot", "relation", "source", "target", "evidence", "status", "partial", "unknown"},
    "site": {"id", "identity_key", "edge_id", "file_id", "file", "line", "col", "relation", "source", "target", "evidence", "status", "partial", "unknown"},
    "evidence": {"id", "identity_key", "owner_id", "position", "default_txt", "default_type_id", "default_ref_id", "edge_id", "file_id", "line", "col", "conditional", "args_sig", "recv_src_kind", "recv_type_usr", "recv_decl_usr", "recv_type_id", "recv_decl_id", "recv_param_pos", "recv_type_is_value", "relation", "source", "target", "evidence", "status", "partial", "unknown"},
    "type": {"id", "identity_key", "type_key", "spelling", "kind", "is_const", "is_volatile", "is_restrict", "cv_qualifiers", "decl_usr", "decl_id", "canonical_id"},
}


def _field_available(view_name: str, field_name: str) -> bool:
    if view_name in (SYMBOL_VIEW, ENTITY_VIEW):
        return field_name in _FIELDS
    return field_name in _TYPED_FIELDS.get(view_name, set())

# ---- Predicates ---------------------------------------------------------------


@dataclass(frozen=True)
class Pred:
    op: str  # all_of | any_of | not | eq | ne | glob | in
    kids: tuple["Pred", ...] = ()
    field: str = ""
    str_values: tuple[str, ...] = ()
    int_value: Optional[int] = None
    relation: str = ""
    target: Optional["Pred"] = None
    min_depth: int = 1
    max_depth: int = 1
    threshold: int = 0
    inbound: bool = False


@dataclass(frozen=True)
class TargetSet:
    kind: str
    refs: tuple[str, ...]


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


def _relation_pred(op: str, relation: str, target: Optional[Pred], min_depth: int,
                   max_depth: int, inbound: bool, threshold: int = 0) -> Pred:
    return Pred(op=op, relation=relation, target=target, min_depth=min_depth,
                max_depth=max_depth, inbound=inbound, threshold=threshold)


def exists(relation: str, target: Optional[Pred] = None, min_depth: int = 1,
           max_depth: int = 1, inbound: bool = False) -> Pred:
    return _relation_pred("exists", relation, target, min_depth, max_depth, inbound)


def none(relation: str, target: Optional[Pred] = None, min_depth: int = 1,
         max_depth: int = 1, inbound: bool = False) -> Pred:
    return _relation_pred("none", relation, target, min_depth, max_depth, inbound)


def all(relation: str, target: Optional[Pred] = None, min_depth: int = 1,
        max_depth: int = 1, inbound: bool = False) -> Pred:
    return _relation_pred("all", relation, target, min_depth, max_depth, inbound)


def at_least(threshold: int, relation: str, target: Optional[Pred] = None,
             min_depth: int = 1, max_depth: int = 1,
             inbound: bool = False) -> Pred:
    return _relation_pred("at_least", relation, target, min_depth, max_depth,
                          inbound, threshold)


def exactly(threshold: int, relation: str, target: Optional[Pred] = None,
            min_depth: int = 1, max_depth: int = 1,
            inbound: bool = False) -> Pred:
    return _relation_pred("exactly", relation, target, min_depth, max_depth,
                          inbound, threshold)


def any_target(refs: Sequence[str]) -> TargetSet:
    return TargetSet("any", tuple(refs))


def all_targets(refs: Sequence[str]) -> TargetSet:
    return TargetSet("all", tuple(refs))


def no_targets(refs: Sequence[str]) -> TargetSet:
    return TargetSet("none", tuple(refs))


def _target_ref(ref: str) -> Pred:
    return any_of([eq("usr", ref), eq("qual_name", ref), eq("spelling", ref)])


def _target_set_pred(relation: str, targets: TargetSet, inbound: bool = False,
                     max_depth: int = 1) -> Pred:
    if not targets.refs:
        return any_of([]) if targets.kind == "any" else all_of([])
    parts = [exists(relation, _target_ref(ref), 1, max_depth, inbound)
             for ref in targets.refs]
    if targets.kind == "any":
        return parts[0] if len(parts) == 1 else any_of(parts)
    if targets.kind == "all":
        return parts[0] if len(parts) == 1 else all_of(parts)
    return none(relation, any_of([_target_ref(ref) for ref in targets.refs]),
                1, max_depth, inbound)


def inherits_from(target: str | TargetSet, transitive: bool = False) -> Pred:
    if isinstance(target, TargetSet):
        return _target_set_pred("inherits", target, max_depth=32 if transitive else 1)
    return exists("inherits", _target_ref(target), 1, 32 if transitive else 1)


def implements(target: str | TargetSet) -> Pred:
    if isinstance(target, TargetSet):
        return _target_set_pred("implements", target)
    return exists("implements", _target_ref(target))


def has_ancestor(target: str, transitive: bool = True) -> Pred:
    return inherits_from(target, transitive)


def has_member(target: Optional[Pred] = None) -> Pred:
    return exists("field_of", target, inbound=True)


def has_method(target: Optional[Pred] = None) -> Pred:
    return exists("method_of", target, inbound=True)


def has_field(target: Optional[Pred] = None) -> Pred:
    return has_member(target)


def has_nested(target: Optional[Pred] = None) -> Pred:
    return exists("contains", target)


def has_template_arg(target: Optional[Pred] = None) -> Pred:
    return exists("instantiates", target)


def is_specialization_of(target: str) -> Pred:
    return exists("specializes", _target_ref(target))


def is_instantiation_of(target: str) -> Pred:
    return exists("instantiates", _target_ref(target))


def calls(target: Optional[Pred] = None) -> Pred:
    return exists("calls", target)


def called_by(target: Optional[Pred] = None) -> Pred:
    return exists("calls", target, inbound=True)


def uses(target: Optional[Pred] = None) -> Pred:
    return exists("uses", target)


def used_by(target: Optional[Pred] = None) -> Pred:
    return exists("uses", target, inbound=True)


def is_abstract() -> Pred:
    return in_list("entity_type", ["abstract_class", "abstract_class_template"])


def is_interface() -> Pred:
    return in_list("entity_type", ["interface", "interface_template"])


def is_pure() -> Pred:
    return eq("is_pure", True)


def is_static() -> Pred:
    return eq("is_static", True)


def is_template() -> Pred:
    return in_list("kind", ["class-template", "function-template"])


def is_instance() -> Pred:
    return exists("instantiates")

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
    unknown: UnknownPolicy = UnknownPolicy.EXCLUDE


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


def nodes(pred: Optional[Pred] = None,
          unknown: UnknownPolicy = UnknownPolicy.EXCLUDE) -> Stage:
    return Stage(op="nodes", pred=pred, unknown=unknown)


def view(level: str) -> Stage:
    return Stage(op="view", level=level)


def where(pred: Pred,
          unknown: UnknownPolicy = UnknownPolicy.EXCLUDE) -> Stage:
    return Stage(op="where", pred=pred, unknown=unknown)


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


def sites() -> Stage:
    return Stage(op="sites")


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


# ---- Textual CXQ -------------------------------------------------------------

def _cxq_trim(value: str) -> str:
    return value.strip()


def _cxq_atom(value: str) -> str:
    text = _cxq_trim(value)
    if len(text) >= 2 and text[0] == text[-1] and text[0] in "'\"":
        return text[1:-1]
    return text


def _cxq_split(value: str, delimiter: str) -> list[str]:
    parts: list[str] = []
    begin = 0
    parens = brackets = 0
    quote = ""
    escaped = False
    for i, char in enumerate(value):
        if quote:
            if char == quote and not escaped:
                quote = ""
            escaped = char == "\\" and not escaped
            continue
        if char in "'\"":
            quote = char
        elif char == "(":
            parens += 1
        elif char == ")":
            parens -= 1
        elif char == "[":
            brackets += 1
        elif char == "]":
            brackets -= 1
        elif char == delimiter and parens == 0 and brackets == 0:
            parts.append(_cxq_trim(value[begin:i]))
            begin = i + 1
    if quote or parens or brackets:
        _fail("E_PARSE", "unbalanced quotes or delimiters")
    parts.append(_cxq_trim(value[begin:]))
    return parts


def _cxq_call(token: str, name: str) -> list[str]:
    value = _cxq_trim(token)
    prefix = f"{name}("
    if not value.startswith(prefix) or not value.endswith(")"):
        _fail("E_PARSE", f"expected {name}(...)")
    inside = value[len(prefix):-1]
    return [] if not _cxq_trim(inside) else _cxq_split(inside, ",")


def _cxq_find_keyword(value: str, keyword: str) -> Optional[int]:
    parens = brackets = 0
    quote = ""
    escaped = False
    for i, char in enumerate(value):
        if quote:
            if char == quote and not escaped:
                quote = ""
            escaped = char == "\\" and not escaped
            continue
        if char in "'\"":
            quote = char
            continue
        if char == "(":
            parens += 1
            continue
        if char == ")":
            parens -= 1
            continue
        if char == "[":
            brackets += 1
            continue
        if char == "]":
            brackets -= 1
            continue
        end = i + len(keyword)
        if (parens == 0 and brackets == 0 and value[i:end] == keyword and
                (i == 0 or value[i - 1].isspace()) and
                (end == len(value) or value[end].isspace())):
            return i
    return None


def _cxq_int(value: str) -> Optional[int]:
    text = _cxq_trim(value)
    digits = text[1:] if text.startswith("-") else text
    if not digits or any(char < "0" or char > "9" for char in digits):
        return None
    try:
        number = int(text)
    except ValueError:
        return None
    if number < -(1 << 63) or number > (1 << 63) - 1:
        return None
    return number


def _cxq_list(value: str) -> list[str]:
    text = _cxq_trim(value)
    if len(text) < 2 or text[0] != "[" or text[-1] != "]":
        _fail("E_PARSE", "expected a bracketed value list")
    inside = _cxq_trim(text[1:-1])
    if not inside:
        _fail("E_PARSE", "value list must not be empty")
    return [_cxq_atom(part) for part in _cxq_split(inside, ",")]


def _cxq_pred(expression: str) -> Pred:
    text = _cxq_trim(expression)
    while text.startswith("(") and text.endswith(")"):
        inner = text[1:-1]
        if len(_cxq_split(inner, ",")) != 1:
            break
        text = _cxq_trim(inner)
    if text.startswith("not "):
        return not_(_cxq_pred(text[4:]))
    for keyword in ("or", "and"):
        at = _cxq_find_keyword(text, keyword)
        if at is not None:
            children = [_cxq_pred(text[:at]), _cxq_pred(text[at + len(keyword):])]
            return any_of(children) if keyword == "or" else all_of(children)
    for name in ("eq", "ne", "glob", "in"):
        if text.startswith(f"{name}("):
            args = _cxq_call(text, name)
            if name == "in":
                if len(args) != 2:
                    _fail("E_PARSE", "in() requires a field and list")
                return in_list(_cxq_atom(args[0]), _cxq_list(args[1]))
            if len(args) != 2:
                _fail("E_PARSE", f"{name}() requires a field and value")
            field_name, value = _cxq_atom(args[0]), _cxq_atom(args[1])
            if name == "glob":
                return glob(field_name, value)
            if value in ("true", "false"):
                pred = eq(field_name, value == "true")
            elif (number := _cxq_int(value)) is not None:
                pred = eq(field_name, number)
            else:
                pred = eq(field_name, value)
            return not_(pred) if name == "ne" else pred
    for operator in ("!=", "~=", "=", " in "):
        at = text.find(operator)
        if at == -1:
            continue
        field_name = _cxq_trim(text[:at])
        value = _cxq_trim(text[at + len(operator):])
        if not field_name:
            _fail("E_PARSE", "missing predicate field")
        if operator == " in ":
            return in_list(field_name, _cxq_list(value))
        rhs = _cxq_atom(value)
        if operator == "~=":
            return glob(field_name, rhs)
        if rhs in ("true", "false"):
            pred = eq(field_name, rhs == "true")
        elif (number := _cxq_int(rhs)) is not None:
            pred = eq(field_name, number)
        else:
            pred = eq(field_name, rhs)
        return not_(pred) if operator == "!=" else pred
    _fail("E_PARSE", f"unsupported predicate '{text}'")


def _cxq_stage(token: str) -> Stage:
    value = _cxq_trim(token)
    if value.startswith("rank("):
        _fail("E_PARSE", "rank() is not available in v1")
    names = ("nodes", "view", "where", "out", "in", "sites", "union", "intersect",
             "except", "select", "count", "distinct", "order_by",
             "limit")
    for name in names:
        if not value.startswith(f"{name}("):
            continue
        args = _cxq_call(value, name)
        if name == "nodes":
            if len(args) > 1:
                _fail("E_PARSE", "nodes() takes zero or one predicate")
            return nodes() if not args else nodes(_cxq_pred(args[0]))
        if name == "view":
            if len(args) != 1 or _cxq_atom(args[0]) not in (SYMBOL_VIEW, ENTITY_VIEW):
                _fail("E_PARSE", "view() requires symbol or entity")
            return view(_cxq_atom(args[0]))
        if name == "where":
            if len(args) != 1:
                _fail("E_PARSE", "where() requires one expression")
            return where(_cxq_pred(args[0]))
        if name == "sites":
            if args:
                _fail("E_PARSE", "sites() takes no arguments")
            return sites()
        if name in ("out", "in"):
            if not args:
                _fail("E_PARSE", f"{name}() requires a relation")
            if len(args) > 3:
                _fail("E_PARSE", "traversal accepts relation and at most two depth arguments")
            min_depth = max_depth = 1
            if len(args) == 2 and args[1].startswith("depth="):
                bounds = args[1][6:].split("..")
                if len(bounds) != 2 or any(_cxq_int(x) is None for x in bounds):
                    _fail("E_PARSE", "depth must be written as depth=min..max")
                min_depth, max_depth = int(bounds[0]), int(bounds[1])
            elif len(args) == 2:
                depth = _cxq_int(args[1])
                if depth is None:
                    _fail("E_PARSE", "depth must be an integer or depth=min..max")
                min_depth = max_depth = depth
            elif len(args) == 3:
                first = _cxq_int(args[1])
                second = _cxq_int(args[2])
                if first is None or second is None:
                    _fail("E_PARSE", "depth must be written as depth=min..max")
                min_depth, max_depth = first, second
            return (out if name == "out" else in_)(_cxq_atom(args[0]), min_depth, max_depth)
        if name in ("select", "order_by"):
            fields = [_cxq_atom(arg) for arg in args]
            if not fields:
                _fail("E_PARSE", f"{name}() requires fields")
            return select(fields) if name == "select" else order_by(fields)
        if name == "count":
            if args:
                _fail("E_PARSE", "count() takes no arguments")
            return count()
        if name == "distinct":
            if args:
                _fail("E_PARSE", "distinct() takes no arguments")
            return distinct()
        if name == "limit":
            if len(args) != 1 or (number := _cxq_int(args[0])) is None:
                _fail("E_PARSE", "limit() requires one integer")
            return limit(number)
        if name in ("union", "intersect", "except"):
            if len(args) != 1:
                _fail("E_PARSE", f"{name}() requires one query")
            nested = parse_cxq(args[0])
            nested_query = Query(nested.source)
            for nested_stage in nested.stages:
                nested_query = nested_query | nested_stage
            return {"union": union_, "intersect": intersect,
                    "except": except_}[name](nested_query)
    _fail("E_PARSE", f"unknown stage '{value}'")


def parse_cxq(text: str) -> Plan:
    """Parse the dependency-free v1 textual CXQ subset into a QueryPlan."""
    parts = _cxq_split(text, "|")
    if not parts or not parts[0]:
        _fail("E_PARSE", "query is empty")
    first = _cxq_trim(parts[0])
    if first.startswith("codebase("):
        args = _cxq_call(first, "codebase")
        if args:
            _fail("E_PARSE", "codebase() takes no arguments")
        query = Query(codebase())
    elif first.startswith("symbol("):
        args = _cxq_call(first, "symbol")
        if len(args) != 1:
            _fail("E_PARSE", "symbol() requires one reference")
        query = Query(symbol(_cxq_atom(args[0])))
    elif first.startswith("entity("):
        args = _cxq_call(first, "entity")
        if len(args) != 1:
            _fail("E_PARSE", "entity() requires one reference")
        query = Query(entity(_cxq_atom(args[0])))
    else:
        _fail("E_PARSE", "query must start with codebase(), symbol(), or entity()")
    for part in parts[1:]:
        query = query | _cxq_stage(part)
    return query.plan


# ---- Validation / normalization -----------------------------------------------------


def _check_cmp(p: Pred, active: str) -> None:
    if not _field_available(active, p.field):
        _fail("E_FIELD", f"field '{p.field}' is unavailable in {active} view")
    if active not in (SYMBOL_VIEW, ENTITY_VIEW):
        if p.op == "glob":
            _fail("E_FIELD", "glob predicates are not supported for typed views")
        typed_strings = {
            "identity_key", "name", "spelling", "type_key", "default_text",
            "default_origin", "default_txt", "reference_semantics", "literal",
            "src_kind", "type_usr", "decl_usr", "callee_usr", "args_sig",
            "recv_src_kind", "recv_type_usr", "recv_decl_usr",
        }
        is_string = p.field in typed_strings
        if is_string:
            if p.int_value is not None:
                _fail("E_FIELD", f"field '{p.field}' takes a string value")
            bad_arity = (len(p.str_values) == 0 if p.op == "in"
                         else len(p.str_values) != 1)
        else:
            if p.op == "in":
                _fail("E_FIELD", f"field '{p.field}' supports eq/ne only")
            if p.int_value is None:
                _fail("E_FIELD", f"field '{p.field}' takes an integer value")
            bad_arity = False
        if bad_arity:
            _fail("E_FIELD", f"bad value arity for field '{p.field}'")
        return
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


def _norm_pred(p: Pred, active: str) -> Pred:
    if p.op in ("all_of", "any_of"):
        if not p.kids:
            _fail("E_FIELD", "empty boolean combinator")
        kids: list[Pred] = []
        for k in p.kids:
            nk = _norm_pred(k, active)
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
        nk = _norm_pred(p.kids[0], active)
        if nk.op == "not":
            return nk.kids[0]  # not(not(p)) -> p
        return Pred(op="not", kids=(nk,))
    if p.op in ("eq", "ne", "glob", "in"):
        _check_cmp(p, active)
        return p
    if p.op in ("exists", "none", "all", "at_least", "exactly"):
        rel = resolve_relation(p.relation, active)
        if rel is None:
            _fail("E_RELATION", f"unknown relation '{p.relation}' in {active} view")
        if not 1 <= p.min_depth <= p.max_depth <= 32:
            _fail("E_DEPTH", "depth bounds must satisfy 1 <= min <= max <= 32")
        if p.op in ("at_least", "exactly") and p.threshold < 0:
            _fail("E_LIMIT", "quantifier threshold must be >= 0")
        target = (_norm_pred(p.target, rel[1]) if p.target is not None else None)
        return replace(p, relation=f"{rel[1]}.{rel[0]}", target=target)
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
                ns = replace(stage, pred=_norm_pred(stage.pred, st.active))
            st.codebase_unenumerated = False
        elif stage.op == "view":
            if st.shape != "nodes":
                _fail("E_STAGE", "view() applies to a node stream")
            if stage.level not in LOGICAL_VIEWS:
                _fail("E_VIEW", f"unknown view '{stage.level}'")
            logical_transition = (
                st.active in (SYMBOL_VIEW, ENTITY_VIEW)
                and stage.level in (SYMBOL_VIEW, ENTITY_VIEW)
            )
            if (not st.codebase_unenumerated and stage.level != st.active
                    and not logical_transition):
                _fail("E_VIEW", f"cannot change view from {st.active} to {stage.level}")
            st.active = stage.level
        elif stage.op == "where":
            if st.shape != "nodes":
                _fail("E_STAGE", "where() applies to a node stream")
            if stage.pred is None:
                _fail("E_FIELD", "where() requires a predicate")
            consume()
            ns = replace(stage, pred=_norm_pred(stage.pred, st.active))
        elif stage.op in ("out", "in"):
            consume()
            if st.shape != "nodes":
                _fail("E_STAGE", "traversal applies to a node stream")
            inbound = stage.op == "in"
            rel = resolve_relation(stage.relation, st.active, inbound)
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
            metadata = RELATION_METADATA.get(rel, {})
            typed = bool(metadata.get("virtual")) or st.active not in (SYMBOL_VIEW, ENTITY_VIEW) or _relation_view(metadata.get("target", rel[1])) not in (SYMBOL_VIEW, ENTITY_VIEW)
            if typed and (stage.min_depth != 1 or stage.max_depth != 1):
                _fail("E_DEPTH", "typed traversal currently supports only depth 1..1")
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
            st.active = (_relation_view(metadata.get("source", rel[1]))
                         if inbound else _relation_view(metadata.get("target", rel[1])))
        elif stage.op == "sites":
            consume()
            if st.shape != "nodes":
                _fail("E_STAGE", "sites() applies to a node stream")
            if st.active != "edge":
                _fail("E_VIEW", "sites() requires an edge node stream")
            st.active = "site"
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
                if not _field_available(st.active, f):
                    _fail("E_FIELD", f"field '{f}' is unavailable in {st.active} view")
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
                if not _field_available(st.active, f):
                    _fail("E_FIELD", f"field '{f}' is unavailable in {st.active} view")
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
    if p.op in ("exists", "none", "all", "at_least", "exactly"):
        out: dict[str, Any] = {"op": p.op, "relation": p.relation}
        if p.inbound:
            out["direction"] = "in"
        out["min_depth"] = p.min_depth
        out["max_depth"] = p.max_depth
        if p.op in ("at_least", "exactly"):
            out["threshold"] = p.threshold
        if p.target is not None:
            out["pred"] = _pred_to_dict(p.target)
        return out
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
            if s.unknown != UnknownPolicy.EXCLUDE:
                o["unknown"] = s.unknown.value
        elif s.op == "view":
            o["level"] = s.level
        elif s.op == "where":
            o["pred"] = _pred_to_dict(s.pred)  # type: ignore[arg-type]
            if s.unknown != UnknownPolicy.EXCLUDE:
                o["unknown"] = s.unknown.value
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
    partial: bool = False
    unknown: bool = False
    scalar: int = 0
    fields: tuple[str, ...] = ()
    rows: list[tuple[Any, ...]] = field(default_factory=list)
    index: IndexIdentity | None = None

    def to_dict(self) -> dict[str, Any]:
        if self.shape == "scalar":
            return {"shape": "scalar", "view": self.view, "count": self.scalar,
                    "truncated": self.truncated,
                    "index": self.index.to_dict() if self.index else None}
        return {
            "shape": self.shape,
            "view": self.view,
            "count": len(self.rows),
            "truncated": self.truncated,
            "index": self.index.to_dict() if self.index else None,
            "rows": [dict(zip(self.fields, row)) for row in self.rows],
        }

    def to_envelope_dict(self) -> dict[str, Any]:
        """Return the versioned HSE-70 envelope for this query result."""
        from .result_protocol import from_query_result

        if self.index is None:
            raise PlanError("E_RESULT: result has no index identity")
        return from_query_result(self, self.index).to_dict()


def _col_expr(field_name: str, symbol_alias: str = "s",
              entity_alias: str = "en") -> str:
    if field_name == "id":
        return f"{symbol_alias}.id"
    if field_name == "usr":
        return f"{symbol_alias}.usr"
    if field_name == "semantic_universe":
        return f"(SELECT su.key FROM semantic_universe su WHERE su.id = {symbol_alias}.semantic_universe_id)"
    if field_name == "identity_key":
        return f"{symbol_alias}.identity_key"
    if field_name == "name":
        return f"COALESCE({symbol_alias}.qual_name, {symbol_alias}.spelling)"
    if field_name == "spelling":
        return f"{symbol_alias}.spelling"
    if field_name == "qual_name":
        return f"{symbol_alias}.qual_name"
    if field_name == "kind":
        return f"{symbol_alias}.kind"
    if field_name == "entity_type":
        return f"{entity_alias}.kind"
    if field_name == "is_definition":
        return f"{symbol_alias}.is_definition"
    if field_name == "is_pure":
        return f"{symbol_alias}.is_pure"
    if field_name == "is_static":
        return f"{symbol_alias}.is_static"
    if field_name == "file":
        return f"{symbol_alias}.file_id"
    if field_name == "line":
        return f"{symbol_alias}.line"
    if field_name == "col":
        return f"{symbol_alias}.col"
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


def _next_alias(aliases: list[int], prefix: str) -> str:
    value = f"{prefix}{aliases[0]}"
    aliases[0] += 1
    return value


def _relation_candidates_sql(p: Pred, active: str, args: list[Any],
                             aliases: list[int], outer_alias: str) -> str:
    rel = resolve_relation(p.relation, active)
    if rel is None:
        raise PlanError(f"E_RELATION: unknown relation '{p.relation}'")
    table = "entity_edge" if rel[1] == ENTITY_VIEW else "edge"
    from_col = "dst_id" if p.inbound else "src_id"
    to_col = "src_id" if p.inbound else "dst_id"
    edge_alias = _next_alias(aliases, "qe")
    target_alias = _next_alias(aliases, "qt")
    target_entity_alias = _next_alias(aliases, "qen")
    if p.max_depth > 1 or p.min_depth > 1:
        args.extend([rel[2], rel[2], p.max_depth])
        value = ["1"]
        if p.target is not None:
            value = ["("]
            _pred_sql(p.target, rel[1], value, args, aliases,
                      target_alias, target_entity_alias)
            value.append(")")
        result = (
            "WITH RECURSIVE reach(id, depth) AS (SELECT e."
            + to_col + " , 1 FROM " + table + " e WHERE e.kind = ? AND e."
            + from_col + " = " + outer_alias + ".id UNION ALL SELECT e." + to_col
            + " , reach.depth + 1 FROM " + table + " e JOIN reach ON e."
            + from_col + " = reach.id WHERE e.kind = ? AND reach.depth < ?) "
            "SELECT DISTINCT " + target_alias + ".id, " + "".join(value)
            + " AS value FROM reach JOIN symbol " + target_alias + " ON "
            + target_alias + ".id = reach.id LEFT JOIN entity_node "
            + target_entity_alias + " ON " + target_entity_alias + ".id = "
            + target_alias + ".id WHERE reach.depth BETWEEN ? AND ?"
        )
        args.extend([p.min_depth, p.max_depth])
        return result
    else:
        value = ["1"]
        if p.target is not None:
            value = ["("]
            _pred_sql(p.target, rel[1], value, args, aliases,
                      target_alias, target_entity_alias)
            value.append(")")
        args.append(rel[2])
        return (
            "SELECT DISTINCT " + target_alias + ".id, " + "".join(value)
            + " AS value FROM " + table + " " + edge_alias + " JOIN symbol "
            + target_alias + " ON " + target_alias + ".id = " + edge_alias
            + "." + to_col + " LEFT JOIN entity_node " + target_entity_alias
            + " ON " + target_entity_alias + ".id = " + target_alias
            + ".id WHERE " + edge_alias + ".kind = ? AND " + edge_alias
            + "." + from_col + " = " + outer_alias + ".id"
        )


def _relation_count_sql(p: Pred, active: str, args: list[Any],
                        aliases: list[int], truth: str,
                        outer_alias: str) -> str:
    candidates = _relation_candidates_sql(p, active, args, aliases, outer_alias)
    return "(SELECT COUNT(*) FROM (" + candidates + ") AS " + _next_alias(aliases, "rows") + " WHERE value IS " + truth + ")"


def _pred_sql(p: Pred, active: str, sql: list[str], args: list[Any],
              aliases: list[int], symbol_alias: str = "s",
              entity_alias: str = "en") -> None:
    if p.op in ("all_of", "any_of"):
        joiner = " AND " if p.op == "all_of" else " OR "
        sql.append("(")
        for i, k in enumerate(p.kids):
            if i:
                sql.append(joiner)
            _pred_sql(k, active, sql, args, aliases, symbol_alias, entity_alias)
        sql.append(")")
        return
    if p.op == "not":
        sql.append("NOT (")
        _pred_sql(p.kids[0], active, sql, args, aliases, symbol_alias, entity_alias)
        sql.append(")")
        return
    if p.op in ("exists", "none", "all", "at_least", "exactly"):
        rel = resolve_relation(p.relation, active)
        if rel is None:
            raise PlanError(f"E_RELATION: unknown relation '{p.relation}'")
        complete = RELATION_METADATA[rel]["completeness"] == "complete"
        if p.op in ("exists", "none"):
            true_count = _relation_count_sql(p, active, args, aliases, "TRUE", symbol_alias)
            unknown_count = (
                _relation_count_sql(p, active, args, aliases, "NULL", symbol_alias)
                if p.target is not None else "0"
            )
            sql.append(
                f"CASE WHEN {true_count} > 0 THEN {1 if p.op == 'exists' else 0} "
                f"WHEN {unknown_count} > 0 THEN NULL WHEN {1 if complete else 0} "
                f"THEN {0 if p.op == 'exists' else 1} ELSE NULL END"
            )
            return
        if p.op == "all":
            false_count = _relation_count_sql(p, active, args, aliases, "FALSE", symbol_alias)
            unknown_count = (
                _relation_count_sql(p, active, args, aliases, "NULL", symbol_alias)
                if p.target is not None else "0"
            )
            sql.append(
                f"CASE WHEN {false_count} > 0 THEN 0 WHEN {unknown_count} > 0 "
                f"THEN NULL WHEN {1 if complete else 0} THEN 1 ELSE NULL END"
            )
            return
        true_count = _relation_count_sql(p, active, args, aliases, "TRUE", symbol_alias)
        if p.op == "at_least":
            unknown_count = (
                _relation_count_sql(p, active, args, aliases, "NULL", symbol_alias)
                if p.target is not None else "0"
            )
            sql.append(
                f"CASE WHEN {true_count} >= {p.threshold} THEN 1 WHEN {1 if complete else 0} "
                f"AND {unknown_count} = 0 THEN 0 ELSE NULL END"
            )
        else:
            equal_count = _relation_count_sql(p, active, args, aliases, "TRUE", symbol_alias)
            unknown_for_equal = (
                _relation_count_sql(p, active, args, aliases, "NULL", symbol_alias)
                if p.target is not None else "0"
            )
            unknown_for_complete = (
                _relation_count_sql(p, active, args, aliases, "NULL", symbol_alias)
                if p.target is not None else "0"
            )
            sql.append(
                f"CASE WHEN {true_count} > {p.threshold} THEN 0 WHEN {1 if complete else 0} "
                f"AND {equal_count} = {p.threshold} AND {unknown_for_equal} = 0 THEN 1 "
                f"WHEN {1 if complete else 0} AND {unknown_for_complete} = 0 "
                "THEN 0 ELSE NULL END"
            )
        return
    col = _col_expr(p.field, symbol_alias, entity_alias)
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
    return any(_pred_uses_entity_type(k) for k in p.kids) or (
        p.target is not None and _pred_uses_entity_type(p.target))


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
        self.keys: list[tuple[int, ...]] = []
        self.fields: tuple[str, ...] = ()
        self.rows: list[tuple[Any, ...]] = []
        self.row_ids: list[int] = []
        self.truncated = False
        self.partial = False
        self.unknown = False
        # True only while a limit() is in effect with NO cardinality-expanding
        # stage (nodes/out/in/union) after it -- otherwise _finish()
        # re-applies the default result cap (PR #20 review).
        self.limit_in_effect = False


def _is_typed_view(view_name: str) -> bool:
    return view_name not in (SYMBOL_VIEW, ENTITY_VIEW)


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
        result = self._finish(st)
        result.index = self._db.index_identity()
        return result

    def explain(self, plan: Plan) -> dict[str, Any]:
        """Return the normalized plan and the current index identity."""
        normalized = validate(plan)
        return {
            "plan": plan_to_dict(normalized),
            "index": self._db.index_identity().to_dict(),
        }

    # -- plan walk ------------------------------------------------------------

    def _run_plan(self, plan: Plan) -> _Stream:
        st = _Stream()
        st.view = ENTITY_VIEW if plan.source.kind == "entity" else SYMBOL_VIEW
        if plan.source.kind != "codebase":
            st.ids = self._resolve_source(plan.source)
        for stage in plan.stages:
            self._reject_ambiguous_ungrouped(st)
            if stage.op == "nodes":
                self._enumerate(st, stage.pred, stage.unknown)
                st.limit_in_effect = False
            elif stage.op == "view":
                self._change_view(st, stage.level)
            elif stage.op == "where":
                self._filter(st, stage.pred, stage.unknown)  # type: ignore[arg-type]
            elif stage.op in ("out", "in"):
                if stage.mode == TraversalMode.DEVIRTUALIZED.value:
                    self._traverse_devirtualized(st, stage)
                else:
                    self._traverse(st, stage)
                st.limit_in_effect = False
            elif stage.op == "sites":
                self._expand_sites(st)
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
        self._reject_ambiguous_ungrouped(st)
        return st

    # -- stages ----------------------------------------------------------------

    def _expand_sites(self, st: _Stream) -> None:
        sites_rows: list[tuple[int, ...]] = []
        for edge in st.keys:
            rows = self._conn.execute(
                "SELECT edge_id,file_id,COALESCE(line,0),COALESCE(col,0) "
                "FROM edge_site WHERE edge_id=? ORDER BY file_id,line,col",
                (edge[0],),
            )
            sites_rows.extend(tuple(row) for row in rows)
            if len(sites_rows) >= TRAVERSE_NODE_BUDGET:
                st.truncated = True
                break
        st.keys = sorted(set(sites_rows))[:TRAVERSE_NODE_BUDGET]
        st.ids = []
        st.view = "site"

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
        if level not in (SYMBOL_VIEW, ENTITY_VIEW):
            st.ids = []
            st.keys = []
            st.view = level
            return
        if st.view not in (SYMBOL_VIEW, ENTITY_VIEW):
            st.keys = []
        if level == ENTITY_VIEW and st.view != ENTITY_VIEW:
            kept: list[int] = []
            for at in range(0, len(st.ids), ID_CHUNK):
                chunk = st.ids[at:at + ID_CHUNK]
                sql = ("SELECT id FROM entity_node WHERE id IN ("
                       + ",".join("?" * len(chunk)) + ") ORDER BY id")
                kept.extend(r["id"] for r in self._conn.execute(sql, chunk))
            st.ids = sorted(set(kept))
        st.view = level

    @staticmethod
    def _append_unknown_policy(sql: list[str], policy: UnknownPolicy) -> None:
        sql.append(" IS NOT FALSE" if policy == UnknownPolicy.INCLUDE else " IS TRUE")

    def _enumerate(self, st: _Stream, pred: Optional[Pred],
                   unknown: UnknownPolicy) -> None:
        if st.view not in (SYMBOL_VIEW, ENTITY_VIEW):
            queries = {
                "parameter": "SELECT owner_id,position,pack_index FROM parameter ORDER BY owner_id,position,pack_index",
                "template_parameter": "SELECT owner_id,position FROM template_param ORDER BY owner_id,position",
                "template_argument": "SELECT owner_id,position,pack_index FROM template_arg ORDER BY owner_id,position,pack_index",
                "call_argument": "SELECT edge_id,file_id,line,col,position FROM call_arg ORDER BY edge_id,file_id,line,col,position",
                "edge": "SELECT id FROM edge ORDER BY id",
                "site": "SELECT edge_id,file_id,COALESCE(line,0),COALESCE(col,0) FROM edge_site ORDER BY edge_id,file_id,line,col",
                "evidence": "SELECT edge_id,file_id,COALESCE(line,0),COALESCE(col,0) FROM edge_site ORDER BY edge_id,file_id,line,col",
                "type": "SELECT id FROM type_node ORDER BY id",
            }
            st.keys = [tuple(row) for row in self._conn.execute(
                queries[st.view] + " LIMIT ?", (ENUMERATE_BUDGET + 1,))]
            if len(st.keys) > ENUMERATE_BUDGET:
                del st.keys[ENUMERATE_BUDGET:]
                st.truncated = True
            if pred is not None:
                self._filter(st, pred, unknown)
            return
        sql = ["SELECT s.id FROM symbol s"]
        if st.view == ENTITY_VIEW:
            sql.append(" JOIN entity_node en ON en.id = s.id")
        elif pred is not None and _pred_uses_entity_type(pred):
            sql.append(self._join_clause(True))
        args: list[Any] = []
        if pred is not None:
            sql.append(" WHERE ")
            aliases = [0]
            _pred_sql(pred, st.view, sql, args, aliases)
            if unknown == UnknownPolicy.ERROR:
                probe = "".join(sql) + " IS NULL LIMIT 1"
                if self._conn.execute(probe, args).fetchone() is not None:
                    raise PlanError("E_UNKNOWN: predicate evaluation is unknown")
            self._append_unknown_policy(sql, unknown)
        sql.append(" ORDER BY s.id LIMIT ?")
        args.append(ENUMERATE_BUDGET + 1)
        st.ids = [r["id"] for r in self._conn.execute("".join(sql), args)]
        if len(st.ids) > ENUMERATE_BUDGET:
            del st.ids[ENUMERATE_BUDGET:]
            st.truncated = True

    def _filter(self, st: _Stream, pred: Pred, unknown: UnknownPolicy) -> None:
        if _is_typed_view(st.view):
            fields = tuple(self._predicate_fields(pred))
            by_key = self._fetch_typed_cells(st, fields)
            st.keys = [
                key for key in st.keys
                if key in by_key
                if self._predicate_matches(pred, dict(zip(fields, by_key[key])))
            ]
            return
        out_ids: list[int] = []
        join = self._join_clause(_pred_uses_entity_type(pred))
        for at in range(0, len(st.ids), ID_CHUNK):
            chunk = st.ids[at:at + ID_CHUNK]
            sql = [
                f"SELECT s.id FROM symbol s{join} WHERE s.id IN ("
                + ",".join("?" * len(chunk)) + ") AND ("
            ]
            args: list[Any] = list(chunk)
            aliases = [0]
            _pred_sql(pred, st.view, sql, args, aliases)
            sql.append(")")
            if unknown == UnknownPolicy.ERROR:
                probe = "".join(sql) + " IS NULL LIMIT 1"
                if self._conn.execute(probe, args).fetchone() is not None:
                    raise PlanError("E_UNKNOWN: predicate evaluation is unknown")
            self._append_unknown_policy(sql, unknown)
            sql.append(" ORDER BY s.id")
            out_ids.extend(
                r["id"] for r in self._conn.execute("".join(sql), args))
        st.ids = sorted(set(out_ids))

    @staticmethod
    def _predicate_fields(pred: Pred) -> set[str]:
        if pred.op in ("all_of", "any_of", "not"):
            return set().union(*(Executor._predicate_fields(k) for k in pred.kids))
        return {pred.field}

    @staticmethod
    def _predicate_matches(pred: Pred, values: dict[str, Any]) -> bool:
        if pred.op == "all_of":
            return all(Executor._predicate_matches(k, values) for k in pred.kids)
        if pred.op == "any_of":
            return any(Executor._predicate_matches(k, values) for k in pred.kids)
        if pred.op == "not":
            return not Executor._predicate_matches(pred.kids[0], values)
        value = values.get(pred.field)
        if pred.int_value is not None:
            expected: Any = pred.int_value
        elif pred.str_values:
            expected = pred.str_values[0]
        else:
            expected = None
        if pred.op == "eq":
            return value == expected
        if pred.op == "ne":
            return value != expected
        if pred.op == "in":
            return value in pred.str_values
        return False

    def _traverse(self, st: _Stream, stage: Stage) -> None:
        """Path-length-window BFS (PR #20 review): a node is emitted iff SOME
        path of length d in [min_depth, max_depth] reaches it -- not only its
        shortest first-discovery depth. No cross-level visited set;
        termination comes from the finite max_depth (<= 32) and the state
        budget (cumulative level sizes). The stream view follows the
        relation's layer."""
        inbound = stage.op == "in"
        rel = resolve_relation(stage.relation, st.view, inbound)
        assert rel is not None  # validated
        metadata = RELATION_METADATA.get(rel, {})
        target_view = (_relation_view(metadata.get("source", rel[1]))
                       if inbound else _relation_view(metadata.get("target", rel[1])))
        if metadata.get("virtual") or target_view not in (SYMBOL_VIEW, ENTITY_VIEW) \
                or st.view not in (SYMBOL_VIEW, ENTITY_VIEW):
            self._traverse_typed(st, stage, rel, metadata, inbound)
            return
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
        st.view = target_view

    def _portable_symbol(self, symbol_id: int) -> str:
        row = self._conn.execute(
            "SELECT COALESCE(su.key,''), s.identity_key, s.usr "
            "FROM symbol s LEFT JOIN semantic_universe su "
            "ON su.id=s.semantic_universe_id WHERE s.id=?", (symbol_id,)
        ).fetchone()
        if row is None:
            return f"missing-symbol:{symbol_id}"
        return row[1] or f"{row[0]}\x1f{row[2]}"

    def _ambiguous_ungrouped_file(self, file_id: int) -> bool:
        row = self._conn.execute(
            "SELECT c.name,c.path,r.name,r.remote_url,su.key "
            "FROM file f JOIN directory d ON d.id=f.directory_id "
            "JOIN component c ON c.id=d.component_id "
            "LEFT JOIN repository r ON r.id=c.repository_id "
            "LEFT JOIN semantic_universe su ON "
            "su.id=COALESCE(c.semantic_universe_id,r.semantic_universe_id,1) "
            "WHERE f.id=?", (file_id,)
        ).fetchone()
        if row is None or not os.path.isabs(row[1]):
            return False
        if not row[0]:
            return True
        owner = _component_owner(row[2], row[3], row[4])
        components = self._conn.execute(
            "SELECT DISTINCT c.path,r.name,r.remote_url,su.key "
            "FROM component c LEFT JOIN repository r ON r.id=c.repository_id "
            "LEFT JOIN semantic_universe su ON "
            "su.id=COALESCE(c.semantic_universe_id,r.semantic_universe_id,1) "
            "WHERE c.name=?", (row[0],)
        )
        return any(
            os.path.isabs(candidate[0])
            and _component_owner(candidate[1], candidate[2], candidate[3]) == owner
            and candidate[0] != row[1]
            for candidate in components
        )

    def _reject_ambiguous_ungrouped(self, st: _Stream) -> None:
        if st.view not in ("call_argument", "site", "evidence"):
            return
        for key in st.keys:
            if (st.view == "evidence" and len(key) > 4 and key[4] == 1):
                continue
            if self._ambiguous_ungrouped_file(key[1]):
                raise PlanError("E_IDENTITY: ambiguous ungrouped component identity")

    def _portable_file(self, file_id: int) -> str:
        row = self._conn.execute(
            "SELECT c.name,c.path,d.path,f.name,r.name,r.remote_url,su.key "
            "FROM file f "
            "JOIN directory d ON d.id=f.directory_id "
            "JOIN component c ON c.id=d.component_id "
            "LEFT JOIN repository r ON r.id=c.repository_id "
            "LEFT JOIN semantic_universe su ON su.id=COALESCE(c.semantic_universe_id,r.semantic_universe_id,1) "
            "WHERE f.id=?", (file_id,)
        ).fetchone()
        if row is None:
            return f"missing-file:{file_id}"
        owner = (f"remote:{row[5]}" if row[5] else
                 f"repo:{row[4]}" if row[4] else f"universe:{row[6] or 'legacy'}")
        if os.path.isabs(row[1]):
            component = f"ungrouped:{row[0]}"
        else:
            component = f"grouped:{row[0]}\x1f{row[1]}"
        relative = "/".join(part.strip("/") for part in (row[2], row[3]) if part)
        return "file:" + "".join(
            f"{len(value.encode('utf-8'))}:{value}"
            for value in (owner, component, relative)
        )

    def _portable_type(self, type_id: int) -> str:
        row = self._conn.execute(
            "SELECT type_key,spelling FROM type_node WHERE id=?", (type_id,)
        ).fetchone()
        return (row[0] or row[1]) if row else f"missing-type:{type_id}"

    def _portable_edge(self, edge_id: int) -> str:
        row = self._conn.execute(
            "SELECT src_id,dst_id,kind FROM edge WHERE id=?", (edge_id,)
        ).fetchone()
        if row is None:
            return f"missing-edge:{edge_id}"
        return f"{self._portable_symbol(row[0])}:{row[2]}:{self._portable_symbol(row[1])}"

    def _logical_identity(self, view: str, key: tuple[int, ...]) -> str:
        if view == "parameter":
            return f"parameter:{self._portable_symbol(key[0])}:{key[1]}:{key[2]}"
        if view == "template_parameter":
            return f"template_parameter:{self._portable_symbol(key[0])}:{key[1]}"
        if view == "template_argument":
            return f"template_argument:{self._portable_symbol(key[0])}:{key[1]}:{key[2]}"
        if view == "call_argument":
            return (f"call_argument:{self._portable_edge(key[0])}:"
                    f"{self._portable_file(key[1])}:{key[2]}:{key[3]}:{key[4]}")
        if view == "evidence":
            if len(key) > 4 and key[4] == 1:
                return f"evidence:template_default:{self._portable_symbol(key[0])}:{key[1]}"
            return (f"evidence:{self._portable_edge(key[0])}:"
                    f"{self._portable_file(key[1])}:{key[2]}:{key[3]}")
        if view == "site":
            return (f"site:{self._portable_edge(key[0])}:"
                    f"{self._portable_file(key[1])}:{key[2]}:{key[3]}")
        if view == "edge":
            return f"edge:{self._portable_edge(key[0])}"
        if view == "type":
            return f"type:{self._portable_type(key[0])}"
        return view + ":" + ":".join(str(value) for value in key)

    def _logical_row_id(self, view: str, key: tuple[int, ...]) -> int:
        value = 1469598103934665603
        for byte in self._logical_identity(view, key).encode():
            value ^= byte
            value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        return value & 0x7FFFFFFFFFFFFFFF

    def _traverse_typed(
        self, st: _Stream, stage: Stage, rel: tuple[str, str, int],
        metadata: dict[str, Any], inbound: bool,
    ) -> None:
        target = (_relation_view(metadata.get("source", rel[1]))
                  if inbound else _relation_view(metadata.get("target", rel[1])))
        rows: list[tuple[int, ...]] = []
        ids: list[int] = []

        def add_rows(sql: str, args: Sequence[Any]) -> None:
            remaining = TRAVERSE_NODE_BUDGET - len(rows) - len(ids)
            if remaining <= 0:
                st.truncated = True
                return
            result = [tuple(row) for row in self._conn.execute(
                sql + " LIMIT ?", [*args, remaining + 1])]
            if len(result) > remaining:
                result = result[:remaining]
                st.truncated = True
            rows.extend(result)

        def add_ids(sql: str, args: Sequence[Any]) -> None:
            remaining = TRAVERSE_NODE_BUDGET - len(rows) - len(ids)
            if remaining <= 0:
                st.truncated = True
                return
            result = [row[0] for row in self._conn.execute(
                sql + " LIMIT ?", [*args, remaining + 1])]
            if len(result) > remaining:
                result = result[:remaining]
                st.truncated = True
            ids.extend(result)

        def add_synthetic(key: tuple[int, ...]) -> None:
            if len(rows) + len(ids) >= TRAVERSE_NODE_BUDGET:
                st.truncated = True
                return
            rows.append(key)

        if not inbound and st.view == SYMBOL_VIEW:
            for owner in st.ids:
                if rel[0] == "has_parameter":
                    add_rows("SELECT owner_id,position,pack_index FROM parameter WHERE owner_id=? ORDER BY position,pack_index", (owner,))
                elif rel[0] == "has_template_parameter":
                    add_rows("SELECT owner_id,position FROM template_param WHERE owner_id=? ORDER BY position", (owner,))
                elif rel[0] == "has_template_argument":
                    add_rows("SELECT owner_id,position,pack_index FROM template_arg WHERE owner_id=? ORDER BY position,pack_index", (owner,))
                elif rel[0] == "has_call_edge":
                    add_rows("SELECT id FROM edge WHERE src_id=? AND kind=? ORDER BY id", (owner, rel[2] - 23))
                elif rel[0] == "has_evidence":
                    add_rows("SELECT es.edge_id,es.file_id,COALESCE(es.line,0),COALESCE(es.col,0) FROM edge_site es JOIN edge e ON e.id=es.edge_id WHERE e.src_id=? ORDER BY es.edge_id,es.file_id,es.line,es.col", (owner,))
                elif rel[0] == "has_site":
                    add_rows("SELECT es.edge_id,es.file_id,COALESCE(es.line,0),COALESCE(es.col,0) FROM edge_site es JOIN edge e ON e.id=es.edge_id WHERE e.src_id=? ORDER BY es.edge_id,es.file_id,es.line,es.col", (owner,))
                elif rel[0] == "of_type":
                    add_ids("SELECT type_id FROM symbol_type WHERE symbol_id=? ORDER BY type_id", (owner,))
        elif not inbound and st.view == SYMBOL_VIEW and rel[0] == "of_type":
            for owner in st.ids:
                add_ids("SELECT type_id FROM symbol_type WHERE symbol_id=? ORDER BY type_id", (owner,))
        elif inbound and st.view in ("parameter", "template_parameter", "template_argument", "call_argument", "edge", "evidence") and rel[0] in ("has_parameter", "has_template_parameter", "has_template_argument", "has_call_edge"):
            if rel[0] in ("has_parameter", "has_template_parameter", "has_template_argument"):
                ids.extend(key[0] for key in st.keys)
            elif rel[0] == "has_call_edge":
                for key in st.keys:
                    add_ids("SELECT src_id FROM edge WHERE id=?", key[:1])
        elif not inbound and st.view == "parameter":
            for owner, position, pack_index in st.keys:
                if rel[0] in ("of_type", "declared_type", "adjusted_type"):
                    column = {"of_type": "type_id", "declared_type": "declared_type_id", "adjusted_type": "adjusted_type_id"}[rel[0]]
                    add_ids(f"SELECT {column} FROM parameter WHERE owner_id=? AND position=? AND pack_index=? AND {column} IS NOT NULL", (owner, position, pack_index))
                elif rel[0] == "references_symbol":
                    add_ids("SELECT decl_id FROM type_node WHERE id=(SELECT type_id FROM parameter WHERE owner_id=? AND position=? AND pack_index=?) AND decl_id IS NOT NULL UNION SELECT symbol_id FROM symbol_type WHERE type_id=(SELECT type_id FROM parameter WHERE owner_id=? AND position=? AND pack_index=?) ORDER BY 1", (owner, position, pack_index, owner, position, pack_index))
                elif rel[0] == "has_evidence":
                    add_rows("SELECT e.edge_id,e.file_id,COALESCE(e.line,0),COALESCE(e.col,0) FROM edge_site e JOIN parameter p ON p.file_id=e.file_id AND p.line=e.line AND p.col=e.col WHERE p.owner_id=? AND p.position=? AND p.pack_index=? ORDER BY e.edge_id,e.file_id,e.line,e.col", (owner, position, pack_index))
        elif not inbound and st.view == "template_parameter":
            for owner, position in st.keys:
                if rel[0] == "of_type":
                    add_ids("SELECT type_id FROM template_param WHERE owner_id=? AND position=? AND type_id IS NOT NULL", (owner, position))
                elif rel[0] == "has_default":
                    if self._conn.execute("SELECT 1 FROM template_param WHERE owner_id=? AND position=? AND (default_txt IS NOT NULL OR default_type_id IS NOT NULL OR default_ref_id IS NOT NULL)", (owner, position)).fetchone():
                        add_synthetic((owner, position, 0, 0, 1))
        elif not inbound and st.view == "template_argument":
            for owner, position, pack_index in st.keys:
                if rel[0] == "of_type":
                    add_ids("SELECT type_id FROM template_arg WHERE owner_id=? AND position=? AND pack_index=? AND type_id IS NOT NULL", (owner, position, pack_index))
                elif rel[0] == "references_symbol":
                    add_ids("SELECT ref_id FROM template_arg WHERE owner_id=? AND position=? AND pack_index=? AND ref_id IS NOT NULL", (owner, position, pack_index))
        elif not inbound and st.view == "edge":
            for (edge_id,) in st.keys:
                if rel[0] == "has_argument":
                    add_rows("SELECT edge_id,file_id,line,col,position FROM call_arg WHERE edge_id=? ORDER BY file_id,line,col,position", (edge_id,))
                elif rel[0] == "has_evidence":
                    add_rows("SELECT edge_id,file_id,COALESCE(line,0),COALESCE(col,0) FROM edge_site WHERE edge_id=? ORDER BY file_id,line,col", (edge_id,))
                elif rel[0] == "has_site":
                    add_rows("SELECT edge_id,file_id,COALESCE(line,0),COALESCE(col,0) FROM edge_site WHERE edge_id=? ORDER BY file_id,line,col", (edge_id,))
        elif not inbound and st.view == "call_argument":
            for edge_id, file_id, line, col, position in st.keys:
                if rel[0] == "of_type":
                    add_ids("SELECT type_id FROM call_arg WHERE edge_id=? AND file_id=? AND line=? AND col=? AND position=? AND type_id IS NOT NULL", (edge_id, file_id, line, col, position))
                elif rel[0] == "references_symbol":
                    add_ids("SELECT decl_id FROM call_arg WHERE edge_id=? AND file_id=? AND line=? AND col=? AND position=? AND decl_id IS NOT NULL", (edge_id, file_id, line, col, position))
                elif rel[0] == "has_evidence":
                    add_rows("SELECT edge_id,file_id,COALESCE(line,0),COALESCE(col,0) FROM edge_site WHERE edge_id=? AND file_id=? AND line=? AND col=?", (edge_id, file_id, line, col))
        elif not inbound and st.view == "evidence":
            for edge_id, file_id, line, col in st.keys:
                if rel[0] == "of_edge":
                    add_rows("SELECT edge_id FROM edge_site WHERE edge_id=? AND file_id=? AND line=? AND col=?", (edge_id, file_id, line, col))
                elif rel[0] == "of_occurrence":
                    add_rows("SELECT edge_id,file_id,line,col,position FROM call_arg WHERE edge_id=? AND file_id=? AND line=? AND col=?", (edge_id, file_id, line, col))
        elif not inbound and st.view == "site":
            for edge_id, file_id, line, col in st.keys:
                if rel[0] == "of_edge":
                    add_rows("SELECT edge_id FROM edge_site WHERE edge_id=? AND file_id=? AND COALESCE(line,0)=? AND COALESCE(col,0)=?", (edge_id, file_id, line, col))
        elif not inbound and st.view == "type":
            for (type_id,) in st.keys:
                if rel[0] == "references_symbol":
                    add_ids("SELECT decl_id FROM type_node WHERE id=? AND decl_id IS NOT NULL UNION SELECT symbol_id FROM symbol_type WHERE type_id=? ORDER BY 1", (type_id, type_id))
                elif rel[0] == "has_type_edge":
                    add_ids("SELECT dst_id FROM type_edge WHERE src_id=? ORDER BY position", (type_id,))
        elif inbound and st.view == "type":
            column = {"of_type": "type_id", "declared_type": "declared_type_id", "adjusted_type": "adjusted_type_id"}.get(rel[0])
            if column is not None:
                if rel[1] == SYMBOL_VIEW:
                    for (type_id,) in st.keys:
                        add_ids("SELECT symbol_id FROM symbol_type WHERE type_id=? ORDER BY symbol_id", (type_id,))
                    column = None
            if column is not None:
                table = {"parameter": ("parameter", "owner_id,position,pack_index"), "template_parameter": ("template_param", "owner_id,position"), "template_argument": ("template_arg", "owner_id,position,pack_index"), "call_argument": ("call_arg", "edge_id,file_id,line,col,position")}.get(rel[1])
                if table is not None:
                    for (type_id,) in st.keys:
                        add_rows(f"SELECT {table[1]} FROM {table[0]} WHERE {column}=? ORDER BY {table[1]}", (type_id,))
        elif inbound and st.view == SYMBOL_VIEW:
            if rel[0] == "references_symbol":
                for symbol_id in st.ids:
                    if rel[1] == "parameter":
                        add_rows("SELECT p.owner_id,p.position,p.pack_index FROM parameter p WHERE EXISTS (SELECT 1 FROM type_node t WHERE t.id=p.type_id AND t.decl_id=?) OR EXISTS (SELECT 1 FROM symbol_type st WHERE st.type_id=p.type_id AND st.symbol_id=?) ORDER BY p.owner_id,p.position,p.pack_index", (symbol_id, symbol_id))
                    elif rel[1] == "template_argument":
                        add_rows("SELECT owner_id,position,pack_index FROM template_arg WHERE ref_id=? ORDER BY owner_id,position,pack_index", (symbol_id,))
                    elif rel[1] == "call_argument":
                        add_rows("SELECT edge_id,file_id,line,col,position FROM call_arg WHERE decl_id=? ORDER BY edge_id,file_id,line,col,position", (symbol_id,))
                    elif rel[1] == "type":
                        add_ids("SELECT type_id FROM symbol_type WHERE symbol_id=? UNION SELECT id FROM type_node WHERE decl_id=? ORDER BY 1", (symbol_id, symbol_id))
            elif rel[0] == "of_type":
                for symbol_id in st.ids:
                    add_ids("SELECT symbol_id FROM symbol_type WHERE type_id=?", (symbol_id,))
        elif inbound and st.view == "evidence":
            for edge_id, file_id, line, col in st.keys:
                if rel[0] == "has_evidence":
                    if rel[1] == "edge":
                        add_rows("SELECT edge_id FROM edge_site WHERE edge_id=? AND file_id=? AND COALESCE(line,0)=? AND COALESCE(col,0)=?", (edge_id, file_id, line, col))
                    elif rel[1] == "call_argument":
                        add_rows("SELECT edge_id,file_id,line,col,position FROM call_arg WHERE edge_id=? AND file_id=? AND line=? AND col=?", (edge_id, file_id, line, col))
                    elif rel[1] == "symbol":
                        add_ids("SELECT src_id FROM edge WHERE id=?", (edge_id,))
                    elif rel[1] == "parameter":
                        add_rows("SELECT owner_id,position,pack_index FROM parameter WHERE file_id=? AND line=? AND col=?", (file_id, line, col))
        elif inbound and st.view == "site":
            for edge_id, file_id, line, col in st.keys:
                if rel[0] in ("has_site", "of_edge"):
                    add_rows("SELECT edge_id FROM edge_site WHERE edge_id=? AND file_id=? AND COALESCE(line,0)=? AND COALESCE(col,0)=?", (edge_id, file_id, line, col))
        elif inbound and st.view == "call_argument" and rel[0] == "has_argument":
            for key in st.keys:
                add_rows("SELECT id FROM edge WHERE id=?", key[:1])
        elif inbound and st.view == "call_argument" and rel[0] == "of_occurrence":
            for key in st.keys:
                add_rows("SELECT edge_id,file_id,COALESCE(line,0),COALESCE(col,0) FROM edge_site WHERE edge_id=? AND file_id=? AND COALESCE(line,0)=? AND COALESCE(col,0)=?", key[:4])
        elif inbound and st.view == "edge" and rel[0] == "of_edge":
            for key in st.keys:
                add_rows("SELECT edge_id,file_id,COALESCE(line,0),COALESCE(col,0) FROM edge_site WHERE edge_id=? ORDER BY file_id,line,col", key[:1])
        elif inbound and st.view == "type" and rel[0] == "has_type_edge":
            for (type_id,) in st.keys:
                add_ids("SELECT src_id FROM type_edge WHERE dst_id=? ORDER BY src_id", (type_id,))

        if target in (SYMBOL_VIEW, ENTITY_VIEW):
            st.ids = sorted(set(ids))
            st.keys = []
        else:
            st.ids = []
            st.keys = sorted(set(rows))
            if target == "type":
                st.keys = sorted(set(st.keys).union((item,) for item in ids))
            if len(st.keys) > TRAVERSE_NODE_BUDGET:
                del st.keys[TRAVERSE_NODE_BUDGET:]
                st.truncated = True
        if target in (SYMBOL_VIEW, ENTITY_VIEW) and len(st.ids) > TRAVERSE_NODE_BUDGET:
            del st.ids[TRAVERSE_NODE_BUDGET:]
            st.truncated = True
        st.view = target

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
        if _is_typed_view(st.view):
            left, right = set(st.keys), set(sub.keys)
            if stage.op == "union":
                st.keys = sorted(left | right)
            elif stage.op == "intersect":
                st.keys = sorted(left & right)
            else:
                st.keys = sorted(left - right)
            st.ids = []
            if len(st.keys) > TRAVERSE_NODE_BUDGET:
                del st.keys[TRAVERSE_NODE_BUDGET:]
                st.truncated = True
            return
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

    @staticmethod
    def _typed_table(view: str) -> str:
        return {
            "parameter": "parameter", "template_parameter": "template_param",
            "template_argument": "template_arg", "call_argument": "call_arg",
            "edge": "edge", "evidence": "edge_site", "type": "type_node",
            "site": "edge_site",
        }[view]

    @staticmethod
    def _typed_where(view: str) -> str:
        return {
            "parameter": "owner_id=? AND position=? AND pack_index=?",
            "template_parameter": "owner_id=? AND position=?",
            "template_argument": "owner_id=? AND position=? AND pack_index=?",
            "call_argument": "edge_id=? AND file_id=? AND line=? AND col=? AND position=?",
            "evidence": "edge_id=? AND file_id=? AND line=? AND col=?",
            "site": "edge_id=? AND file_id=? AND line=? AND col=?",
            "edge": "id=?", "type": "id=?",
        }[view]

    @staticmethod
    def _typed_args(view: str, key: tuple[int, ...]) -> tuple[int, ...]:
        return key

    @staticmethod
    def _typed_column(view: str, field_name: str) -> str:
        if field_name == "file":
            return "file_id"
        if field_name == "cv_qualifiers" and view == "type":
            return "(is_const + 2 * is_volatile + 4 * is_restrict)"
        if field_name == "edge_id" and view == "edge":
            return "id"
        if field_name in ("id", "identity_key"):
            return ""
        columns = {
            "parameter": {"owner_id", "position", "pack_index", "name", "type_id", "declared_type_id", "adjusted_type_id", "default_text", "default_origin", "reference_semantics", "file_id", "line", "col"},
            "template_parameter": {"owner_id", "position", "param_kind", "name", "default_txt", "type_id", "default_type_id", "default_ref_id"},
            "template_argument": {"owner_id", "position", "pack_index", "arg_kind", "ref_id", "literal", "type_id"},
            "call_argument": {"edge_id", "file_id", "line", "col", "position", "src_kind", "type_usr", "decl_usr", "callee_usr", "type_id", "decl_id", "callee_id", "type_is_value"},
            "evidence": {"edge_id", "file_id", "line", "col", "conditional", "args_sig", "recv_src_kind", "recv_type_usr", "recv_decl_usr", "recv_type_id", "recv_decl_id", "recv_param_pos", "recv_type_is_value"},
            "site": {"edge_id", "file_id", "line", "col"},
            "edge": {"id", "src_id", "dst_id", "kind", "count", "base_access", "is_virtual", "vtable_slot"},
            "type": {"id", "type_key", "spelling", "kind", "is_const", "is_volatile", "is_restrict", "decl_usr", "decl_id", "canonical_id"},
        }
        return field_name if field_name in columns[view] else ""

    def _derived_typed_cell(
        self, view: str, key: tuple[int, ...], field_name: str,
    ) -> tuple[bool, Any]:
        derived_fields = {"relation", "source", "target", "evidence", "status", "partial", "unknown"}
        if field_name not in derived_fields:
            return False, None
        if view == "evidence" and len(key) > 4 and key[4] == 1:
            values = {"evidence": "declaration", "status": "partial", "partial": 1, "unknown": 0}
            return True, values.get(field_name)
        if view not in {"edge", "site", "evidence"}:
            return True, None
        row = self._conn.execute("SELECT kind FROM edge WHERE id=?", (key[0],)).fetchone()
        relation = next((item for item in RELATION_CATALOG
                         if item[1] == SYMBOL_VIEW and item[2] == row[0]), None) if row else None
        metadata = RELATION_METADATA.get(relation, {}) if relation else {}
        if field_name == "relation":
            return True, relation[0] if relation else None
        if field_name in {"source", "target", "evidence"}:
            return True, metadata.get(field_name)
        if field_name == "status":
            return True, metadata.get("completeness")
        if field_name == "partial":
            return True, int(metadata.get("completeness") == "partial")
        return True, int(metadata.get("completeness") == "unknown") if relation else 1

    def _fetch_typed_cells(
        self, st: _Stream, fields: Sequence[str]
    ) -> dict[tuple[int, ...], tuple[Any, ...]]:
        result: dict[tuple[int, ...], tuple[Any, ...]] = {}
        string_fields = {"name", "spelling", "type_key", "default_text", "default_origin", "default_txt", "reference_semantics", "literal", "src_kind", "type_usr", "decl_usr", "callee_usr", "args_sig", "recv_src_kind", "recv_type_usr", "recv_decl_usr", "identity_key", "file"}
        for key in st.keys:
            if st.view == "evidence" and len(key) > 4 and key[4] == 1:
                source = self._conn.execute(
                    "SELECT default_txt,default_type_id,default_ref_id "
                    "FROM template_param WHERE owner_id=? AND position=?",
                    key[:2],
                ).fetchone()
                if source is None:
                    continue
                cells = []
                for field_name in fields:
                    handled, derived = self._derived_typed_cell(st.view, key, field_name)
                    if handled:
                        cells.append(derived)
                    elif field_name == "identity_key":
                        cells.append(self._logical_identity(st.view, key))
                    elif field_name == "id":
                        cells.append(self._logical_row_id(st.view, key))
                    elif field_name == "owner_id":
                        cells.append(key[0])
                    elif field_name == "position":
                        cells.append(key[1])
                    elif field_name == "default_txt":
                        cells.append(source[0])
                    elif field_name == "default_type_id":
                        cells.append(source[1])
                    elif field_name == "default_ref_id":
                        cells.append(source[2])
                    elif field_name == "role":
                        cells.append("default")
                    elif field_name == "provenance":
                        cells.append("template_parameter")
                    else:
                        cells.append(None)
                result[key] = tuple(cells)
                continue
            columns: list[str] = []
            indexes: list[int] = []
            for field_name in fields:
                column = self._typed_column(st.view, field_name)
                indexes.append(len(columns) if column else -1)
                if column:
                    columns.append(column)
            select = ",".join(columns) if columns else "1"
            sql = f"SELECT {select} FROM {self._typed_table(st.view)} WHERE {self._typed_where(st.view)}"
            row = self._conn.execute(sql, self._typed_args(st.view, key)).fetchone()
            if row is None:
                continue
            cells: list[Any] = []
            for field_name, index in zip(fields, indexes):
                handled, derived = self._derived_typed_cell(st.view, key, field_name)
                if handled:
                    cells.append(derived)
                elif field_name == "identity_key":
                    cells.append(self._logical_identity(st.view, key))
                elif field_name == "id":
                    cells.append(self._logical_row_id(st.view, key))
                elif index < 0:
                    cells.append(None)
                elif row[index] is None:
                    cells.append(None)
                elif field_name == "file":
                    cells.append(self._file_path(row[index]))
                elif field_name in string_fields:
                    cells.append(row[index])
                else:
                    cells.append(row[index])
            result[key] = tuple(cells)
        return result

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
        self._reject_ambiguous_ungrouped(st)
        if st.view not in (SYMBOL_VIEW, ENTITY_VIEW):
            by_key = self._fetch_typed_cells(st, fields)
            st.fields = tuple(fields)
            st.rows = []
            st.row_ids = []
            for key in st.keys:
                if key in by_key:
                    st.rows.append(by_key[key])
                    st.row_ids.append(self._logical_row_id(st.view, key))
            st.keys = []
            return
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
            if st.keys:
                st.keys = sorted(set(st.keys))
            else:
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
            if st.keys:
                by_key = self._fetch_typed_cells(st, fields)
                st.keys.sort(key=lambda key: (tuple(_cell_key(c) for c in by_key[key]), key))
                return
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
            if st.keys:
                del st.keys[n:]
            else:
                del st.ids[n:]
        else:
            del st.rows[n:]
            del st.row_ids[n:]

    # -- finish --------------------------------------------------------------------

    def _update_status(self, st: _Stream) -> None:
        for key in st.keys:
            partial_handled, partial = self._derived_typed_cell(st.view, key, "partial")
            unknown_handled, unknown = self._derived_typed_cell(st.view, key, "unknown")
            if partial_handled and partial:
                st.partial = True
            if unknown_handled and unknown:
                st.unknown = True

    def _finish(self, st: _Stream) -> Result:
        self._update_status(st)
        self._reject_ambiguous_ungrouped(st)
        if st.shape == "scalar":
            return Result(
                shape="scalar", view=st.view, truncated=st.truncated,
                partial=st.partial, unknown=st.unknown,
                scalar=len(st.rows) if st.rows else (len(st.keys) if st.keys else len(st.ids)))
        if st.shape == "nodes":
            if st.view not in (SYMBOL_VIEW, ENTITY_VIEW):
                self._materialize(st, ("id", "identity_key"))
            else:
                self._materialize(
                    st,
                    ("id", "usr", "semantic_universe", "identity_key", "name", "kind"),
                )
        if not st.limit_in_effect and len(st.rows) > DEFAULT_RESULT_CAP:
            del st.rows[DEFAULT_RESULT_CAP:]
            del st.row_ids[DEFAULT_RESULT_CAP:]
            st.truncated = True
        return Result(
            shape=st.shape, view=st.view, truncated=st.truncated,
            partial=st.partial, unknown=st.unknown,
            fields=st.fields, rows=st.rows)
