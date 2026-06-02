"""
cxg_graph — tiny helper for querying a cpp-indexer CodexGraph in IndraDB.

This wraps the project's own `cpp_agent_nav.IndraDBAPI` (which already lifts the
4 MB gRPC receive cap — see the cpp-graph-code-reasoning skill) and adds a few
convenience methods plus a SQLite resolver for the compact-ingest id maps.

Environment
-----------
  CXG_DB_URI     graph location           (default: indradb://localhost:27652)
  CXG_SYMBOL_DB  path to cxg-symbols.db   (optional; enables file_id/symbol_id resolution)

Schema cheat-sheet
------------------
Vertex type label (`t`) == node `kind`:
  MODULE CLASS METHOD FUNCTION FIELD PARAMETER TYPE ENUM ENUMERATOR
  NAMESPACE TYPEDEF MACRO HEADER GLOBAL_VARIABLE REPO
Property conventions that bite:
  * For METHOD/FUNCTION, `qualified_name` embeds the PARAMETER LIST but no scope
    e.g. "offsets_store(std::vector<TopicPartition *> &)" — so look up by `name`.
  * `name` is the bare identifier. One `name` may match several overloaded nodes.
  * No `file_path` property under compact-ingest — only `file_id` (int) into
    cxg-symbols.db. Same for `symbol_id` -> usr.
  * `CALLS`/`USES` edges originate from METHOD/FUNCTION nodes, never CLASS.
  * Function/method nodes also carry the full source in the `code` property.
Edge types: INHERITS OVERRIDES CALLS USES HAS_METHOD HAS_PARAM RETURNS
            OF_TYPE CONTAINS POINTS_TO INCLUDES EXTERNAL_REF
"""
from __future__ import annotations

import os
import sqlite3
import sys
from pathlib import Path

# Locate the project's graph engine relative to this file (repo-root/.claude/...).
_ENGINE = Path(__file__).resolve().parents[1] / ".claude/skills/cpp-graph-code-reasoning/scripts"
if str(_ENGINE) not in sys.path:
    sys.path.insert(0, str(_ENGINE))
from cpp_agent_nav import IndraDBAPI  # noqa: E402

DEFAULT_URI = os.environ.get("CXG_DB_URI", "indradb://localhost:27652")

# Edge kinds by which a type/class is referenced (inbound = "who depends on this type").
TYPE_REF_EDGES = ("USES", "OF_TYPE", "POINTS_TO")


class Cxg:
    """Thin convenience layer over IndraDBAPI."""

    def __init__(self, uri: str = DEFAULT_URI):
        self.api = IndraDBAPI(uri)

    # ── lookups ──────────────────────────────────────────────────────────────
    def by_name(self, name: str, budget: int = 60):
        return self.api.lookup_property("name", name, budget=budget)["rows"]

    def by_qualified(self, qn: str, budget: int = 60):
        return self.api.lookup_property("qualified_name", qn, budget=budget)["rows"]

    def of_kind(self, kind: str, budget: int = 100000):
        """All vertices of a node kind (CLASS, FUNCTION, ...)."""
        return self.api.lookup_property("kind", kind, budget=budget)["rows"]

    def functions(self, name: str):
        """Function/method nodes named `name` (skips CLASS/TYPE/etc)."""
        return [r for r in self.by_name(name) if r["t"] in ("METHOD", "FUNCTION")]

    def one_class(self, qn: str):
        rows = self.by_qualified(qn)
        return next((r for r in rows if r["t"] == "CLASS"), None)

    # ── traversal ────────────────────────────────────────────────────────────
    def neighbors(self, vid, direction: str, edge_kind: str | None = None, budget: int = 300):
        return self.api.neighbors(vid, direction, edge_kind, budget=budget)["rows"]

    def all_props(self) -> dict[str, dict]:
        """vertex-id -> {id, t, <props>} for every vertex (one full scan)."""
        ix, client = self.api.indradb, self.api.client
        by_id: dict[str, dict] = {}
        for batch in client.get(ix.AllVertexQuery().properties()):
            for vp in batch:
                d = by_id.setdefault(str(vp.vertex.id), {"id": str(vp.vertex.id), "t": str(vp.vertex.t)})
                for np in vp.props:
                    d[np.name] = np.value
        return by_id

    # ── call graph ───────────────────────────────────────────────────────────
    def callees(self, name: str) -> list[str]:
        """Functions/methods CALLED BY `name` (union over overloads)."""
        out: list[str] = []
        for fn in self.functions(name):
            for r in self.neighbors(fn["id"], "outbound", "CALLS"):
                out.append(r["node"]["properties"].get("name"))
        return sorted({x for x in out if x})

    def callers(self, name: str) -> list[str]:
        """Functions/methods that CALL `name` (union over overloads)."""
        out: list[str] = []
        for fn in self.functions(name):
            for r in self.neighbors(fn["id"], "inbound", "CALLS"):
                out.append(r["node"]["properties"].get("name"))
        return sorted({x for x in out if x})


class SymbolMap:
    """Resolves compact-ingest integer ids via cxg-symbols.db (files, symbols)."""

    def __init__(self, db_path: str | None = None):
        db_path = db_path or os.environ.get("CXG_SYMBOL_DB")
        self.conn = sqlite3.connect(db_path) if db_path and os.path.exists(db_path) else None
        self.db_path = db_path

    def file(self, file_id) -> str | None:
        if not self.conn or file_id in (None, ""):
            return None
        row = self.conn.execute("SELECT path FROM files WHERE id=?", (int(file_id),)).fetchone()
        return row[0] if row else None

    def usr(self, symbol_id) -> str | None:
        if not self.conn or symbol_id in (None, ""):
            return None
        row = self.conn.execute("SELECT usr FROM symbols WHERE id=?", (int(symbol_id),)).fetchone()
        return row[0] if row else None
