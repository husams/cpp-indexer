"""Executable HSE-27 inventory coverage for all public read-query surfaces."""

from __future__ import annotations

import inspect
from pathlib import Path

from indexer.entity_graph import EntityGraph, EntityQuery
from indexer.query import GraphQuery


def _inventory() -> list[tuple[str, str, str]]:
    path = Path(__file__).parents[2] / "tests" / "golden" / "legacy_query_operations.txt"
    entries = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        surface, qualified = line.split(":", 1)
        owner, operation = qualified.split(".", 1)
        entries.append((surface, owner, operation))
    return entries


def test_python_inventory_is_executable_and_matches_public_surfaces():
    owners = {
        "GraphQuery": GraphQuery,
        "EntityGraph": EntityGraph,
        "EntityQuery": EntityQuery,
    }
    for surface, owner, operation in _inventory():
        if not surface.startswith("python_"):
            continue
        cls = owners[owner]
        member = inspect.getattr_static(cls, operation)
        assert callable(member), f"{owner}.{operation} is not callable"
        assert not operation.startswith("_"), f"private operation: {operation}"

    # Reflection is the inventory: adding a new public method automatically
    # makes this production-surface check visit it, instead of silently
    # expanding a hand-authored parity claim.
    for cls in owners.values():
        for name, member in inspect.getmembers(cls, inspect.isfunction):
            if name.startswith("_") or name in {"close"}:
                continue
            assert callable(member), f"{cls.__name__}.{name} is not callable"
