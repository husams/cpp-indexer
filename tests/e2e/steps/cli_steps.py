"""THEN: the CLI surface -- what the commands printed, and read-only queries."""

from __future__ import annotations

import json
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


@then(parsers.parse("`cidx {argv}` returns complete deterministic template JSON"))
def cidx_template_json(workspace: Workspace, argv: str) -> None:
    first = workspace.run_ok(*shlex.split(argv), "--json")
    second = workspace.run_ok(*shlex.split(argv), "--json")
    assert first.stdout == second.stdout
    document = json.loads(first.stdout)
    assert set(document) == {"symbol", "relationships", "template_params", "template_args"}
    assert all(set(row) == {"kind", "target"} for row in document["relationships"])
    assert all({
        "position", "param_kind", "kind_name", "name", "default",
        "type", "default_type", "default_ref",
    } == set(row) for row in document["template_params"])
    assert all({
        "position", "pack_index", "arg_kind", "kind_name", "literal",
        "ref_id", "type",
    } == set(row) for row in document["template_args"])
    assert document["relationships"] == sorted(
        document["relationships"],
        key=lambda row: (row["kind"], row["target"].get("name", ""), row["target"]["id"]),
    )
    assert document["template_params"] == sorted(
        document["template_params"], key=lambda row: row["position"]
    )
    assert document["template_args"] == sorted(
        document["template_args"],
        key=lambda row: (row["position"], row["pack_index"] if row["pack_index"] is not None else -1),
    )


@then(parsers.parse("`cidx {argv}` returns complete deterministic signature JSON"))
def cidx_signature_json(workspace: Workspace, argv: str) -> None:
    first = workspace.run_ok(*shlex.split(argv), "--json")
    second = workspace.run_ok(*shlex.split(argv), "--json")
    assert first.stdout == second.stdout
    document = json.loads(first.stdout)
    assert set(document) == {
        "symbol", "returns", "params", "of_type", "underlying_type", "slots",
    }
    type_keys = {
        "id", "spelling", "kind", "canonical", "decl_usr", "const",
        "volatile", "restrict", "layers",
    }
    slot_keys = {
        "role", "position", "pack_index", "name", "declared_type", "adjusted_type",
        "mode", "value_kind", "named_decl", "reference_semantics", "default", "default_origin",
    }
    for key in ("returns", "of_type", "underlying_type"):
        assert document[key] is None or set(document[key]) == type_keys
        if document[key] is not None:
            assert document[key]["layers"]
    for row in document["params"]:
        assert set(row) == {
            "position", "pack_index", "name", "type", "declared_type", "adjusted_type",
            "mode", "value_kind", "named_decl", "reference_semantics", "default", "default_origin",
        }
        for key in ("type", "declared_type", "adjusted_type"):
            assert row[key] is None or set(row[key]) == type_keys
            if row[key] is not None:
                assert row[key]["layers"]
    assert all(set(row) == slot_keys for row in document["slots"])
