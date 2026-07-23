"""GIVEN / WHEN: stage the fixture and drive the cidx CLI pipeline."""

from __future__ import annotations

import json
import shutil

from pytest_bdd import given, parsers, when

from .paths import FIXTURES_DIR
from .workspace import Workspace


def _stage_fixture(workspace: Workspace, fixture: str, std: str) -> Workspace:
    src = FIXTURES_DIR / fixture
    assert src.is_file(), (
        f"fixture {fixture!r} not found in {FIXTURES_DIR}; "
        f"available: {sorted(p.name for p in FIXTURES_DIR.glob('*.cpp'))}"
    )
    workspace.fixture = fixture
    workspace.source = workspace.src_dir / fixture
    shutil.copyfile(src, workspace.source)

    compile_commands = [
        {
            "directory": str(workspace.src_dir),
            "file": str(workspace.source),
            "arguments": ["clang++", f"-std={std}", "-c", fixture],
        }
    ]
    (workspace.root / "compile_commands.json").write_text(
        json.dumps(compile_commands, indent=2) + "\n"
    )
    assert not workspace.db.exists(), "workspace must start without an index.db"
    return workspace


@given(parsers.parse('a clean index workspace for fixture "{fixture}"'))
def a_clean_workspace(workspace: Workspace, fixture: str) -> Workspace:
    return _stage_fixture(workspace, fixture, "c++17")


@given(parsers.parse('a clean index workspace for fixture "{fixture}" compiled as {std}'))
def a_clean_workspace_with_std(workspace: Workspace, fixture: str, std: str) -> Workspace:
    return _stage_fixture(workspace, fixture, std.strip().lower())


@when("I build the index with the cidx CLI")
def build_the_index(workspace: Workspace) -> None:
    """The canonical four-command pipeline, exactly as documented for users."""
    workspace.run_ok("init")
    workspace.run_ok("import", "--db", str(workspace.root), "--name", "fixture")
    workspace.run_ok("index")
    workspace.run_ok("resolve")
