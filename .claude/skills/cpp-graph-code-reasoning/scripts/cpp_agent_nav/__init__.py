"""Reusable C++ code-structure navigation APIs for agents.

Typical use:

    from cpp_agent_nav import NavigationSession

    nav = NavigationSession(db_path=".agent-nav.sqlite3", session="query")
    nav.init(repo_root=".")
    candidates = nav.resolve_symbol("src/foo.cc", "Foo::bar", focus=True)
    snippet = nav.snippet("src/foo.cc", "Foo::bar", char_budget=2200)
    graph_nodes = nav.graph_lookup("indradb://localhost:27615", "qualified_name", "Foo::bar", focus=True)
    callers = nav.lookahead("indradb://localhost:27615", direction="inbound", edge_kind="CALLS")
"""

from .api import (
    DEFAULT_BUDGET,
    DEFAULT_DB,
    DEFAULT_SESSION,
    DEFAULT_SNIPPET_CHARS,
    EdgeRef,
    IndraDBAPI,
    LibclangAPI,
    NavStore,
    NavigationSession,
    NodeRef,
    allocation_hints,
    profile_frames,
    spec_outline,
)

__all__ = [
    "DEFAULT_BUDGET",
    "DEFAULT_DB",
    "DEFAULT_SESSION",
    "DEFAULT_SNIPPET_CHARS",
    "EdgeRef",
    "IndraDBAPI",
    "LibclangAPI",
    "NavStore",
    "NavigationSession",
    "NodeRef",
    "allocation_hints",
    "profile_frames",
    "spec_outline",
]
