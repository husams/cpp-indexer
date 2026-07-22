"""Gherkin data tables: cell coercion, row parsing and row-list comparison.

Cell conventions (see the conftest docstring):

    true / false      -> bool
    -                 -> None (absent / not set)
    "" (empty cell)   -> None
    123               -> int
    anything else     -> str
"""

from __future__ import annotations

import json
from typing import Any


def _coerce(cell: str) -> Any:
    """Gherkin cell -> Python value."""
    text = cell.strip()
    if text in ("", "-"):
        return None
    if text == "true":
        return True
    if text == "false":
        return False
    if text.lstrip("+-").isdigit():
        return int(text)
    return text


def rows(datatable: list[list[str]]) -> list[dict[str, Any]]:
    """Gherkin data table -> list of {column: coerced value}, header row first."""
    assert datatable and len(datatable) >= 1, "expected a data table"
    header = [h.strip() for h in datatable[0]]
    assert all(header), f"table contains an empty header cell: {datatable[0]!r}"
    duplicates = sorted({h for h in header if header.count(h) > 1})
    assert not duplicates, f"table contains duplicate column(s): {duplicates}"
    out = []
    for raw in datatable[1:]:
        assert len(raw) == len(header), (
            f"row {raw!r} has {len(raw)} cells but the header has {len(header)}"
        )
        out.append({h: _coerce(c) for h, c in zip(header, raw)})
    return out


def assert_same(
    expected: list[dict[str, Any]],
    actual: list[dict[str, Any]],
    what: str,
    *,
    ordered: bool = True,
) -> None:
    """Compare row lists, restricting actual rows to the stated columns."""
    if not expected:
        assert not actual, f"{what}: expected none, got {actual}"
        return
    cols = list(expected[0].keys())
    for row in expected:
        assert list(row.keys()) == cols, f"{what}: ragged table header"
    if actual:
        known = set().union(*(row.keys() for row in actual))
        unknown = set(cols) - known
        assert not unknown, (
            f"{what}: unknown table column(s) {sorted(unknown)}; known: {sorted(known)}"
        )
    trimmed = [{k: a.get(k) for k in cols} for a in actual]

    if ordered:
        ok = trimmed == expected
    else:
        ok = sorted(map(_key, trimmed)) == sorted(map(_key, expected))
    assert ok, (
        f"{what} differ\n"
        f"  expected ({len(expected)}):\n"
        + "\n".join(f"    {json.dumps(r, default=str)}" for r in expected)
        + f"\n  actual ({len(trimmed)}):\n"
        + "\n".join(f"    {json.dumps(r, default=str)}" for r in trimmed)
    )


def _key(row: dict[str, Any]) -> str:
    return json.dumps(row, sort_keys=True, default=str)
