"""Importable memory-optimization navigator module.

DO NOT read this file. Build scripts against the API documented in
``cpp-memory-optimization/reference/API_REFERENCE.md``. This module is a thin,
stateful wrapper over the shared ``cpp_agent_nav`` engine; the reference is a
complete substitute for its source.

    import sys
    sys.path.insert(0, ".claude/skills/cpp-memory-optimization/scripts")
    from memory_nav import MemorySession

    nav = MemorySession(compile_commands="build/compile_commands.json")
    triage = nav.triage(
        profile="/path/to/massif.txt",
        project_root=".",
        source="src/hot.cc",
        name="Namespace::hotFunction",
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

__all__ = ["MemorySession", "DEFAULT_BUDGET", "DEFAULT_DB"]


class MemorySession(NavigationSession):
    """NavigationSession specialized for memory-profile triage.

    Inherits the full engine surface (resolve_symbol, snippet, graph_lookup,
    lookahead, frontier, step, checkpoint, backtrack, record_profile). Adds one
    composite helper, ``triage``, that records profile evidence in SQLite and
    returns a small structured summary.
    """

    def __init__(
        self,
        db_path: str | Path = DEFAULT_DB,
        session: str = "memory",
        compile_commands: str | Path | None = None,
        default_budget: int = DEFAULT_BUDGET,
    ) -> None:
        super().__init__(db_path, session, compile_commands, default_budget)

    def triage(
        self,
        profile: str | Path,
        project_root: str | Path | None = None,
        source: str | Path | None = None,
        name: str | None = None,
        focus: bool = True,
        budget: int | None = None,
    ) -> dict[str, Any]:
        """Parse a bounded set of profile frames + optionally resolve a hot frame.

        Returns ``{profile_summary, resolved_hot_symbol_candidates, snapshot}``
        where ``profile_summary`` is
        ``{parsed_frames, project_owned_frames, top_project_frames}``. Larger
        detail stays in SQLite; navigate further with the inherited snippet /
        graph_lookup / lookahead methods.
        """
        eff_budget = budget or self.default_budget
        frames = self.record_profile(profile, project_root, eff_budget)
        project_frames = [f for f in frames if f.get("project_owned")]

        resolved: list[dict[str, Any]] = []
        if source and name:
            nodes = self.resolve_symbol(source, name, focus=focus, budget=budget)
            resolved = [node.brief() for node in nodes]
            self.store.observe(
                "memory.hot-symbol",
                f"Resolved {len(resolved)} candidate(s) for hot frame {name}",
                resolved[0]["key"] if resolved else None,
                {"source": str(source), "name": name},
            )

        return {
            "profile_summary": {
                "parsed_frames": len(frames),
                "project_owned_frames": len(project_frames),
                "top_project_frames": project_frames[:eff_budget],
            },
            "resolved_hot_symbol_candidates": resolved,
            "snapshot": self.status(min(eff_budget, 8)),
        }
