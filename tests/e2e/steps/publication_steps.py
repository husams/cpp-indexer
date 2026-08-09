"""S-117: canonical E2E comparison for ordered windowed publication."""

from __future__ import annotations

import json
from pathlib import Path

from pytest_bdd import given, then, when

from .paths import REPO_ROOT
from .symbol_facts import sym_facts
from .workspace import Workspace


_CORPUS = (
    "src/query/exec.cpp",
    "src/query/plan.cpp",
    "src/query/cxq.cpp",
    "src/cli/commands.cpp",
    "src/ast/index_engine.cpp",
)


@given("the S-117 five-TU batch corpus", target_fixture="publication_corpus")
def the_s117_five_tu_batch_corpus() -> tuple[Path, ...]:
    compile_db = REPO_ROOT / "build" / "compile_commands.json"
    assert compile_db.is_file(), (
        f"compile database not found at {compile_db}; configure the CMake build first"
    )
    corpus = tuple(REPO_ROOT / relative for relative in _CORPUS)
    missing = [str(path) for path in corpus if not path.is_file()]
    assert not missing, f"S-117 corpus files are missing: {missing}"
    return corpus


def _canonical_tu_query(workspace: Workspace, translation_unit: Path) -> bytes:
    target = translation_unit.resolve()
    symbols = sorted(
        (
            symbol
            for symbol in workspace.symbols()
            if symbol.file is not None and Path(symbol.file).resolve() == target
        ),
        key=lambda symbol: (symbol.usr, symbol.name, symbol.kind),
    )
    assert symbols, f"canonical query returned no symbols for {translation_unit}"
    symbol_rows = sorted(
        (sym_facts(symbol) for symbol in symbols),
        key=lambda row: json.dumps(row, sort_keys=True),
    )
    return json.dumps(
        {"symbols": symbol_rows},
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


def _index_corpus(
    parent: Workspace,
    corpus: tuple[Path, ...],
    *,
    name: str,
    jobs: int,
) -> dict[str, bytes]:
    root = parent.root / name
    cache = root / "cache"
    src_dir = root / "src"
    cache.mkdir(parents=True)
    src_dir.mkdir()
    run = Workspace(root=root, cache=cache, src_dir=src_dir, cidx=parent.cidx)
    try:
        profile = root / "profile.json"
        run.run_ok("init")
        run.run_ok("import", "--db", str(REPO_ROOT / "build"), "--name", "cidx")
        run.run_ok(
            "index",
            *(str(path) for path in corpus),
            "--jobs",
            str(jobs),
            "--profile-json",
            str(profile),
        )
        run.run_ok("resolve")
        assert profile.is_file(), f"profile JSON was not written at {profile}"
        profile_data = json.loads(profile.read_text())
        if jobs > 1:
            counters = profile_data.get("summary", {}).get("counters", {})
            assert counters.get("parallel.workers") == jobs
            assert counters.get("parallel.items_published") == len(corpus)
        return {
            str(path.relative_to(REPO_ROOT)): _canonical_tu_query(run, path)
            for path in corpus
        }
    finally:
        run.close()


@when(
    "I index the corpus with five workers and with one worker using profile JSON",
    target_fixture="publication_outputs",
)
def index_parallel_and_serial(
    workspace: Workspace, publication_corpus: tuple[Path, ...]
) -> dict[str, dict[str, bytes]]:
    return {
        "parallel": _index_corpus(
            workspace, publication_corpus, name="parallel", jobs=5
        ),
        "serial": _index_corpus(workspace, publication_corpus, name="serial", jobs=1),
    }


@then("every translation unit has byte-identical canonical query output")
def every_translation_unit_matches(
    publication_outputs: dict[str, dict[str, bytes]],
) -> None:
    parallel = publication_outputs["parallel"]
    serial = publication_outputs["serial"]
    assert parallel.keys() == serial.keys()
    assert len(parallel) == len(_CORPUS)
    for translation_unit in sorted(parallel):
        assert parallel[translation_unit] == serial[translation_unit], (
            f"canonical query output differs for {translation_unit}\n"
            f"parallel={parallel[translation_unit].decode()}\n"
            f"serial={serial[translation_unit].decode()}"
        )
