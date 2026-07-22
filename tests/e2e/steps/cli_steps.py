"""THEN: the CLI surface -- what the commands printed, and read-only queries."""

from __future__ import annotations

import shlex

from pytest_bdd import parsers, then

from .tables import rows
from .workspace import Workspace


@then(parsers.parse('the CLI output contains "{needle}"'))
def cli_output_contains(workspace: Workspace, needle: str) -> None:
    assert needle in workspace.cli_output, (
        f"{needle!r} not found in CLI output:\n{workspace.cli_output}"
    )


@then(parsers.parse("`cidx {argv}` lists:"))
def cidx_lists(workspace: Workspace, argv: str, datatable: list[list[str]]) -> None:
    """Run a read-only cidx subcommand and check each expected line appears."""
    proc = workspace.run_ok(*shlex.split(argv))
    for row in rows(datatable):
        assert set(row) == {"line"}, (
            f"cidx output tables require exactly one 'line' column; got {sorted(row)}"
        )
        needle = row["line"]
        assert needle in proc.stdout, (
            f"`cidx {argv}` output missing {needle!r}:\n{proc.stdout}"
        )
