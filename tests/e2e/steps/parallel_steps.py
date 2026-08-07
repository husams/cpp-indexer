"""B-006: steps for driving the cidx pipeline with more than one worker.

The rest of the suite stages exactly one fixture per scenario and runs the
pipeline with no extra flags (see steps/pipeline_steps.py). Reproducing B-006
needs two things those steps cannot express:

  * **two translation units in one workspace.** The runner collapses a
    one-worker plan onto the serial path, so a single-unit corpus never creates
    a worker thread and can never reproduce the bug.
  * **extra flags on `cidx index`.** `--jobs N` is the whole point, and the
    default (no flag) automatic policy needs its own scenario because that is
    what a user actually hits.

The profile-inspection steps give AC 4238 -- "the automatic policy completes
AND writes its --profile-json telemetry" -- an executable verification path. A
clean exit alone says nothing about telemetry and cannot even show the run took
the parallel path.
"""

from __future__ import annotations

import json
import os
import shutil
from pathlib import Path

import pytest
from pytest_bdd import given, parsers, then, when

from .paths import FIXTURES_DIR
from .workspace import Workspace


@given(
    parsers.parse(
        'a clean index workspace for fixtures "{fixtures}" compiled as {std}'
    )
)
def a_clean_multi_unit_workspace(
    workspace: Workspace, fixtures: str, std: str
) -> Workspace:
    """Stage two or more fixtures into one workspace and one compile database.

    Deliberately a separate step from the single-fixture one rather than a
    generalisation of it: `workspace.source` and `workspace.fixture` are
    scalars that the rest of the suite's steps read, and quietly redefining
    them to mean "the first of several" would make every existing scenario's
    failure messages lie.
    """
    names = [name.strip() for name in fixtures.split(",") if name.strip()]
    assert len(names) >= 2, (
        "this step exists to build a multi-unit corpus; use the single-fixture "
        "step for one unit"
    )

    entries = []
    for name in names:
        src = FIXTURES_DIR / name
        assert src.is_file(), (
            f"fixture {name!r} not found in {FIXTURES_DIR}; "
            f"available: {sorted(p.name for p in FIXTURES_DIR.glob('*.cpp'))}"
        )
        staged = workspace.src_dir / name
        shutil.copyfile(src, staged)
        entries.append(
            {
                "directory": str(workspace.src_dir),
                "file": str(staged),
                "arguments": ["clang++", f"-std={std.strip().lower()}", "-c", name],
            }
        )

    workspace.fixture = ", ".join(names)
    workspace.source = workspace.src_dir / names[0]
    (workspace.root / "compile_commands.json").write_text(
        json.dumps(entries, indent=2) + "\n"
    )
    assert not workspace.db.exists(), "workspace must start without an index.db"
    return workspace


@when(parsers.parse('I build the index with the cidx CLI passing "{flags}"'))
def build_the_index_with_flags(workspace: Workspace, flags: str) -> None:
    """The canonical pipeline, with extra flags on the `index` step only.

    `run` rather than `run_ok`: a SIGBUS is exactly what this scenario is
    looking for, and `run_ok`'s assertion would report it as an opaque
    "failed with exit -10" before the dedicated step below can explain that a
    negative return code means a signal. `resolve` is skipped when `index`
    did not succeed, because resolving a half-written index tests nothing.

    Path-valued flags are resolved against the workspace so a scenario can
    write `--profile-json profile.json` without knowing the temporary
    directory.
    """
    workspace.run_ok("init")
    workspace.run_ok("import", "--db", str(workspace.root), "--name", "fixture")

    argv: list[str] = []
    resolve_next = False
    for token in flags.split():
        argv.append(str(workspace.root / token) if resolve_next else token)
        resolve_next = token in {"--profile-json", "--profile-sqlite-config"}

    proc = workspace.run("index", *argv)
    if proc.returncode == 0:
        workspace.run_ok("resolve")


@then("the cidx CLI exited cleanly")
def the_cli_exited_cleanly(workspace: Workspace) -> None:
    """Assert no command died on a signal, naming the signal when one did.

    A plain "exit != 0" assertion is not good enough here. On POSIX,
    subprocess reports a signal death as a negative return code, and the whole
    point of B-006 is that the failure is a signal with no diagnostic
    attached: without naming it, the report is an empty stdout and an
    unexplained number.
    """
    import signal

    for command in workspace.commands:
        code = command["returncode"]
        argv = " ".join(command["argv"])
        if code < 0:
            name = signal.Signals(-code).name
            raise AssertionError(
                f"`{argv}` was killed by {name} ({code}).\n"
                f"  A worker crashed rather than reporting a failure: there is "
                f"no diagnostic and no telemetry to inspect.\n"
                f"  stdout was {len(command['stdout'])} bytes, stderr "
                f"{len(command['stderr'])} bytes."
            )
        assert code == 0, (
            f"`{argv}` failed with exit {code}\n"
            f"--- stdout ---\n{command['stdout']}\n"
            f"--- stderr ---\n{command['stderr']}"
        )


# --------------------------------------------------------------------------
# AC 4238: the automatic policy must complete AND write its telemetry.
#
# `--profile-json` is written at the end of a successful run, so its absence is
# precisely the observable a crashed or aborted run leaves behind. Reading it
# back also identifies which path ran: `parallel.*` counters exist only on the
# parallel runner's reporting path (src/index/parallel_index.cpp), so a serial
# fallback is visible rather than silently indistinguishable from success.
# --------------------------------------------------------------------------


def _profile(workspace: Workspace, name: str) -> dict:
    path = Path(workspace.root / name)
    assert path.is_file(), (
        f"no profile at {path}: `cidx index --profile-json` did not write its "
        f"telemetry. A run that dies on a signal leaves exactly this evidence "
        f"behind -- nothing."
    )
    try:
        return json.loads(path.read_text())
    except json.JSONDecodeError as exc:  # truncated by a crash mid-write
        raise AssertionError(f"{path} is not valid JSON: {exc}") from exc


@then(parsers.parse('the profile JSON "{name}" exists and parses'))
def the_profile_exists_and_parses(workspace: Workspace, name: str) -> None:
    profile = _profile(workspace, name)
    assert profile.get("schema_version"), f"{name} has no schema_version"
    assert "summary" in profile, f"{name} has no summary section"


@then(
    parsers.parse(
        'the profile JSON "{name}" shows the automatic policy ran more than one worker'
    )
)
def the_profile_shows_multiple_workers(workspace: Workspace, name: str) -> None:
    """Assert the default policy really planned a parallel run on this host.

    On a single-CPU runner the automatic policy legitimately plans one worker
    and the runner takes the serial path, which is not what AC 4238 is about
    ("on a multi-core host"). That case is skipped with the host's core count
    named -- never passed silently, because a silent pass here would report
    the criterion as verified on a machine that cannot exercise it.
    """
    cpus = os.cpu_count() or 1
    if cpus < 2:
        pytest.skip(f"automatic policy needs a multi-core host; this one has {cpus} CPU")

    counters = _profile(workspace, name).get("summary", {}).get("counters", {})
    parallel = {k: v for k, v in counters.items() if k.startswith("parallel.")}
    assert parallel, (
        f"{name} carries no parallel.* counters on a {cpus}-CPU host: the "
        f"automatic policy took the serial path, so this run did not exercise "
        f"the defect."
    )
    workers = parallel.get("parallel.workers")
    assert workers is not None and workers >= 2, (
        f"automatic policy planned {workers} worker(s) on a {cpus}-CPU host"
    )


@then(parsers.parse('the profile JSON "{name}" reports {count:d} published units'))
def the_profile_reports_published_units(
    workspace: Workspace, name: str, count: int
) -> None:
    counters = _profile(workspace, name).get("summary", {}).get("counters", {})
    published = counters.get("parallel.items_published")
    assert published == count, (
        f"parallel.items_published was {published}, expected {count}: the run "
        f"exited cleanly but did not publish every unit"
    )
