"""Validate required Story acceptance checks at the merged revision."""

from __future__ import annotations

import builtins
import json
import os
from pathlib import Path
import re
import subprocess
import threading
from typing import Any, NoReturn

from backlog_cli.api import Backlog
from backlog_cli.hooks import Action


_ACTIVE_MARKER_ATTRIBUTE = "_cidx_backlog_delivery_validation_markers"
_MAX_REASON_CHARS = 2048
_CHECK_ACTIONS = {
    Action.CHECK_STARTED,
    Action.CHECK_PASSED,
    Action.CHECK_FAILED,
    Action.CHECK_CANCELLED,
    Action.CHECK_TIMED_OUT,
}


def _parameter(trigger: dict[str, Any], name: str, environment: str) -> str:
    parameters = trigger.get("parameters")
    if isinstance(parameters, dict):
        value = parameters.get(name)
        if isinstance(value, str) and value.strip():
            return value.strip()
    return os.environ.get(environment, "").strip()


def _git(root: Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", "-C", str(root), *arguments],
        check=True,
        capture_output=True,
        text=True,
        timeout=30,
    )
    return completed.stdout.strip()


def _checkout(
    trigger: dict[str, Any], task: Any
) -> tuple[Path | None, str | None, dict[str, Any] | None]:
    root_text = _parameter(trigger, "project_root", "BACKLOG_PROJECT_ROOT")
    revision = _parameter(trigger, "delivery_revision", "BACKLOG_DELIVERY_REVISION")
    pull_request_url = _parameter(
        trigger, "pull_request_url", "BACKLOG_DELIVERY_PR_URL"
    )
    if not root_text:
        return (
            None,
            None,
            {
                "item_id": None,
                "executor": None,
                "outcome": "validation_environment_unavailable",
                "reason": "project_root is required",
            },
        )
    if not revision:
        return (
            None,
            None,
            {
                "item_id": None,
                "executor": None,
                "outcome": "validation_environment_unavailable",
                "reason": "delivery_revision is required",
            },
        )
    if not pull_request_url:
        return (
            None,
            None,
            {
                "item_id": None,
                "executor": None,
                "outcome": "validation_environment_unavailable",
                "reason": "pull_request_url is required",
            },
        )
    if pull_request_url.rstrip("/") != str(task.pr_url or "").rstrip("/"):
        return (
            None,
            None,
            {
                "item_id": None,
                "executor": None,
                "outcome": "validation_environment_unavailable",
                "reason": "delivery pull_request_url does not match the task PR",
            },
        )
    if re.fullmatch(r"[0-9a-fA-F]{40}", revision) is None:
        return (
            None,
            None,
            {
                "item_id": None,
                "executor": None,
                "outcome": "validation_environment_unavailable",
                "reason": "delivery_revision must be a full 40-character Git SHA",
            },
        )

    try:
        root = Path(root_text).expanduser().resolve(strict=True)
        if not root.is_dir():
            raise ValueError("project_root is not a directory")
        top_level = Path(_git(root, "rev-parse", "--show-toplevel")).resolve()
        if top_level != root:
            raise ValueError("project_root is not the checkout root")
        head = _git(root, "rev-parse", "HEAD")
        expected = _git(root, "rev-parse", f"{revision}^{{commit}}")
        if head != expected:
            raise ValueError(f"checkout HEAD {head} does not match {expected}")
        if _git(root, "status", "--porcelain", "--untracked-files=normal"):
            raise ValueError("checkout is not clean")
        if (
            not (root / "CMakeLists.txt").is_file()
            or not (root / ".backlog" / "workflow.yaml").is_file()
        ):
            raise ValueError("checkout is not a cpp-indexer project root")
    except (OSError, subprocess.SubprocessError, ValueError) as error:
        return (
            None,
            None,
            {
                "item_id": None,
                "executor": None,
                "outcome": "validation_environment_unavailable",
                "reason": str(error),
            },
        )

    return root, expected, None


def _item_value(item: dict[str, Any], name: str) -> Any:
    value = item.get(name)
    if value is not None:
        return value
    execution = item.get("execution")
    return execution.get(name) if isinstance(execution, dict) else None


def _required_acceptance_items(task: Any) -> list[dict[str, Any]]:
    return [
        item
        for item in task.item_details("acceptance_criteria")
        if _item_value(item, "executor") in {"shell", "hook"}
        and (_item_value(item, "requirement") or "required") == "required"
    ]


def _status(result: Any) -> str:
    status = getattr(result, "status", "error")
    value = getattr(status, "value", status)
    return str(value).lower()


def _reason(result: Any) -> str:
    values = (
        getattr(result, "diagnostic", None),
        getattr(result, "reason", None),
        getattr(result, "detail", None),
    )
    reason = next((str(value) for value in values if value), "no diagnostic")
    if len(reason) <= _MAX_REASON_CHARS:
        return reason
    return reason[:_MAX_REASON_CHARS] + "...[truncated]"


def _failure(item: dict[str, Any], result: Any) -> dict[str, Any] | None:
    status = _status(result)
    if status == "pass":
        return None

    reason = _reason(result)
    reason_code = reason.partition(":")[0].strip().lower()
    if status == "skipped" and reason_code == "policy_denied":
        outcome = "validation_policy_denied"
    elif status == "skipped" and reason_code == "batch_budget_exhausted":
        outcome = "validation_batch_budget_exhausted"
    elif status == "skipped":
        outcome = "validation_skipped"
    elif status == "fail":
        outcome = "check_failed"
    elif status == "error" and reason_code in {
        "timed_out",
        "timeout",
        "hook_timed_out",
    }:
        outcome = "check_timeout"
    elif status == "error":
        outcome = "check_error"
    else:
        outcome = "check_error"

    return {
        "item_id": int(_item_value(item, "id")),
        "executor": str(
            getattr(result, "executor", None)
            or _item_value(item, "executor")
            or "unknown"
        ),
        "outcome": outcome,
        "reason": reason,
    }


def _active_markers() -> set[tuple[str, int]]:
    markers = getattr(builtins, _ACTIVE_MARKER_ATTRIBUTE, None)
    if not isinstance(markers, set):
        markers = set()
        setattr(builtins, _ACTIVE_MARKER_ATTRIBUTE, markers)
    return markers


def _refuse(
    backlog: Backlog,
    task_key: str,
    action: Action,
    actor: str,
    failures: list[dict[str, Any]],
) -> NoReturn:
    record = {
        "action": action.value,
        "actor": actor,
        "failures": failures,
        "task_key": task_key,
    }
    serialized = json.dumps(record, sort_keys=True, separators=(",", ":"))
    backlog.add_item(task_key, "note", f"validation.refused {serialized}")
    backlog.commit()
    raise RuntimeError(f"delivery_validation_refused: {serialized}")


def pre_transition(
    action: Action,
    trigger: dict[str, Any],
    current_state: str,
    new_state: str,
    backlog: Backlog,
) -> str:
    """Gate only the configured Story Accepted-to-Done merge transition."""

    task_key = str(trigger.get("task_key") or "")
    marker = (task_key, threading.get_ident())
    if action in _CHECK_ACTIONS and marker in _active_markers():
        return current_state

    if not (
        action == Action.PR_MERGED
        and current_state == "accepted"
        and new_state == "done"
        and task_key
    ):
        return new_state

    task = backlog.task(task_key)
    if task.task_type != "story":
        return new_state

    items = _required_acceptance_items(task)
    if not items:
        return new_state

    actor = str(trigger.get("actor") or "").strip()
    if not actor:
        _refuse(
            backlog,
            task_key,
            action,
            "unattributed",
            [
                {
                    "item_id": None,
                    "executor": None,
                    "outcome": "validation_environment_unavailable",
                    "reason": "the Done action actor is required",
                }
            ],
        )

    root, _revision, checkout_failure = _checkout(trigger, task)
    if checkout_failure is not None:
        _refuse(
            backlog,
            task_key,
            action,
            actor,
            [checkout_failure],
        )
    assert root is not None

    policy = root / ".backlog" / "execution.yaml"
    if not policy.is_file():
        _refuse(
            backlog,
            task_key,
            action,
            actor,
            [
                {
                    "item_id": None,
                    "executor": None,
                    "outcome": "validation_policy_denied",
                    "reason": f"missing trusted execution policy: {policy}",
                }
            ],
        )

    markers = _active_markers()
    markers.add(marker)
    try:
        results = [
            backlog.run_item(
                int(_item_value(item, "id")),
                str(root),
                actor=actor,
            )
            for item in items
        ]
    finally:
        markers.discard(marker)

    failures = [
        failure
        for item, result in zip(items, results, strict=True)
        if (failure := _failure(item, result)) is not None
    ]
    if failures:
        _refuse(backlog, task_key, action, actor, failures)
    return new_state
