"""Fixtures for the cidx BDD end-to-end suite; the steps live in `steps/`.

Contract of this suite
----------------------
* The index is produced **only** through the `cidx` command line -- the same
  four-command pipeline a user runs (`init` -> `import` -> `index` -> `resolve`).
  No Python indexing, no direct Storage writes, no libclang in-process.
* Every scenario gets its **own clean `index.db`** in its own `tmp_path`
  workspace, so no scenario can observe another's rows.
* Assertions go exclusively through the public Python graph-query API
  (`indexer.query.GraphQuery`).
* The *expected* symbols, edges and signature facts are stated in the Gherkin
  feature files as data tables. The step modules only know how to compare; they
  never hard-code what a fixture should contain.

Layout: this file holds the two pytest fixtures and nothing else. Every step
definition and every helper lives in one module per concern under `steps/`,
listed in `steps/__init__.py`.

Table cell conventions (used by every `Then` table)
---------------------------------------------------
    true / false      -> bool
    -                 -> None (absent / not set)
    "" (empty cell)   -> None
    123               -> int
    anything else     -> str

Symbol selectors (the quoted name in scalar steps and in edge tables)
---------------------------------------------------------------------
    add                     qualified name, or spelling; must be unique
    add@11                  ... narrowed to the symbol declared on line 11
    add@11:1                ... narrowed to line 11, column 1
    usr:c:@F@add<#d>#d#d#   exact USR (always unambiguous)
"""

from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import Iterator

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

# Star import: this is what registers the step definitions with pytest-bdd.
from steps import *  # noqa: E402,F401,F403
from steps import REPO_ROOT, Workspace  # noqa: E402


@pytest.fixture(scope="session")
def cidx_bin() -> Path:
    """Path to the cidx binary under test ($CIDX_BIN, else build/cidx)."""
    env = os.environ.get("CIDX_BIN")
    candidate = Path(env) if env else REPO_ROOT / "build" / "cidx"
    if not candidate.is_file() or not os.access(candidate, os.X_OK):
        pytest.fail(
            f"cidx binary not found at {candidate} -- build it first "
            f"(cmake -S . -B build && cmake --build build -j) or set $CIDX_BIN"
        )
    return candidate.resolve()


@pytest.fixture
def workspace(tmp_path: Path, cidx_bin: Path) -> Iterator[Workspace]:
    """A clean, per-scenario workspace holding its own index.db."""
    ws = Workspace(
        root=tmp_path / "ws",
        cache=tmp_path / "ws" / "cache",
        src_dir=tmp_path / "ws" / "src",
        cidx=cidx_bin,
    )
    ws.src_dir.mkdir(parents=True)
    ws.cache.mkdir(parents=True)
    yield ws
    ws.close()
