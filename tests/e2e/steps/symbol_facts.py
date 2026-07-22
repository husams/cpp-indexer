"""The comparable view of a symbol, and how a feature-file row matches one."""

from __future__ import annotations

import json
from typing import Any

from .workspace import Workspace


def sym_facts(sym) -> dict[str, Any]:
    """The comparable view of a symbol: every column a feature file may assert."""
    return {
        "usr": sym.usr,
        "spelling": sym.spelling,
        "qual_name": sym.name,
        "kind": sym.kind,
        "type_info": sym.type_info,
        "file": sym.file.name if sym.file else None,
        "line": sym.line,
        "col": sym.col,
        "end_line": sym.end_line,
        "end_col": sym.end_col,
        "is_definition": bool(sym.is_definition),
        "is_instantiation": bool(sym.is_instantiation),
        "is_static": bool(sym.is_static),
        "is_pure": bool(sym.is_pure),
        "is_stub": bool(sym.is_stub),
        "access": sym.access,
    }


def describe(sym) -> str:
    return f"{sym.name} [{sym.kind}] at {sym.file.name if sym.file else '?'}:{sym.line}:{sym.col}"


def match_one(ws: Workspace, expected: dict[str, Any], what: str):
    """Exactly one symbol must agree with every column stated in the row."""
    known = set(sym_facts(ws.symbols()[0]).keys()) if ws.symbols() else set()
    unknown = set(expected) - known if known else set()
    assert not unknown, (
        f"unknown symbol column(s) {sorted(unknown)}; known: {sorted(known)}"
    )

    hits = [
        s
        for s in ws.symbols()
        if all(sym_facts(s)[k] == v for k, v in expected.items())
    ]
    if len(hits) == 1:
        return hits[0]

    # Build a focused diff against the closest candidate (same name, if any).
    name = expected.get("qual_name") or expected.get("spelling") or expected.get("usr")
    near = [
        s for s in ws.symbols() if name in (s.name, s.spelling, s.usr) or name is None
    ]
    detail = "\n".join(
        "      " + json.dumps({k: sym_facts(s).get(k) for k in expected}, default=str)
        for s in near
    )
    raise AssertionError(
        f"{what}: expected exactly 1 symbol matching\n"
        f"      {json.dumps(expected, default=str)}\n"
        f"    but found {len(hits)}. Actual rows with a matching name:\n"
        f"{detail or '      (no symbol with that name)'}\n"
        f"    all indexed symbols:\n"
        + "\n".join(f"      {describe(s)}" for s in ws.symbols())
    )
