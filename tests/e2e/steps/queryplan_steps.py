"""WHEN/THEN: public Python QueryPlan execution and result assertions."""

from __future__ import annotations

import inspect
import sqlite3
from dataclasses import dataclass
from typing import Any

from pytest_bdd import parsers, then, when

from indexer import queryplan as qp
from indexer.storage import Storage

from .tables import rows
from .workspace import Workspace


@dataclass(frozen=True)
class QueryPlanObservation:
    """One public QueryPlan execution plus its static-plan control."""

    canonical: str
    static_canonical: str
    result: qp.Result
    receiver_aware: bool


def _receiver_aware_out(min_depth: int, max_depth: int) -> qp.Stage:
    """Use either accepted HSE-33 IR shape: a mode or a derived relation."""
    parameters = inspect.signature(qp.out).parameters
    if "mode" in parameters:
        mode: Any = "devirtualized"
        for enum_name in ("TraversalMode", "CallTraversalMode", "DispatchMode"):
            enum_type = getattr(qp, enum_name, None)
            if enum_type is None:
                continue
            for member_name in ("DEVIRTUALIZED", "RECEIVER_AWARE"):
                if hasattr(enum_type, member_name):
                    mode = getattr(enum_type, member_name)
                    break
        return qp.out(
            "calls",
            min_depth=min_depth,
            max_depth=max_depth,
            mode=mode,
        )

    for name, layer, _kind in qp.relation_catalog():
        normalized = name.replace("-", "_").lower()
        if layer == qp.SYMBOL_VIEW and "call" in normalized and (
            "devirtual" in normalized or "receiver" in normalized
        ):
            return qp.out(name, min_depth=min_depth, max_depth=max_depth)

    raise AssertionError(
        "HSE-33 incomplete: the public Python QueryPlan builder exposes neither "
        "a receiver-aware/devirtualized calls traversal mode nor a typed derived "
        "relation"
    )


def _run_plan(workspace: Workspace, plan: qp.Plan) -> qp.Result:
    """Run through the public executor on an explicitly read-only connection."""
    uri = f"file:{workspace.db.resolve()}?mode=ro"
    connection = sqlite3.connect(uri, uri=True)
    try:
        storage = Storage.from_connection(connection, str(workspace.db))
        return qp.Executor(storage).run(plan)
    finally:
        connection.close()


def _query(
    workspace: Workspace,
    root: str,
    min_depth: int,
    max_depth: int,
    *,
    receiver_aware: bool,
) -> QueryPlanObservation:
    static_query = (
        qp.start(qp.symbol(root))
        | qp.out("calls", min_depth=min_depth, max_depth=max_depth)
        | qp.select(["name", "kind", "line"])
    )
    stage = (
        _receiver_aware_out(min_depth, max_depth)
        if receiver_aware
        else qp.out("calls", min_depth=min_depth, max_depth=max_depth)
    )
    query = (
        qp.start(qp.symbol(root))
        | stage
        | qp.select(["name", "kind", "line"])
    )
    return QueryPlanObservation(
        canonical=qp.canonical_json(query.plan),
        static_canonical=qp.canonical_json(static_query.plan),
        result=_run_plan(workspace, query.plan),
        receiver_aware=receiver_aware,
    )


@when(
    parsers.parse(
        'I run the static Python QueryPlan from "{root}" over calls at depths '
        "{min_depth:d} through {max_depth:d}"
    ),
    target_fixture="queryplan_observation",
)
def run_static_queryplan(
    workspace: Workspace, root: str, min_depth: int, max_depth: int
) -> QueryPlanObservation:
    return _query(
        workspace,
        root,
        min_depth,
        max_depth,
        receiver_aware=False,
    )


@when(
    parsers.parse(
        'I run the receiver-aware Python QueryPlan from "{root}" over calls at '
        "depths {min_depth:d} through {max_depth:d}"
    ),
    target_fixture="queryplan_observation",
)
def run_receiver_aware_queryplan(
    workspace: Workspace, root: str, min_depth: int, max_depth: int
) -> QueryPlanObservation:
    return _query(
        workspace,
        root,
        min_depth,
        max_depth,
        receiver_aware=True,
    )


@then("receiver-aware traversal is explicit in the canonical QueryPlan")
def canonical_plan_encodes_receiver_awareness(
    queryplan_observation: QueryPlanObservation,
) -> None:
    observation = queryplan_observation
    assert observation.receiver_aware
    assert observation.canonical != observation.static_canonical, (
        "HSE-33 incomplete: receiver-aware traversal normalized to the same "
        "canonical QueryPlan as ordinary static calls traversal"
    )


@then("the QueryPlan result is complete")
def queryplan_result_is_complete(
    queryplan_observation: QueryPlanObservation,
) -> None:
    assert not queryplan_observation.result.truncated


def _result_rows(observation: QueryPlanObservation) -> list[dict[str, Any]]:
    result = observation.result.to_dict()
    assert result["shape"] == "rows", result
    return result["rows"]


def _matches(actual: dict[str, Any], expected: dict[str, Any]) -> bool:
    return all(actual.get(key) == value for key, value in expected.items())


@then("the QueryPlan result contains:")
def queryplan_result_contains(
    queryplan_observation: QueryPlanObservation,
    datatable: list[list[str]],
) -> None:
    actual = _result_rows(queryplan_observation)
    for expected in rows(datatable):
        matches = [item for item in actual if _matches(item, expected)]
        assert len(matches) == 1, (
            f"expected exactly one QueryPlan row matching {expected}, "
            f"found {len(matches)} in {actual}"
        )


@then("the QueryPlan result excludes:")
def queryplan_result_excludes(
    queryplan_observation: QueryPlanObservation,
    datatable: list[list[str]],
) -> None:
    actual = _result_rows(queryplan_observation)
    for excluded in rows(datatable):
        matches = [item for item in actual if _matches(item, excluded)]
        assert not matches, f"unexpected QueryPlan row(s) matching {excluded}: {matches}"
