"""The per-scenario indexing workspace: the CLI runner and the query handle."""

from __future__ import annotations

import os
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

from .paths import REPO_ROOT  # noqa: F401  (puts python/ on sys.path)

from indexer.query import GraphQuery


@dataclass
class Workspace:
    """One scenario's isolated indexing workspace and its query handle."""

    root: Path
    cache: Path
    src_dir: Path
    cidx: Path
    fixture: str = ""
    source: Optional[Path] = None
    commands: list[dict[str, Any]] = field(default_factory=list)
    _graph: Optional[GraphQuery] = None
    _symbols: Optional[list] = None

    # -- CLI ---------------------------------------------------------------- #

    @property
    def db(self) -> Path:
        return self.cache / "index.db"

    def run(self, *args: str) -> subprocess.CompletedProcess:
        """Run one cidx subcommand in the workspace with an isolated cache."""
        env = dict(os.environ)
        env["INDEXER_CACHE"] = str(self.cache)
        proc = subprocess.run(
            [str(self.cidx), *args],
            cwd=self.root,
            env=env,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.commands.append(
            {
                "argv": ["cidx", *args],
                "returncode": proc.returncode,
                "stdout": proc.stdout,
                "stderr": proc.stderr,
            }
        )
        return proc

    def run_ok(self, *args: str) -> subprocess.CompletedProcess:
        proc = self.run(*args)
        assert proc.returncode == 0, (
            f"`cidx {' '.join(args)}` failed with exit {proc.returncode}\n"
            f"--- stdout ---\n{proc.stdout}\n--- stderr ---\n{proc.stderr}"
        )
        return proc

    @property
    def cli_output(self) -> str:
        return "\n".join(c["stdout"] + c["stderr"] for c in self.commands)

    # -- query -------------------------------------------------------------- #

    @property
    def graph(self) -> GraphQuery:
        assert self.db.is_file(), f"no index database at {self.db}"
        if self._graph is None:
            self._graph = GraphQuery(str(self.db))
        return self._graph

    def close(self) -> None:
        if self._graph is not None:
            self._graph.close()
            self._graph = None

    # -- symbol access ------------------------------------------------------ #

    def symbols(self) -> list:
        """Every symbol, ordered by id (cached for the scenario)."""
        if self._symbols is None:
            g = self.graph
            expected_count = g.stats()["symbols"]
            self._symbols = sorted(
                g.find("", limit=expected_count), key=lambda symbol: symbol.id
            )
            assert len(self._symbols) == expected_count, (
                "public graph query returned an incomplete symbol set: "
                f"expected {expected_count}, got {len(self._symbols)}"
            )
        return self._symbols

    def resolve(self, selector: str):
        """Resolve a symbol selector (see conftest docstring) to exactly one Sym."""
        selector = selector.strip()
        syms = self.symbols()

        if selector.startswith("usr:"):
            want = selector[4:]
            hits = [s for s in syms if s.usr == want]
            if len(hits) == 1:
                return hits[0]
            raise AssertionError(
                f"selector {selector!r} matched {len(hits)} symbols; "
                f"known USRs: {sorted(s.usr for s in syms)}"
            )

        name, _, where = selector.partition("@")
        line = col = None
        if where:
            line_s, _, col_s = where.partition(":")
            line = int(line_s)
            col = int(col_s) if col_s else None

        def narrow(candidates: list) -> list:
            if line is not None:
                candidates = [s for s in candidates if s.line == line]
            if col is not None:
                candidates = [s for s in candidates if s.col == col]
            return candidates

        # Qualified name wins over spelling: a constructor's spelling is its
        # class name, so "PointClass" must mean the class, not PointClass().
        hits: list = []
        for tier in (
            [s for s in syms if s.name == name],
            [s for s in syms if s.spelling == name],
        ):
            hits = narrow(tier)
            if len(hits) == 1:
                return hits[0]
            if hits:
                break

        detail = "\n".join(
            f"    {s.name!r} kind={s.kind} at {s.line}:{s.col} usr={s.usr}"
            for s in syms
            if name in (s.name, s.spelling)
        )
        raise AssertionError(
            f"selector {selector!r} matched {len(hits)} symbols, expected 1.\n"
            f"  candidates with that name:\n{detail or '    (none)'}\n"
            f"  disambiguate with name@line, name@line:col, or usr:<usr>"
        )

    def edges(self) -> list[tuple]:
        """Every edge as (src Sym, Edge), deduplicated by edge id."""
        g = self.graph
        seen: dict[int, tuple] = {}
        for s in self.symbols():
            for e in g.edges_out(s, limit=5000):
                seen[e.edge_id] = (s, e)
        return [seen[k] for k in sorted(seen)]
