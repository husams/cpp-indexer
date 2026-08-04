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


def test_canonical_rows_sort_after_portable_identity_normalization(
    tmp_path: Path,
) -> None:
    benchmark = load_benchmark()
    first_root = tmp_path / "trial-1"
    second_root = tmp_path / "trial-2"
    first_rows = [
        [f"{first_root}/b.cpp", "build:" + "f" * 40],
        [f"{first_root}/a.cpp", "build:" + "0" * 40],
    ]
    second_rows = [
        [f"{second_root}/a.cpp", "build:" + "9" * 40],
        [f"{second_root}/b.cpp", "build:" + "1" * 40],
    ]

    assert benchmark.HSE95._normalize_canonical_rows(
        first_rows, first_root
    ) == benchmark.HSE95._normalize_canonical_rows(second_rows, second_root)


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

    legacy_timings = {
        key: 0.0
        for key in benchmark.REQUIRED_PROFILE_TIMINGS
        if not key.startswith("fact_batch_writer.")
    }
    legacy_counters = {
        key: 0
        for key in benchmark.REQUIRED_PROFILE_COUNTERS
        if not key.startswith("fact_batch_writer.")
    }
    profile_path.write_text(
        json.dumps(
            {
                "summary": {
                    "timings": legacy_timings,
                    "counters": legacy_counters,
                },
                "translation_units": [
                    {
key: 0
                        for key in benchmark.REQUIRED_TRANSLATION_UNIT_FIELDS
                    }
                ],
            }
        ),
        encoding="utf-8",
    )
    assert benchmark._load_profile(
        profile_path, require_writer_metrics=False
    )["summary"]
    with pytest.raises(RuntimeError, match="profile is incomplete"):
        benchmark._load_profile(profile_path)


def test_resolve_stage_retains_transform_profile(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    benchmark = load_benchmark()
    case_root = tmp_path / "case"
    corpus_root = case_root / "corpus"
    cache = case_root / "cache"
    corpus_root.mkdir(parents=True)
    cache.mkdir()
    captured: list[str] = []
    profile = {
        "summary": {
            "timings": {key: 0.0 for key in benchmark.REQUIRED_PROFILE_TIMINGS},
            "counters": {key: 0 for key in benchmark.REQUIRED_PROFILE_COUNTERS},
        },
        "translation_units": [],
    }

    def fake_run_timed(command, *_args):
        captured.extend(command)
        return {
            "wall_seconds": 0.1,
            "peak_rss_bytes": 1,
            "stdout": "",
            "stderr": "",
        }

    monkeypatch.setattr(benchmark.HSE95, "run_timed", fake_run_timed)
    monkeypatch.setattr(benchmark.HSE95, "parse_header_counts", lambda _output: {})
    monkeypatch.setattr(
        benchmark,
        "_snapshot",
        lambda *_args, **_kwargs: {"page_bytes": 0, "rows": {}},
    )
    monkeypatch.setattr(
        benchmark, "_load_profile", lambda _path, **_kwargs: profile
    )

    result, _ = benchmark.run_stage(
        tmp_path / "cidx",
        cache,
        case_root,
        corpus_root,
        "resolve",
        ["resolve"],
        previous=None,
        profile=True,
    )

    assert "--profile-json" in captured
    assert result["profile"] is profile


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


def test_commit_ab_reports_writer_deltas_and_parity() -> None:
    benchmark = load_benchmark()

    def stage(label: str, digest: str, prepared: int, steps: int) -> dict:
        return {
            "label": label,
            "wall_seconds": 1.0,
            "peak_rss_bytes": 1024,
            "sqlite": {"snapshot": {"canonical_sha256": digest}},
            "profile": {
                "summary": {
                    "counters": {
                        "sqlite": {
                            "prepare_calls": prepared,
                            "virtual_machine_steps": steps,
                        },
                        "fact_batch_writer.statements_prepared": prepared,
                        "fact_batch_writer.virtual_machine_steps": steps,
                        "fact_batch_writer.rows_staged": 10,
                    }
                },
                "translation_units": [{"path": "a.cpp"}],
            },
        }

    baseline = [{"stages": [stage("cold", "same", 30, 300)]}]
    candidate = [{"stages": [stage("cold", "same", 10, 100)]}]

    result = benchmark.compare_commit_trials(baseline, candidate)

    assert result["semantic_parity"]
    assert result["translation_unit_order_parity"]
    assert result["parity_failures"] == []
    paired = result["paired_trials"][0]
    assert paired["delta"]["statements_prepared"] == -20
    assert paired["delta"]["virtual_machine_steps"] == -200
    assert paired["candidate"]["statements_prepared_per_staged_fact"] == 1.0

    # Elimination is measured against the baseline arm, never synthesized by
    # the writer: 30 - 10 prepares and 300 - 100 VM steps.
    assert paired["delta"]["statements_eliminated"] == 20
    assert paired["delta"]["virtual_machine_steps_eliminated"] == 200
    assert result["statement_elimination"]["statements_eliminated"] == 20
    assert result["statement_elimination"]["virtual_machine_steps_eliminated"] == 200
    assert result["statement_elimination"]["basis"]

    # This A/B runs both arms on the same SQLite runtime profile and performs
    # no durability, index write-cost, query-plan or latency experiment, so it
    # must report those criteria as unqualified rather than assert a conclusion.
    assert result["durability_profile"]["status"] == "not-qualified"
    assert result["durability_profile"]["reason"]
    assert result["secondary_index_decision"]["status"] == "not-qualified"
    assert result["secondary_index_decision"]["reason"]
    assert result["secondary_index_decision"]["removed_or_deferred"] is False
    # The old literal fields must not come back.
    assert "durability_profile_changed" not in result
    assert "binary_nocase_pairs" not in result["secondary_index_decision"]


def test_commit_ab_normalizes_synthetic_translation_unit_roots() -> None:
    benchmark = load_benchmark()

    def stage(root: str) -> dict:
        return {
            "label": "cold",
            "wall_seconds": 1.0,
            "peak_rss_bytes": 1024,
            "sqlite": {"snapshot": {"canonical_sha256": "same"}},
            "profile": {
                "summary": {"counters": {}},
                "translation_units": [
                    {"path": f"{root}/corpus/nested/tu_0000.cpp"}
                ],
            },
        }

    result = benchmark.compare_commit_trials(
        [{"stages": [stage("/tmp/baseline/trial-1")]}],
        [{"stages": [stage("/tmp/candidate/trial-1")]}],
    )

    assert result["translation_unit_order_parity"]
