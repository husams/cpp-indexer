from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys

import pytest


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
PRODUCTION_BENCHMARK = REPOSITORY_ROOT / "benchmarks" / "indexing" / "production.py"


def load_benchmark():
    spec = importlib.util.spec_from_file_location(
        "hse103_production_benchmark", PRODUCTION_BENCHMARK
    )
    assert spec is not None
    assert spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_corpus_shapes_have_declared_owned_header_target(tmp_path: Path) -> None:
    benchmark = load_benchmark()
    baseline = benchmark.generate_corpus(
        tmp_path / "baseline", 8, "baseline", 6, "forward"
    )
    many = benchmark.generate_corpus(tmp_path / "many", 8, "many-headers", 6, "forward")
    fan_in = benchmark.generate_corpus(tmp_path / "fan-in", 8, "fan-in", 6, "forward")

    assert baseline.target_distinct_owned_headers == 2
    assert many.target_distinct_owned_headers == 6
    assert fan_in.target_distinct_owned_headers == 4
    assert (
        sum(
            '#include "high_fan_in.hpp"' in source.read_text(encoding="utf-8")
            for source in fan_in.sources
        )
        == 8
    )
    assert (
        sum(
            '#include "low_fan_in.hpp"' in source.read_text(encoding="utf-8")
            for source in fan_in.sources
        )
        == 2
    )


def test_reverse_order_changes_only_compile_database_order(
    tmp_path: Path,
) -> None:
    benchmark = load_benchmark()
    forward = benchmark.generate_corpus(
        tmp_path / "forward", 4, "baseline", 8, "forward"
    )
    reverse = benchmark.generate_corpus(
        tmp_path / "reverse", 4, "baseline", 8, "reverse"
    )

    forward_commands = json.loads(forward.compile_commands.read_text(encoding="utf-8"))
    reverse_commands = json.loads(reverse.compile_commands.read_text(encoding="utf-8"))
    assert [Path(command["file"]).name for command in reverse_commands] == list(
        reversed([Path(command["file"]).name for command in forward_commands])
    )


def test_model_analysis_prefers_clear_quadratic_growth() -> None:
    benchmark = load_benchmark()
    profile = {
        "translation_units": [
            {"start_position": position, "wall_seconds": 1 + position**2}
            for position in range(1, 20)
        ]
    }

    analysis = benchmark.analyse_translation_units(profile)

    assert analysis["preferred_model"] == "superlinear_quadratic"
    assert analysis["status"] == "superlinear-reproduced"
    assert analysis["models"]["superlinear_quadratic"]["residual_rmse"] < 1e-9


def test_model_analysis_reports_insufficient_series() -> None:
    benchmark = load_benchmark()

    assert benchmark.analyse_translation_units(None) == {
        "status": "unresolved",
        "reason": "fewer than four per-TU profile samples",
        "sample_count": 0,
    }


def test_profile_contract_requires_named_timings_and_counters(
    tmp_path: Path,
) -> None:
    benchmark = load_benchmark()
    profile_path = tmp_path / "profile.json"
    profile_path.write_text(
        json.dumps(
            {
                "summary": {
                    "timings": {key: 0.0 for key in benchmark.REQUIRED_PROFILE_TIMINGS},
                    "counters": {key: 0 for key in benchmark.REQUIRED_PROFILE_COUNTERS},
                },
                "translation_units": [],
            }
        ),
        encoding="utf-8",
    )

    assert benchmark._load_profile(profile_path)["summary"]

    profile_path.write_text(
        json.dumps({"summary": {"timings": {}, "counters": {}}}),
        encoding="utf-8",
    )
    with pytest.raises(RuntimeError, match="profile is incomplete"):
        benchmark._load_profile(profile_path)


def test_disabled_overhead_retains_spread_and_parity(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    benchmark = load_benchmark()
    samples = iter(
        [
            (10.0, "digest"),
            (10.05, "digest"),
            (10.2, "digest"),
            (10.25, "digest"),
            (9.9, "digest"),
            (9.95, "digest"),
        ]
    )

    def fake_cold_only(*_args, **_kwargs):
        wall, digest = next(samples)
        return {
            "wall_seconds": wall,
            "sqlite": {"snapshot": {"canonical_sha256": digest}},
            "translation_unit_order": ["tu_0000.cpp", "tu_0001.cpp"],
            "translation_unit_order_sha256": "order-digest",
        }

    monkeypatch.setattr(benchmark, "run_cold_only_case", fake_cold_only)
    comparison = benchmark.compare_disabled_overhead(
        tmp_path / "instrumented",
        tmp_path / "uninstrumented",
        1000,
        3,
        tmp_path / "work",
    )

    assert comparison["canonical_semantic_parity"]
    assert comparison["execution_order_evidence"]["all_trials_match"]
    assert comparison["within_one_percent"]
    assert comparison["uninstrumented_spread_seconds"] == {
        "minimum": 9.9,
        "maximum": 10.2,
    }
    assert comparison["instrumented_spread_seconds"] == {
        "minimum": 9.95,
        "maximum": 10.25,
    }


def test_main_rejects_fewer_than_three_trials(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    benchmark = load_benchmark()
    monkeypatch.setattr(
        "sys.argv",
        [
            "production.py",
            "--cidx",
            str(tmp_path / "missing-cidx"),
            "--trials",
            "2",
            "--work-root",
            str(tmp_path / "work"),
            "--output",
            str(tmp_path / "report.json"),
        ],
    )

    with pytest.raises(SystemExit, match="--trials must be at least 3"):
        benchmark.main()


def test_main_requires_noindex_work_root_on_macos(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    benchmark = load_benchmark()
    monkeypatch.setattr(benchmark.platform, "system", lambda: "Darwin")
    monkeypatch.setattr(
        "sys.argv",
        [
            "production.py",
            "--cidx",
            str(tmp_path / "missing-cidx"),
            "--work-root",
            str(tmp_path / "work"),
            "--output",
            str(tmp_path / "report.json"),
        ],
    )

    with pytest.raises(SystemExit, match=r"must end in \.noindex"):
        benchmark.main()
