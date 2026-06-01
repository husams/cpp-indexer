"""Importable functional-analysis navigator module.

DO NOT read this file. Build scripts against the API documented in
``cpp-functional-analysis/reference/API_REFERENCE.md``. This module is a thin,
stateful wrapper over the shared ``cpp_agent_nav`` engine; the reference is a
complete substitute for its source.

    import sys
    sys.path.insert(0, ".claude/skills/cpp-functional-analysis/scripts")
    from functional_nav import FunctionalSession

    nav = FunctionalSession(compile_commands="build/compile_commands.json")
    ctx = nav.spec_context(
        spec="/path/to/spec.md",
        source="src/foo.cc",
        name="Namespace::Class::method",
    )
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Any

# The shared navigation engine lives in the sibling graph-reasoning skill.
_ENGINE = Path(__file__).resolve().parents[2] / "cpp-graph-code-reasoning" / "scripts"
if str(_ENGINE) not in sys.path:
    sys.path.insert(0, str(_ENGINE))

from cpp_agent_nav import DEFAULT_BUDGET, DEFAULT_DB, NavigationSession  # noqa: E402

__all__ = ["FunctionalSession", "DEFAULT_BUDGET", "DEFAULT_DB"]


class FunctionalSession(NavigationSession):
    """NavigationSession specialized for functional / spec-coverage analysis.

    Inherits the full engine surface (resolve_symbol, snippet, graph_lookup,
    lookahead, frontier, step, checkpoint, backtrack, record_spec). Adds one
    composite helper, ``spec_context``, that captures the start-of-analysis
    evidence in SQLite and returns a small structured plan.
    """

    def __init__(
        self,
        db_path: str | Path = DEFAULT_DB,
        session: str = "functional",
        compile_commands: str | Path | None = None,
        default_budget: int = DEFAULT_BUDGET,
    ) -> None:
        super().__init__(db_path, session, compile_commands, default_budget)

    def spec_context(
        self,
        spec: str | Path | None = None,
        source: str | Path | None = None,
        name: str | None = None,
        focus: bool = True,
        budget: int | None = None,
    ) -> dict[str, Any]:
        """Record a bounded spec outline + optional implementation symbol.

        Returns ``{spec_outline, resolved_symbol_candidates, snapshot}``.
        Larger detail stays in SQLite; navigate further with the inherited
        snippet / graph_lookup / lookahead methods.
        """
        outline = self.record_spec(spec, budget) if spec else None

        resolved: list[dict[str, Any]] = []
        if source and name:
            nodes = self.resolve_symbol(source, name, focus=focus, budget=budget)
            resolved = [node.brief() for node in nodes]
            self.store.observe(
                "functional.symbol",
                f"Resolved {len(resolved)} candidate implementation symbol(s) for {name}",
                resolved[0]["key"] if resolved else None,
                {"source": str(source), "name": name},
            )

        return {
            "spec_outline": outline,
            "resolved_symbol_candidates": resolved,
            "snapshot": self.status(min(budget or self.default_budget, 8)),
        }
