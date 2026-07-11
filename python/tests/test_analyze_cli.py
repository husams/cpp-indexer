"""CLI tests for `cidx analyze` (Souffle Datalog analyses over the index).

Hermetic paths (arg validation, --list, --export-facts) always run; tests
that execute the standalone `souffle` interpreter are skipped when it is not
installed.
"""

import json
import os
import sqlite3

import pytest

from indexer import cli, souffle


def run(argv, capsys):
    rc = cli.main(argv)
    cap = capsys.readouterr()
    return rc, cap.out, cap.err


needs_souffle = pytest.mark.skipif(
    souffle.find_souffle() is None,
    reason="souffle interpreter not installed",
)


# --- hermetic: flag validation and --list -----------------------------------


def test_list_rules(capsys):
    rc, out, err = run(["analyze", "--list"], capsys)
    assert rc == 0 and err == ""
    doc = json.loads(out)
    assert [r["name"] for r in doc["rules"]] == ["callgraph", "cycles", "unused"]
    assert all(r["description"] for r in doc["rules"])


def test_exactly_one_mode_required(resolved_db, capsys):
    rc, out, err = run(["analyze", "--db", resolved_db], capsys)
    assert rc == 2
    assert "exactly one of --list, --export-facts, --rule, or --rules-file" in err

    rc, out, err = run(
        ["analyze", "--list", "--rule", "cycles", "--db", resolved_db], capsys
    )
    assert rc == 2


def test_jobs_must_be_positive(resolved_db, capsys):
    rc, out, err = run(
        ["analyze", "--rule", "cycles", "--jobs", "0", "--db", resolved_db], capsys
    )
    assert rc == 2
    assert "error: --jobs must be at least 1" in err


def test_unknown_rule(resolved_db, capsys):
    rc, out, err = run(["analyze", "--rule", "nope", "--db", resolved_db], capsys)
    assert rc == 1
    assert "error: unknown rule: nope (see cidx analyze --list)" in err


def test_missing_index(tmp_path, capsys):
    missing = str(tmp_path / "absent.db")
    rc, out, err = run(["analyze", "--rule", "cycles", "--db", missing], capsys)
    assert rc == 1
    assert f"error: index not found at {missing}" in err


def test_missing_rules_file(resolved_db, tmp_path, capsys):
    absent = str(tmp_path / "absent.dl")
    rc, out, err = run(
        ["analyze", "--rules-file", absent, "--db", resolved_db], capsys
    )
    assert rc == 1
    assert f"error: rules file not found: {absent}" in err


# --- hermetic: fact export ----------------------------------------------------


def test_export_facts(resolved_db, tmp_path, capsys):
    out_dir = str(tmp_path / "facts")
    rc, out, err = run(
        ["analyze", "--export-facts", out_dir, "--db", resolved_db], capsys
    )
    assert rc == 0 and err == ""

    names = [name for name, _ in souffle.FACT_QUERIES]
    for name in names:
        assert os.path.isfile(os.path.join(out_dir, name + ".facts"))
    with open(os.path.join(out_dir, "cidx_facts.dl"), encoding="utf-8") as fh:
        assert fh.read() == souffle.prelude_text()

    conn = sqlite3.connect(resolved_db)
    n_symbols = conn.execute("SELECT COUNT(*) FROM symbol").fetchone()[0]
    n_edges = conn.execute("SELECT COUNT(*) FROM edge").fetchone()[0]
    conn.close()
    with open(os.path.join(out_dir, "symbol.facts"), encoding="utf-8") as fh:
        sym_lines = fh.read().splitlines()
    assert len(sym_lines) == n_symbols
    assert all(len(line.split("\t")) == 8 for line in sym_lines)
    with open(os.path.join(out_dir, "edge.facts"), encoding="utf-8") as fh:
        edge_lines = fh.read().splitlines()
    assert len(edge_lines) == n_edges
    assert all(len(line.split("\t")) == 5 for line in edge_lines)

    total = 0
    for name in names:
        with open(os.path.join(out_dir, name + ".facts"), encoding="utf-8") as fh:
            total += len(fh.read().splitlines())
    assert out == f"{out_dir}: {len(names)} fact files, {total} rows\n"


def test_export_facts_rejects_wrong_schema(tmp_path, capsys):
    bogus = str(tmp_path / "bogus.db")
    conn = sqlite3.connect(bogus)
    conn.execute("CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT)")
    conn.execute("INSERT INTO meta VALUES ('schema_version', '1')")
    conn.commit()
    conn.close()
    rc, out, err = run(
        ["analyze", "--export-facts", str(tmp_path / "f"), "--db", bogus], capsys
    )
    assert rc == 1
    assert "error: unsupported index schema version: 1" in err


# --- souffle-backed runs -------------------------------------------------------


@needs_souffle
def test_callgraph_rule(resolved_db, capsys):
    rc, out, err = run(
        ["analyze", "--rule", "callgraph", "--db", resolved_db], capsys
    )
    assert rc == 0, err
    doc = json.loads(out)
    assert doc["rule"] == "callgraph"
    assert doc["db"] == os.path.abspath(resolved_db)
    # Direct `calls` edges in the fixture graph (conftest._seed) plus the
    # dispatch_calls edges resolve_pass() adds for the virtual draw call.
    direct = [
        ["c:@F@main", "main", "c:@F@helper", "helper"],
        ["c:@F@helper", "helper", "c:@F@compute", "compute"],
        ["c:@F@helper", "helper", "c:@F@ext_fn", ""],
        ["c:@F@render", "render", "c:@S@Base@F@draw#", "draw"],
        ["c:@F@render", "render", "c:@S@Derived@F@draw#", "draw"],
        ["c:@F@render", "render", "c:@S@Derived2@F@draw#", "draw"],
    ]
    assert doc["relations"]["call"] == sorted(direct)
    assert doc["relations"]["call_transitive"] == sorted(
        direct
        + [
            ["c:@F@main", "main", "c:@F@compute", "compute"],
            ["c:@F@main", "main", "c:@F@ext_fn", ""],
        ]
    )


@needs_souffle
def test_cycles_rule_empty(resolved_db, capsys):
    rc, out, err = run(["analyze", "--rule", "cycles", "--db", resolved_db], capsys)
    assert rc == 0, err
    doc = json.loads(out)
    assert doc["relations"] == {"cycle_edge": [], "cycle_member": []}


@needs_souffle
def test_unused_rule(resolved_db, tmp_path, capsys):
    rc, out, err = run(["analyze", "--rule", "unused", "--db", resolved_db], capsys)
    assert rc == 0, err
    doc = json.loads(out)
    # `render` is the only defined, never-called, non-override function
    # (main is excluded by name; ext_fn is an unresolved stub).
    repo = str(tmp_path / "repo")
    assert doc["relations"]["unused"] == [
        ["c:@F@render", "render", f"{repo}/lib.c", "60"]
    ]


@needs_souffle
def test_custom_rules_file(resolved_db, tmp_path, capsys):
    rules = tmp_path / "count_calls.dl"
    rules.write_text(
        ".decl calls_count(n:number)\n"
        'calls_count(n) :- n = count : { edge(e, _, _, k, _), '
        'edge_kind(k, "calls"), e != 0 }.\n'
        ".output calls_count\n",
        encoding="utf-8",
    )
    rc, out, err = run(
        ["analyze", "--rules-file", str(rules), "--db", resolved_db], capsys
    )
    assert rc == 0, err
    doc = json.loads(out)
    assert doc["rule"] == str(rules)
    assert doc["relations"] == {"calls_count": [["4"]]}


@needs_souffle
def test_bad_program_reports_souffle_error(resolved_db, tmp_path, capsys):
    rules = tmp_path / "broken.dl"
    rules.write_text(".decl broken(\n", encoding="utf-8")
    rc, out, err = run(
        ["analyze", "--rules-file", str(rules), "--db", resolved_db], capsys
    )
    assert rc == 1
    assert "error: souffle failed (exit " in err
