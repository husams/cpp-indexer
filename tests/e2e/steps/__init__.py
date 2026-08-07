"""Step definitions and their support code, one module per concern.

`conftest.py` star-imports this package, which is how pytest-bdd finds the
steps: every `@given/@when/@then` registers a `pytestbdd_stepdef_*` fixture in
its defining module, and this package re-exports those into the conftest
namespace, where pytest collects them.

Step modules
    pipeline_steps    GIVEN the fixture workspace / WHEN the cidx CLI runs
    queryplan_steps   public Python QueryPlan execution and result assertions
    parallel_steps    multi-unit workspaces and `cidx index` worker flags
    index_steps       index-level facts (database, resolve, file/symbol totals)
    symbol_steps      symbol rows: membership, exhaustiveness, spans
    signature_steps   the signature/type tier: types, parameters, templates
    edge_steps        the edge graph: counts, relationship tables, kind totals
    edge_site_steps   edge sites: where a relationship is written in the source
    callgraph_steps   callers and callees
    hierarchy_steps   bases, subclasses, overrides and dynamic dispatch
    provenance_steps  receiver provenance and query-time devirtualization
    cli_steps         CLI output and read-only cidx subcommands

Support modules (no steps)
    paths             filesystem anchors, and python/ on sys.path
    workspace         the per-scenario Workspace: CLI runner + query handle
    tables            Gherkin table parsing and row-list comparison
    symbol_facts      the comparable view of a symbol, and row matching
    edge_facts        rendering and lookup helpers for edges and sites
"""

from __future__ import annotations

from types import ModuleType

from . import (
    callgraph_steps,
    cli_steps,
    edge_site_steps,
    edge_steps,
    hierarchy_steps,
    index_steps,
    parallel_steps,
    pipeline_steps,
    provenance_steps,
    queryplan_steps,
    signature_steps,
    symbol_steps,
)
from .paths import E2E_DIR, FIXTURES_DIR, REPO_ROOT
from .workspace import Workspace

_STEP_MODULES: tuple[ModuleType, ...] = (
    pipeline_steps,
    parallel_steps,
    queryplan_steps,
    index_steps,
    symbol_steps,
    signature_steps,
    edge_steps,
    edge_site_steps,
    callgraph_steps,
    hierarchy_steps,
    provenance_steps,
    cli_steps,
)

_STEP_FIXTURE_PREFIX = "pytestbdd_stepdef_"


def _export_step_definitions() -> list[str]:
    """Re-export every module's step fixtures, refusing to shadow one."""
    exported: dict[str, ModuleType] = {}
    for module in _STEP_MODULES:
        for name, value in vars(module).items():
            if not name.startswith(_STEP_FIXTURE_PREFIX):
                continue
            assert name not in exported, (
                f"{module.__name__} and {exported[name].__name__} define the same "
                f"step; one would shadow the other ({name})"
            )
            exported[name] = module
            globals()[name] = value
    return sorted(exported)


__all__ = [
    "E2E_DIR",
    "FIXTURES_DIR",
    "REPO_ROOT",
    "Workspace",
    *_export_step_definitions(),
]
