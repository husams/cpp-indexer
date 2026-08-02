#!/usr/bin/env python3
"""Production-corpus and end-to-end indexing benchmark for HSE-103."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import sqlite3
import statistics
import subprocess
import time
from typing import Any, Iterable, Mapping

try:
    from benchmarks.indexing.frontend_reuse import qualification_contract
except ModuleNotFoundError:  # direct script execution from benchmarks/indexing
    from frontend_reuse import qualification_contract


MINIMUM_TRIALS = 3
BASELINE_DISTINCT_OWNED_HEADERS = 2
DEFAULT_MANY_HEADER_TARGET = 128
SQLITE_EXPERIMENTS: dict[str, dict[str, Any]] = {
    "shipped-control": {},
    "cache-64m": {"cache_size": -65_536},
    "mmap-256m": {"mmap_size": 268_435_456},
    "temp-memory": {"temp_store": "MEMORY"},
    "wal-normal": {"journal_mode": "WAL", "synchronous": "NORMAL"},
    "delete-full": {"journal_mode": "DELETE", "synchronous": "FULL"},
}
ATTRIBUTION_EXPERIMENTS = {
    "identity-reconciliation": {
        "case": "self-index",
        "stage": "self-cold",
        "profile_fields": [
            "summary.timings.identity_reconciliation",
            "summary.counters.reconciliation_calls",
            "summary.counters.reconciliation_rows_changed",
        ],
        "consumer": "HSE-114",
    },
    "applicability-maintenance": {
        "comparison": [
            "baseline:32:forward:cold",
            "header-heavy:32:forward:cold",
            "many-headers:32:forward:cold",
        ],
        "profile_fields": [
            "summary.timings.applicability_association",
            "summary.counters.applicability_attempted",
            "summary.counters.applicability_inserted",
            "summary.counters.applicability_ignored",
            "summary.counters.applicability_deleted",
            "summary.counters.applicability_temporary_rows",
            "summary.counters.include_attempted",
            "summary.counters.include_inserted_or_updated",
            "summary.counters.include_ignored",
            "summary.counters.include_deleted",
            "summary.counters.include_cascade_deleted",
            "summary.counters.include_path_resolution_queries",
        ],
    },
    "workspace-file-validation": {
        "comparison": [
            "baseline:32:forward:unchanged-warm",
            "baseline:32:forward:configuration-change",
            "baseline:32:forward:generated-input",
        ],
        "profile_fields": [
            "summary.timings.source_validation_hashing",
            "summary.timings.workspace_snapshot_configuration",
        ],
    },
    "database-growth-lookups-writes": {
        "comparison": [
            "baseline:1000:forward:cold",
            "baseline:1000:reverse:cold",
        ],
        "profile_fields": [
            "translation_units.database_cardinality_before",
            "summary.sqlite",
            "summary.timings.fact_persistence",
        ],
    },
}
REQUIRED_PROFILE_TIMINGS = frozenset(
    {
        "source_validation_hashing",
        "workspace_snapshot_configuration",
        "driver_subprocesses",
        "clang_front_end",
        "root_symbols",
        "root_declarations",
        "root_definitions",
        "root_namespaces",
        "body_extraction",
        "fact_persistence",
        "include_persistence",
        "applicability_association",
        "commit",
        "transforms",
        "verification",
        "identity_reconciliation",
        "sqlite_prepare",
        "sqlite_vdbe",
    }
)
REQUIRED_PROFILE_COUNTERS = frozenset(
    {
        "applicability_attempted",
        "applicability_inserted",
        "applicability_ignored",
        "applicability_deleted",
        "applicability_temporary_rows",
        "include_attempted",
        "include_inserted_or_updated",
        "include_ignored",
        "include_deleted",
        "include_cascade_deleted",
        "root_traverse_decl_calls",
        "registered_root_traversal_budget",
        "observed_root_traversals",
        "facts_by_family",
        "sqlite",
        "transactions",
        "toolchain_cache",
        "reconciliation_calls",
        "reconciliation_rows_changed",
        "include_path_resolution_queries",
    }
)
REQUIRED_TRANSLATION_UNIT_FIELDS = frozenset(
    {
        "path",
        "start_position",
        "database_cardinality_before",
        "fact_cardinality_before",
        "source_bytes",
        "preprocessed_bytes",
        "include_count",
        "new_headers",
        "already_indexed_headers",
        "configuration_state",
        "wall_seconds",
        "in_process_cpu_seconds",
        "child_process_wall_seconds",
        "peak_rss_bytes",
    }
)

# ---------------------------------------------------------------------------
# S-098 routed root-fusion decision harness
#
# The S-071 baseline is frozen evidence, not something this harness recomputes.
# Every value below is read from the authoritative report attached to backlog
# task S-098; the checked-in fixture is a compact excerpt of that report with
# the same field shape, so one summary reads either one.
# ---------------------------------------------------------------------------
S098_CONTRACT_VERSION = "s098-root-fusion/v1"
S098_BASELINE_CASE = "header-heavy:8:forward"
S098_BASELINE_STAGE = "cold"
S098_SYMBOL_ROOT_TIMING = "root_symbols"
S098_OTHER_ROOT_TIMINGS = (
    "root_declarations",
    "root_definitions",
    "root_namespaces",
)
S098_FIXED_ROOT_TIMINGS = (S098_SYMBOL_ROOT_TIMING, *S098_OTHER_ROOT_TIMINGS)
S098_REGISTERED_BUDGET_COUNTER = "registered_root_traversal_budget"
S098_OBSERVED_TRAVERSAL_COUNTER = "observed_root_traversals"
# S-098 AC #1720: strictly below 25 ms, from the recorded 40.611 ms baseline.
S098_FIXED_ROOT_BUDGET_SECONDS = 0.025
# T-139 AC #2635 owns the symbol-root floor inside that total. It is recorded
# here and reported, but S-098 ship eligibility is governed by the total.
S098_SYMBOL_ROOT_BUDGET_SECONDS = 0.015480
S098_SYMBOL_ROOT_BUDGET_OWNER = "T-139"
S098_BASELINE_FIXTURE_NAME = "fixtures/s071-header-heavy-baseline.json"
S098_BASELINE_ARTIFACT_TASK = "S-098"
S098_BASELINE_ARTIFACT_NAME = "s071-authoritative-r3.json"
S098_BACKLOG_PROJECT = "cpp-indexer"
S098_REQUIRED_REPORT_KEYS = frozenset(
    {
        "method",
        "identity",
        "authoritative_timing",
        "host_quiescence",
        "parity_failures",
        "aggregates",
        "corpus_contract",
        "individual_trials",
    }
)
# Per-trial corpus descriptors that must be identical across a case's trials
# and identical to the baseline's. `target_distinct_owned_headers` is the field
# that catches a candidate measured on a lighter corpus: the case key alone
# only pins shape, file count, and order.
S098_CORPUS_FIELDS = (
    "kind",
    "shape",
    "order",
    "files",
    "baseline_distinct_owned_headers",
    "target_distinct_owned_headers",
)
# Contract fields every measured shape depends on: the owned-header floor each
# shape builds on, and the shape/order vocabulary the case key is written in.
S098_CORPUS_CONTRACT_FIELDS = (
    "baseline_distinct_owned_headers",
    "shapes",
    "orders",
)
# Contract fields only some shapes depend on. `generate_corpus` writes a fixed
# 16 heavy headers for `header-heavy` and ignores `many_header_target` there, so
# binding it for that shape would refuse a re-run that regenerated exactly the
# baseline's corpus with a different flag value.
S098_SHAPE_CONTRACT_FIELDS: dict[str, tuple[str, ...]] = {
    "many-headers": ("many_header_target",),
}
# Front-end reuse changes front-end cost, so a candidate that enables a reuse
# mechanism is not comparable to the no-reuse baseline. What is bound is the
# mechanism *in force*, never the presence of the section: S-071 predates the
# S-075 contract and records no section, while every run of this harness records
# `qualification_contract`, which is that same shipped no-reuse control. Both
# describe a front end that reused nothing, so both normalise to this control.
# Its keys are the bound field set - `front_end_reuse_identity` builds exactly
# them on both paths - so this dict is the only place that set is written down.
# `identity_version` and `explicitly_disabled` are deliberately out: they say
# which contract emitted the control and how it was selected, not what the front
# end did.
S098_NO_REUSE_MECHANISM = "none"
S098_NO_REUSE_CONTROL: dict[str, Any] = {
    "mechanism": S098_NO_REUSE_MECHANISM,
    "artifact_construction": False,
    "artifact_injection": False,
}

# Named rejection reasons, in the deterministic order they are reported.
S098_REASON_ARTIFACT_MISSING = "baseline-artifact-missing"
S098_REASON_ARTIFACT_MISMATCH = "baseline-artifact-mismatch"
S098_REASON_MALFORMED = "malformed-report"
S098_REASON_IDENTITY = "identity-mismatch"
S098_REASON_CONTENDED = "host-not-quiescent"
S098_REASON_TRIALS = "insufficient-trials"
S098_REASON_PARITY = "semantic-parity-failure"
S098_REASON_TRAVERSAL_BUDGET = "traversal-budget-exceeded"
S098_REASON_FIXED_ROOT_BUDGET = "fixed-root-budget-not-met"
S098_REJECTION_REASONS = (
    S098_REASON_ARTIFACT_MISSING,
    S098_REASON_ARTIFACT_MISMATCH,
    S098_REASON_MALFORMED,
    S098_REASON_IDENTITY,
    S098_REASON_CONTENDED,
    S098_REASON_TRIALS,
    S098_REASON_PARITY,
    S098_REASON_TRAVERSAL_BUDGET,
    S098_REASON_FIXED_ROOT_BUDGET,
)


class BaselineError(RuntimeError):
    """A named, deterministic refusal to qualify S-098 evidence."""

    def __init__(self, reason: str, detail: str) -> None:
        super().__init__(f"{reason}: {detail}")
        self.reason = reason
        self.detail = detail


def _load_hse95() -> Any:
    path = Path(__file__).with_name("run.py")
    spec = importlib.util.spec_from_file_location("hse95_benchmark", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load benchmark helper: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


HSE95 = _load_hse95()


@dataclass(frozen=True)
class Corpus:
    root: Path
    compile_commands: Path
    sources: tuple[Path, ...]
    headers: dict[str, Path]
    shape: str
    target_distinct_owned_headers: int
    order: str


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _command_text(command: list[str], *, cwd: Path | None = None) -> str:
    completed = subprocess.run(
        command,
        cwd=cwd,
        capture_output=True,
        text=True,
        check=False,
    )
    return (completed.stdout + completed.stderr).strip()


def _path_within(path: Path, parent: Path) -> bool:
    try:
        path.resolve().relative_to(parent.resolve())
    except ValueError:
        return False
    return True


def environment_identity(
    executable: Path, checkout: Path, compile_databases: Iterable[Path]
) -> dict[str, Any]:
    resolved_executable = executable.resolve()
    sqlite_compile_options = [
        row[0]
        for row in sqlite3.connect(":memory:")
        .execute("PRAGMA compile_options")
        .fetchall()
    ]
    compile_identities = []
    for compile_database in compile_databases:
        compile_identities.append(
            {
                "path": str(compile_database.resolve()),
                "sha256": _sha256(compile_database),
            }
        )
    return {
        "host": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "processor": platform.processor(),
            "python": platform.python_version(),
            "cpu_count": os.cpu_count(),
        },
        "executable": {
            "path": str(resolved_executable),
            "sha256": _sha256(resolved_executable),
            "version": _command_text([str(resolved_executable), "--version"]),
        },
        "checkout": {
            "path": str(checkout.resolve()),
            "commit": _command_text(["git", "rev-parse", "HEAD"], cwd=checkout),
            "dirty": bool(
                _command_text(["git", "status", "--porcelain"], cwd=checkout)
            ),
        },
        "compile_databases": compile_identities,
        "sqlite": {
            "version": sqlite3.sqlite_version,
            "compile_options": sorted(sqlite_compile_options),
        },
        "clang": {
            "version": _command_text(["clang++", "--version"]),
            "resource_dir": _command_text(["clang++", "-print-resource-dir"]),
        },
        "schema": {
            "version": HSE95.EXPECTED_SCHEMA_VERSION,
            "catalog_version": HSE95.EXPECTED_CATALOG_VERSION,
            "catalog_hash": HSE95.EXPECTED_CATALOG_HASH,
        },
        "measurement_condition": {
            "requires_quiescent_host": True,
            "observer_note": (
                "Record competing cidx/compiler processes and system load before "
                "accepting timing evidence."
            ),
        },
    }


def _insert_includes(source: Path, includes: list[str]) -> None:
    text = source.read_text(encoding="utf-8")
    marker = '#include "shared.hpp"\n'
    replacement = marker + "".join(f'#include "{name}"\n' for name in includes)
    if text.count(marker) != 1:
        raise RuntimeError(f"unexpected include marker in {source}")
    source.write_text(text.replace(marker, replacement, 1), encoding="utf-8")


def _write_header(path: Path, symbol: str, value: int) -> None:
    path.write_text(
        "#pragma once\n"
        f"namespace hse103 {{ inline int {symbol}() {{ return {value}; }} }}\n",
        encoding="utf-8",
    )


def _rewrite_compile_order(compile_commands: Path, order: str) -> None:
    commands = json.loads(compile_commands.read_text(encoding="utf-8"))
    if order == "reverse":
        commands.reverse()
    compile_commands.write_text(json.dumps(commands, indent=2), encoding="utf-8")


def generate_corpus(
    root: Path,
    count: int,
    shape: str,
    many_header_target: int,
    order: str,
) -> Corpus:
    compile_commands, sources, _ = HSE95.generate_corpus(root, count)
    headers = {
        "high_fan_in": root / "shared.hpp",
        "coverage": root / "coverage.hpp",
    }
    target = BASELINE_DISTINCT_OWNED_HEADERS

    if shape == "header-heavy":
        extra = []
        for index in range(16):
            name = f"heavy_{index:02d}.hpp"
            _write_header(root / name, f"heavy_{index:02d}", index)
            extra.append(name)
        for source in sources:
            _insert_includes(source, extra)
        headers["low_fan_in"] = root / extra[0]
        target += len(extra)
    elif shape == "many-headers":
        if many_header_target < BASELINE_DISTINCT_OWNED_HEADERS:
            raise ValueError("many-header target must be at least 2")
        names = []
        for index in range(many_header_target - BASELINE_DISTINCT_OWNED_HEADERS):
            name = f"owned_{index:04d}.hpp"
            _write_header(root / name, f"owned_{index:04d}", index)
            names.append(name)
        for index, source in enumerate(sources):
            if names:
                _insert_includes(source, [names[index % len(names)]])
        if names:
            headers["low_fan_in"] = root / names[0]
        target = many_header_target
    elif shape == "fan-in":
        high = root / "high_fan_in.hpp"
        low = root / "low_fan_in.hpp"
        _write_header(high, "high_fan_in", 1)
        _write_header(low, "low_fan_in", 1)
        for index, source in enumerate(sources):
            _insert_includes(
                source,
                ["high_fan_in.hpp"]
                + (["low_fan_in.hpp"] if index < min(2, count) else []),
            )
        headers["high_fan_in"] = high
        headers["low_fan_in"] = low
        target += 2
    elif shape != "baseline":
        raise ValueError(f"unknown corpus shape: {shape}")

    _rewrite_compile_order(compile_commands, order)
    return Corpus(
        root=root,
        compile_commands=compile_commands,
        sources=tuple(sources),
        headers=headers,
        shape=shape,
        target_distinct_owned_headers=target,
        order=order,
    )


def mutate_header(path: Path, revision: int) -> None:
    with path.open("a", encoding="utf-8") as stream:
        stream.write(f"\n#define HSE103_HEADER_REVISION {revision}\n")


def update_compile_database(
    corpus: Corpus,
    *,
    define: str | None = None,
    forced_include: Path | None = None,
) -> None:
    commands = json.loads(corpus.compile_commands.read_text(encoding="utf-8"))
    for command in commands:
        suffix = ""
        if define:
            suffix += f" -D{define}"
        if forced_include:
            suffix += f" -include {forced_include}"
        command["command"] += suffix
    corpus.compile_commands.write_text(json.dumps(commands, indent=2), encoding="utf-8")


def _profile_path(case_root: Path, label: str) -> Path:
    return case_root / f"{label}.profile.json"


def _load_profile(path: Path) -> dict[str, Any]:
    if not path.exists():
        raise RuntimeError(f"profiling output was not created: {path}")
    profile = json.loads(path.read_text(encoding="utf-8"))
    summary = profile.get("summary")
    if not isinstance(summary, dict):
        raise RuntimeError(f"profile summary is missing: {path}")
    timings = summary.get("timings", {})
    counters = summary.get("counters", {})
    missing_timings = sorted(REQUIRED_PROFILE_TIMINGS.difference(timings))
    missing_counters = sorted(REQUIRED_PROFILE_COUNTERS.difference(counters))
    if missing_timings or missing_counters:
        raise RuntimeError(
            f"profile is incomplete: timings={missing_timings}, "
            f"counters={missing_counters}"
        )
    for index, translation_unit in enumerate(profile.get("translation_units", [])):
        missing = sorted(REQUIRED_TRANSLATION_UNIT_FIELDS.difference(translation_unit))
        if missing:
            raise RuntimeError(
                f"profile translation unit {index} is incomplete: {missing}"
            )
    return profile


def _snapshot(
    database: Path, corpus_root: Path, require_coverage: bool
) -> dict[str, Any]:
    return HSE95.database_snapshot(
        database,
        corpus_root,
        require_coverage=require_coverage,
        capture_canonical=True,
    )


def run_stage(
    executable: Path,
    cache: Path,
    case_root: Path,
    corpus_root: Path,
    label: str,
    args: list[str],
    *,
    previous: dict[str, Any] | None,
    profile: bool,
    sqlite_experiment: dict[str, Any] | None = None,
    require_coverage: bool = True,
) -> tuple[dict[str, Any], dict[str, Any]]:
    environment = dict(os.environ)
    environment["INDEXER_CACHE"] = str(cache)
    command = [str(executable), *args]
    profile_path = _profile_path(case_root, label)
    if profile and args and args[0] in {"index", "resolve"}:
        command.extend(["--profile-json", str(profile_path)])
        if sqlite_experiment is not None:
            sqlite_path = case_root / f"{label}.sqlite-experiment.json"
            sqlite_path.write_text(
                json.dumps(sqlite_experiment, indent=2), encoding="utf-8"
            )
            command.extend(["--profile-sqlite-config", str(sqlite_path)])
    metrics = HSE95.run_timed(command, environment, case_root, label)
    database = cache / "index.db"
    current = _snapshot(database, corpus_root, require_coverage)
    stage_profile = (
        _load_profile(profile_path)
        if profile and args[0] in {"index", "resolve"}
        else None
    )
    result = {
        key: value for key, value in metrics.items() if key not in {"stdout", "stderr"}
    }
    result.update(
        {
            "label": label,
            "profile": stage_profile,
            "header_counts": HSE95.parse_header_counts(
                metrics["stdout"] + metrics["stderr"]
            ),
            "sqlite": {
                "snapshot": current,
                "delta": current
                if previous is None
                else {
                    "page_bytes": current["page_bytes"] - previous["page_bytes"],
                    "rows": {
                        key: current["rows"][key] - previous["rows"][key]
                        for key in current["rows"]
                        if current["rows"][key] is not None
                        and previous["rows"][key] is not None
                    },
                },
            },
        }
    )
    return result, current


def _import(
    executable: Path,
    corpus: Corpus,
    cache: Path,
    case_root: Path,
    label: str,
    previous: dict[str, Any] | None,
) -> tuple[dict[str, Any], dict[str, Any]]:
    return run_stage(
        executable,
        cache,
        case_root,
        corpus.root,
        label,
        [
            "import",
            "--db",
            str(corpus.compile_commands),
            "--name",
            f"hse103-{corpus.shape}",
            "--force",
        ],
        previous=previous,
        profile=False,
        require_coverage=False,
    )


def run_synthetic_case(
    executable: Path,
    count: int,
    shape: str,
    many_header_target: int,
    order: str,
    case_root: Path,
    *,
    profile: bool = True,
    sqlite_experiment: dict[str, Any] | None = None,
) -> dict[str, Any]:
    corpus = generate_corpus(
        case_root / "corpus", count, shape, many_header_target, order
    )
    cache = case_root / "cache"
    cache.mkdir()
    stages: list[dict[str, Any]] = []
    previous = None

    measured, previous = _import(
        executable, corpus, cache, case_root, "import", previous
    )
    stages.append(measured)
    for label, arguments in (
        ("cold", ["index"]),
        ("resolve", ["resolve"]),
        ("unchanged-warm", ["index"]),
    ):
        measured, previous = run_stage(
            executable,
            cache,
            case_root,
            corpus.root,
            label,
            arguments,
            previous=previous,
            profile=profile,
            sqlite_experiment=sqlite_experiment,
        )
        stages.append(measured)

    HSE95.mutate_translation_unit(corpus.sources[0], 0, 1)
    measured, previous = run_stage(
        executable,
        cache,
        case_root,
        corpus.root,
        "one-source",
        ["index", str(corpus.sources[0])],
        previous=previous,
        profile=profile,
        sqlite_experiment=sqlite_experiment,
    )
    stages.append(measured)

    low_header = corpus.headers.get("low_fan_in")
    if low_header is not None:
        mutate_header(low_header, 1)
        measured, previous = run_stage(
            executable,
            cache,
            case_root,
            corpus.root,
            "low-fan-in-header",
            ["index"],
            previous=previous,
            profile=profile,
            sqlite_experiment=sqlite_experiment,
        )
        stages.append(measured)

    mutate_header(corpus.headers["high_fan_in"], 2)
    measured, previous = run_stage(
        executable,
        cache,
        case_root,
        corpus.root,
        "high-fan-in-header",
        ["index"],
        previous=previous,
        profile=profile,
        sqlite_experiment=sqlite_experiment,
    )
    stages.append(measured)

    update_compile_database(corpus, define="HSE103_CONFIG_STATE=1")
    measured, previous = _import(
        executable, corpus, cache, case_root, "configuration-import", previous
    )
    stages.append(measured)
    measured, previous = run_stage(
        executable,
        cache,
        case_root,
        corpus.root,
        "configuration-change",
        ["index"],
        previous=previous,
        profile=profile,
        sqlite_experiment=sqlite_experiment,
    )
    stages.append(measured)

    generated = corpus.root / "generated_input.hpp"
    _write_header(generated, "generated_input", 1)
    update_compile_database(corpus, forced_include=generated)
    measured, previous = _import(
        executable, corpus, cache, case_root, "generated-import", previous
    )
    stages.append(measured)
    measured, previous = run_stage(
        executable,
        cache,
        case_root,
        corpus.root,
        "generated-input-prime",
        ["index"],
        previous=previous,
        profile=profile,
        sqlite_experiment=sqlite_experiment,
    )
    stages.append(measured)
    mutate_header(generated, 2)
    measured, previous = run_stage(
        executable,
        cache,
        case_root,
        corpus.root,
        "generated-input",
        ["index"],
        previous=previous,
        profile=profile,
        sqlite_experiment=sqlite_experiment,
    )
    stages.append(measured)

    return {
        "kind": "synthetic",
        "shape": shape,
        "order": order,
        "files": count,
        "baseline_distinct_owned_headers": BASELINE_DISTINCT_OWNED_HEADERS,
        "target_distinct_owned_headers": corpus.target_distinct_owned_headers,
        "compile_database": {
            "path": str(corpus.compile_commands),
            "sha256": _sha256(corpus.compile_commands),
        },
        "stages": stages,
    }


def run_self_index_case(
    executable: Path,
    checkout: Path,
    compile_database: Path,
    case_root: Path,
) -> dict[str, Any]:
    cache = case_root / "cache"
    cache.mkdir()
    stages = []
    previous = None
    for label, args, profile, coverage in (
        (
            "self-import",
            [
                "import",
                "--db",
                str(compile_database),
                "--name",
                "cpp-indexer-self",
                "--repo",
                "cpp-indexer",
                "--force",
            ],
            False,
            False,
        ),
        ("self-cold", ["index"], True, True),
        ("self-resolve", ["resolve"], False, True),
        ("self-unchanged-warm", ["index"], True, True),
    ):
        measured, previous = run_stage(
            executable,
            cache,
            case_root,
            checkout,
            label,
            args,
            previous=previous,
            profile=profile,
            require_coverage=coverage,
        )
        stages.append(measured)
    return {
        "kind": "self-index",
        "checkout": str(checkout.resolve()),
        "compile_database": {
            "path": str(compile_database.resolve()),
            "sha256": _sha256(compile_database),
        },
        "stages": stages,
    }


def run_cold_only_case(
    executable: Path,
    count: int,
    case_root: Path,
) -> dict[str, Any]:
    corpus = generate_corpus(
        case_root / "corpus",
        count,
        "baseline",
        DEFAULT_MANY_HEADER_TARGET,
        "forward",
    )
    cache = case_root / "cache"
    cache.mkdir(parents=True)
    _, previous = _import(executable, corpus, cache, case_root, "import", previous=None)
    cold, _ = run_stage(
        executable,
        cache,
        case_root,
        corpus.root,
        "cold",
        ["index"],
        previous=previous,
        profile=False,
    )
    commands = json.loads(corpus.compile_commands.read_text(encoding="utf-8"))
    order = [Path(command["file"]).name for command in commands]
    cold["translation_unit_order"] = order
    cold["translation_unit_order_sha256"] = hashlib.sha256(
        json.dumps(order, separators=(",", ":")).encode()
    ).hexdigest()
    return cold


def compare_disabled_overhead(
    instrumented: Path,
    uninstrumented: Path,
    count: int,
    trials: int,
    root: Path,
) -> dict[str, Any]:
    paired = []
    parity_failures = []
    for trial in range(1, trials + 1):
        baseline = run_cold_only_case(
            uninstrumented, count, root / f"trial-{trial}" / "uninstrumented"
        )
        candidate = run_cold_only_case(
            instrumented, count, root / f"trial-{trial}" / "instrumented"
        )
        baseline_digest = baseline["sqlite"]["snapshot"]["canonical_sha256"]
        candidate_digest = candidate["sqlite"]["snapshot"]["canonical_sha256"]
        semantic_match = baseline_digest == candidate_digest
        order_match = (
            baseline["translation_unit_order"] == candidate["translation_unit_order"]
        )
        if not semantic_match:
            parity_failures.append(
                f"trial {trial}: instrumented/uninstrumented digests differ"
            )
        if not order_match:
            parity_failures.append(
                f"trial {trial}: instrumented/uninstrumented TU order differs"
            )
        paired.append(
            {
                "trial": trial,
                "uninstrumented_wall_seconds": baseline["wall_seconds"],
                "instrumented_wall_seconds": candidate["wall_seconds"],
                "uninstrumented_digest": baseline_digest,
                "instrumented_digest": candidate_digest,
                "semantic_match": semantic_match,
                "translation_unit_order_match": order_match,
                "uninstrumented_order_sha256": baseline[
                    "translation_unit_order_sha256"
                ],
                "instrumented_order_sha256": candidate["translation_unit_order_sha256"],
            }
        )
    baseline_values = [item["uninstrumented_wall_seconds"] for item in paired]
    candidate_values = [item["instrumented_wall_seconds"] for item in paired]
    baseline_median = statistics.median(baseline_values)
    candidate_median = statistics.median(candidate_values)
    overhead_percent = (
        (candidate_median - baseline_median) / baseline_median * 100
        if baseline_median
        else None
    )
    return {
        "files": count,
        "trial_count": trials,
        "paired_trials": paired,
        "uninstrumented_median_wall_seconds": baseline_median,
        "instrumented_median_wall_seconds": candidate_median,
        "uninstrumented_spread_seconds": {
            "minimum": min(baseline_values),
            "maximum": max(baseline_values),
        },
        "instrumented_spread_seconds": {
            "minimum": min(candidate_values),
            "maximum": max(candidate_values),
        },
        "overhead_percent": overhead_percent,
        "within_one_percent": (
            overhead_percent is not None and overhead_percent <= 1.0
        ),
        "canonical_semantic_parity": not parity_failures,
        "parity_failures": parity_failures,
        "execution_order_evidence": {
            "all_trials_match": all(
                item["translation_unit_order_match"] for item in paired
            ),
            "normalized_order_sha256_pairs": [
                {
                    "trial": item["trial"],
                    "uninstrumented": item["uninstrumented_order_sha256"],
                    "instrumented": item["instrumented_order_sha256"],
                }
                for item in paired
            ],
        },
    }


def _solve(matrix: list[list[float]], values: list[float]) -> list[float]:
    augmented = [row[:] + [values[index]] for index, row in enumerate(matrix)]
    size = len(values)
    for column in range(size):
        pivot = max(range(column, size), key=lambda row: abs(augmented[row][column]))
        if abs(augmented[pivot][column]) < 1e-15:
            raise ValueError("singular model matrix")
        augmented[column], augmented[pivot] = augmented[pivot], augmented[column]
        divisor = augmented[column][column]
        augmented[column] = [value / divisor for value in augmented[column]]
        for row in range(size):
            if row == column:
                continue
            factor = augmented[row][column]
            augmented[row] = [
                value - factor * pivot_value
                for value, pivot_value in zip(augmented[row], augmented[column])
            ]
    return [augmented[index][-1] for index in range(size)]


def fit_model(
    x_values: list[float], y_values: list[float], degree: int
) -> dict[str, Any]:
    if len(x_values) != len(y_values) or len(x_values) <= degree:
        raise ValueError("insufficient samples for model")
    columns = [[x**power for power in range(degree + 1)] for x in x_values]
    normal = [
        [sum(row[left] * row[right] for row in columns) for right in range(degree + 1)]
        for left in range(degree + 1)
    ]
    rhs = [
        sum(row[column] * value for row, value in zip(columns, y_values))
        for column in range(degree + 1)
    ]
    coefficients = _solve(normal, rhs)
    fitted = [
        sum(coefficient * x**power for power, coefficient in enumerate(coefficients))
        for x in x_values
    ]
    residuals = [actual - estimate for actual, estimate in zip(y_values, fitted)]
    sse = sum(value * value for value in residuals)
    mean = statistics.fmean(y_values)
    total = sum((value - mean) ** 2 for value in y_values)
    degrees_of_freedom = len(x_values) - len(coefficients)
    variance = sse / degrees_of_freedom if degrees_of_freedom > 0 else 0.0
    inverse_columns = []
    for index in range(len(coefficients)):
        unit = [0.0] * len(coefficients)
        unit[index] = 1.0
        inverse_columns.append(_solve(normal, unit))
    standard_errors = [
        math.sqrt(max(0.0, variance * inverse_columns[index][index]))
        for index in range(len(coefficients))
    ]
    aic = len(x_values) * math.log(max(sse / len(x_values), 1e-30)) + 2 * len(
        coefficients
    )
    return {
        "degree": degree,
        "coefficients": coefficients,
        "coefficient_standard_errors": standard_errors,
        "residuals": residuals,
        "residual_rmse": math.sqrt(sse / len(x_values)),
        "r_squared": 1.0 - sse / total if total else 1.0,
        "aic": aic,
    }


def analyse_translation_units(profile: dict[str, Any] | None) -> dict[str, Any]:
    translation_units = (profile or {}).get("translation_units", [])
    if len(translation_units) < 4:
        return {
            "status": "unresolved",
            "reason": "fewer than four per-TU profile samples",
            "sample_count": len(translation_units),
        }
    x_values = [float(item["start_position"]) for item in translation_units]
    y_values = [float(item["wall_seconds"]) for item in translation_units]
    models = {
        "constant": fit_model(x_values, y_values, 0),
        "linear": fit_model(x_values, y_values, 1),
        "superlinear_quadratic": fit_model(x_values, y_values, 2),
    }
    best = min(models, key=lambda name: models[name]["aic"])
    quadratic = models["superlinear_quadratic"]
    coefficient = quadratic["coefficients"][2]
    uncertainty = quadratic["coefficient_standard_errors"][2]
    if best == "superlinear_quadratic" and coefficient > 2 * uncertainty:
        status = "superlinear-reproduced"
    elif best == "superlinear_quadratic":
        status = "superlinear-unresolved"
    else:
        status = "superlinear-not-preferred"
    return {
        "status": status,
        "sample_count": len(translation_units),
        "preferred_model": best,
        "models": models,
    }


def _stage(case: dict[str, Any], label: str) -> dict[str, Any]:
    return next(stage for stage in case["stages"] if stage["label"] == label)


def aggregate_trials(trials: list[dict[str, Any]]) -> dict[str, Any]:
    first = trials[0]
    stage_labels = [stage["label"] for stage in first["stages"]]
    stages = {}
    parity_failures = []
    for label in stage_labels:
        values = [_stage(trial, label) for trial in trials]
        profiles = [value.get("profile") for value in values if value.get("profile")]
        timing_names = sorted(
            {name for profile in profiles for name in profile["summary"]["timings"]}
        )
        counter_names = sorted(
            {
                name
                for profile in profiles
                for name, counter in profile["summary"]["counters"].items()
                if isinstance(counter, (int, float))
            }
        )
        digests = [value["sqlite"]["snapshot"]["canonical_sha256"] for value in values]
        integrity = [value["sqlite"]["snapshot"]["integrity_check"] for value in values]
        if len(set(digests)) != 1:
            parity_failures.append(f"{label}: canonical digests differ")
        if any(value != "ok" for value in integrity):
            parity_failures.append(f"{label}: integrity check failed")
        stages[label] = {
            "wall_seconds": statistics.median(
                value["wall_seconds"] for value in values
            ),
            "wall_seconds_trials": [value["wall_seconds"] for value in values],
            "cpu_seconds": statistics.median(value["cpu_seconds"] for value in values),
            "cpu_seconds_trials": [value["cpu_seconds"] for value in values],
            "peak_rss_bytes": statistics.median(
                value["peak_rss_bytes"] for value in values
            ),
            "peak_rss_bytes_trials": [value["peak_rss_bytes"] for value in values],
            "canonical_sha256_trials": digests,
            "semantic_parity": len(set(digests)) == 1,
            "per_tu_analysis": analyse_translation_units(values[0].get("profile")),
            "per_tu_analysis_trials": [
                analyse_translation_units(value.get("profile")) for value in values
            ],
            "profile_summary": {
                "timings": {
                    name: {
                        "median": statistics.median(
                            profile["summary"]["timings"].get(name, 0.0)
                            for profile in profiles
                        ),
                        "trials": [
                            profile["summary"]["timings"].get(name, 0.0)
                            for profile in profiles
                        ],
                    }
                    for name in timing_names
                },
                "counters": {
                    name: {
                        "median": statistics.median(
                            profile["summary"]["counters"].get(name, 0)
                            for profile in profiles
                        ),
                        "trials": [
                            profile["summary"]["counters"].get(name, 0)
                            for profile in profiles
                        ],
                    }
                    for name in counter_names
                },
            },
        }
    return {
        "trial_count": len(trials),
        "stages": stages,
        "parity_failures": parity_failures,
    }


def attribution_summary(
    aggregates: dict[str, Any],
    representative_files: int,
    scale_files: int,
) -> dict[str, Any]:
    def profile(case: str, stage: str) -> dict[str, Any]:
        return aggregates[case]["stages"][stage]["profile_summary"]

    baseline_small = f"baseline:{representative_files}:forward"
    baseline_scale = f"baseline:{scale_files}:forward"
    baseline_scale_reverse = f"baseline:{scale_files}:reverse"
    applicability_cases = [
        baseline_small,
        f"header-heavy:{representative_files}:forward",
        f"many-headers:{representative_files}:forward",
    ]
    summary: dict[str, Any] = {
        "applicability_maintenance": {
            case: profile(case, "cold") for case in applicability_cases
        },
        "workspace_file_validation": {
            stage: profile(baseline_small, stage)
            for stage in (
                "unchanged-warm",
                "configuration-change",
                "generated-input",
            )
        },
        "database_growth_order": {
            "forward": profile(baseline_scale, "cold"),
            "reverse": profile(baseline_scale_reverse, "cold"),
        },
    }
    if "self-index" in aggregates:
        summary["identity_reconciliation"] = profile("self-index", "self-cold")
    else:
        summary["identity_reconciliation"] = {
            "status": "not-run",
            "reason": "self-index was skipped",
        }
    return summary


def recovery_probe(
    executable: Path,
    database: Path,
    corpus_root: Path,
    sqlite_configuration: dict[str, Any],
) -> dict[str, Any]:
    probe = database.with_name(f"{database.stem}-recovery{database.suffix}")
    shutil.copy2(database, probe)
    original = _snapshot(database, corpus_root, require_coverage=True)
    with sqlite3.connect(probe) as connection:
        connection.execute("UPDATE file SET indexed = 0")
    config_path = probe.with_suffix(".sqlite-experiment.json")
    config_path.write_text(json.dumps(sqlite_configuration, indent=2), encoding="utf-8")
    interrupted_profile = probe.with_suffix(".interrupted-profile.json")
    command = [
        str(executable),
        "index",
        "--db",
        str(probe),
        "--profile-json",
        str(interrupted_profile),
        "--profile-sqlite-config",
        str(config_path),
    ]
    child = subprocess.Popen(
        command,
        cwd=corpus_root,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    journal_paths = [
        Path(f"{probe}-journal"),
        Path(f"{probe}-wal"),
    ]
    deadline = time.monotonic() + 10.0
    journal_observed = False
    while child.poll() is None and time.monotonic() < deadline:
        if any(path.exists() for path in journal_paths):
            journal_observed = True
            break
        time.sleep(0.02)
    was_running = child.poll() is None
    if was_running:
        child.kill()
    interrupted_returncode = child.wait()
    with sqlite3.connect(probe) as connection:
        integrity = connection.execute("PRAGMA integrity_check").fetchone()[0]
        foreign_key_violations = connection.execute(
            "PRAGMA foreign_key_check"
        ).fetchall()
    resumed_profile = probe.with_suffix(".resumed-profile.json")
    resumed_command = command.copy()
    resumed_command[resumed_command.index(str(interrupted_profile))] = str(
        resumed_profile
    )
    resumed = subprocess.run(
        resumed_command,
        cwd=corpus_root,
        capture_output=True,
        text=True,
        check=False,
    )
    recovered_snapshot = _snapshot(probe, corpus_root, require_coverage=True)
    parity = recovered_snapshot["canonical_sha256"] == original["canonical_sha256"]
    return {
        "database": str(probe),
        "command": command,
        "termination": "SIGKILL of cidx during disposable re-index",
        "process_was_running": was_running,
        "journal_observed_before_kill": journal_observed,
        "interrupted_returncode": interrupted_returncode,
        "integrity_check": integrity,
        "foreign_key_violations": foreign_key_violations,
        "resume_returncode": resumed.returncode,
        "resume_stderr_tail": resumed.stderr[-2000:],
        "canonical_semantic_parity": parity,
        "recovered": (
            was_running
            and integrity == "ok"
            and not foreign_key_violations
            and resumed.returncode == 0
            and parity
        ),
    }


def baseline_fixture_path() -> Path:
    return Path(__file__).parent.joinpath(*S098_BASELINE_FIXTURE_NAME.split("/"))


def load_baseline_fixture(path: Path | None = None) -> dict[str, Any]:
    """Read the compact, checked-in excerpt of the S-071 authoritative run."""
    source = baseline_fixture_path() if path is None else path
    try:
        text = source.read_text(encoding="utf-8")
    except OSError as error:
        raise BaselineError(
            S098_REASON_ARTIFACT_MISSING, f"baseline fixture is unreadable: {error}"
        ) from error
    try:
        fixture = json.loads(text)
    except json.JSONDecodeError as error:
        raise BaselineError(
            S098_REASON_MALFORMED, f"baseline fixture is not JSON: {error}"
        ) from error
    if not isinstance(fixture, dict) or "source_artifact" not in fixture:
        raise BaselineError(
            S098_REASON_MALFORMED, "baseline fixture does not record its source artifact"
        )
    return fixture


def _numeric(value: Any, where: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise BaselineError(S098_REASON_MALFORMED, f"{where} is not a number")
    return value  # int counters stay integral; timings stay float


def _mapping(value: Any, where: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise BaselineError(S098_REASON_MALFORMED, f"{where} is not an object")
    return value


def _flag(value: Any, where: str) -> bool:
    if not isinstance(value, bool):
        raise BaselineError(S098_REASON_MALFORMED, f"{where} is not a boolean")
    return value


def _trial_series(section: Any, name: str, where: str) -> list[float]:
    entry = _mapping(_mapping(section, where).get(name), f"{where}.{name}")
    trials = entry.get("trials")
    if not isinstance(trials, list) or not trials:
        raise BaselineError(
            S098_REASON_MALFORMED, f"{where}.{name} has no trial series"
        )
    return [
        _numeric(value, f"{where}.{name}.trials[{index}]")
        for index, value in enumerate(trials)
    ]


def corpus_identity(report: Mapping[str, Any], *, case: str) -> dict[str, Any]:
    """Bind the corpus a case was actually measured on.

    The case key only carries shape, file count, and order. The number of
    distinct owned headers the shape generates - the parameter that decides how
    much routed-root work exists - lives per trial, so it is read from there and
    required to agree across every trial of the case.
    """
    trials = _mapping(report.get("individual_trials"), "individual_trials")
    prefix = f"{case}:trial-"
    descriptors: dict[str, dict[str, Any]] = {}
    for key in sorted(trials):
        if not key.startswith(prefix):
            continue
        trial = _mapping(trials[key], f"individual_trials.{key}")
        missing = [field for field in S098_CORPUS_FIELDS if field not in trial]
        if missing:
            raise BaselineError(
                S098_REASON_MALFORMED,
                f"individual_trials.{key} does not declare its corpus: {missing}",
            )
        descriptors[key] = {field: trial[field] for field in S098_CORPUS_FIELDS}
    if not descriptors:
        raise BaselineError(
            S098_REASON_MALFORMED, f"report records no per-trial corpus for {case!r}"
        )
    distinct = {json.dumps(value, sort_keys=True) for value in descriptors.values()}
    if len(distinct) != 1:
        raise BaselineError(
            S098_REASON_MALFORMED,
            f"{case!r} trials were measured on different corpora: {sorted(distinct)}",
        )
    descriptor = next(iter(descriptors.values()))
    if not isinstance(descriptor["shape"], str):
        raise BaselineError(
            S098_REASON_MALFORMED, f"{case!r} does not name the shape it was generated from"
        )
    return descriptor


def corpus_contract_identity(
    report: Mapping[str, Any], *, shape: str
) -> dict[str, Any]:
    """Bind the declared contract fields the measured shape depends on.

    A field the shape's corpus does not read cannot invalidate a comparison, so
    binding it would refuse a re-run that regenerated exactly the baseline's
    corpus. `many_header_target` is the case in point: it only sizes the
    `many-headers` shape.
    """
    contract = _mapping(report.get("corpus_contract"), "corpus_contract")
    fields = (
        *S098_CORPUS_CONTRACT_FIELDS,
        *S098_SHAPE_CONTRACT_FIELDS.get(shape, ()),
    )
    return {field: contract.get(field) for field in sorted(fields)}


def front_end_reuse_identity(report: Mapping[str, Any]) -> dict[str, Any]:
    """Bind the front-end reuse mechanism in force, normalising the control.

    S-071's report predates the S-075 contract and carries no `front_end_reuse`
    section; every report this harness emits carries `qualification_contract`,
    which declares the same shipped no-reuse control. Both mean the front end
    reused nothing, so both normalise to `S098_NO_REUSE_CONTROL` and compare
    equal. Any other mechanism, or any artifact construction or injection,
    still differs and still mismatches. A section that exists but does not say
    which mechanism was in force cannot be compared and is malformed.
    """
    section = report.get("front_end_reuse")
    if section is None:
        return dict(S098_NO_REUSE_CONTROL)
    section = _mapping(section, "front_end_reuse")
    if "mechanism" not in section:
        raise BaselineError(
            S098_REASON_MALFORMED, "front_end_reuse does not declare its mechanism"
        )
    if not isinstance(section["mechanism"], str):
        raise BaselineError(
            S098_REASON_MALFORMED, "front_end_reuse.mechanism is not a string"
        )
    return {
        "mechanism": section["mechanism"],
        "artifact_construction": _flag(
            section.get("artifact_construction", False),
            "front_end_reuse.artifact_construction",
        ),
        "artifact_injection": _flag(
            section.get("artifact_injection", False),
            "front_end_reuse.artifact_injection",
        ),
    }


def identity_fingerprint(
    report: Mapping[str, Any], *, case: str, stage: str
) -> dict[str, Any]:
    """Pin the reference-host measurement method, not the binary under test.

    The executable digest and checkout commit deliberately stay out: a
    candidate run must differ there. Everything that would silently invalidate
    a comparison against the frozen baseline stays in - the corpus the case was
    generated from, the contract fields that corpus depends on, and the
    front-end reuse mechanism in force. Nothing else: a field the measured
    corpus never reads would refuse an otherwise identical re-run.
    """
    identity = _mapping(report.get("identity"), "identity")
    host = _mapping(identity.get("host"), "identity.host")
    schema = _mapping(identity.get("schema"), "identity.schema")
    clang = _mapping(identity.get("clang"), "identity.clang")
    sqlite = _mapping(identity.get("sqlite"), "identity.sqlite")
    corpus = corpus_identity(report, case=case)
    return {
        "method": report.get("method"),
        "case": case,
        "stage": stage,
        "host_platform": host.get("platform"),
        "host_machine": host.get("machine"),
        "host_cpu_count": host.get("cpu_count"),
        "schema_version": schema.get("version"),
        "catalog_version": schema.get("catalog_version"),
        "catalog_hash": schema.get("catalog_hash"),
        "clang_resource_dir": clang.get("resource_dir"),
        "sqlite_version": sqlite.get("version"),
        "corpus": corpus,
        "corpus_contract": corpus_contract_identity(report, shape=corpus["shape"]),
        "front_end_reuse": front_end_reuse_identity(report),
    }


def root_observation(
    report: Mapping[str, Any],
    *,
    case: str = S098_BASELINE_CASE,
    stage: str = S098_BASELINE_STAGE,
) -> dict[str, Any]:
    """Derive the S-098 fixed routed-root series from an HSE-103 report.

    Accepts either the full authoritative report or the compact fixture: both
    carry the same `aggregates[case].stages[stage].profile_summary` shape.
    """
    report = _mapping(report, "report")
    missing = sorted(S098_REQUIRED_REPORT_KEYS.difference(report))
    if missing:
        raise BaselineError(S098_REASON_MALFORMED, f"report is missing {missing}")
    aggregates = _mapping(report.get("aggregates"), "aggregates")
    if case not in aggregates:
        raise BaselineError(S098_REASON_MALFORMED, f"report has no {case!r} aggregate")
    aggregate = _mapping(aggregates[case], f"aggregates.{case}")
    stages = _mapping(aggregate.get("stages"), f"aggregates.{case}.stages")
    if stage not in stages:
        raise BaselineError(
            S098_REASON_MALFORMED, f"{case!r} has no {stage!r} stage"
        )
    where = f"aggregates.{case}.stages.{stage}"
    summary = _mapping(stages[stage], where)
    profile = _mapping(summary.get("profile_summary"), f"{where}.profile_summary")
    timings = profile.get("timings")
    counters = profile.get("counters")

    series = {
        name: _trial_series(timings, name, f"{where}.profile_summary.timings")
        for name in S098_FIXED_ROOT_TIMINGS
    }
    registered = _trial_series(
        counters, S098_REGISTERED_BUDGET_COUNTER, f"{where}.profile_summary.counters"
    )
    observed = _trial_series(
        counters, S098_OBSERVED_TRAVERSAL_COUNTER, f"{where}.profile_summary.counters"
    )
    wall_trials = summary.get("wall_seconds_trials")
    if not isinstance(wall_trials, list) or not wall_trials:
        raise BaselineError(S098_REASON_MALFORMED, f"{where} has no wall trial series")
    wall = [
        _numeric(value, f"{where}.wall_seconds_trials[{index}]")
        for index, value in enumerate(wall_trials)
    ]

    lengths = {len(values) for values in series.values()}
    lengths.update({len(registered), len(observed), len(wall)})
    if len(lengths) != 1:
        raise BaselineError(
            S098_REASON_MALFORMED, f"{where} trial series have different lengths"
        )
    trials = lengths.pop()
    declared = aggregate.get("trial_count")
    if not isinstance(declared, int) or isinstance(declared, bool):
        raise BaselineError(
            S098_REASON_MALFORMED, f"aggregates.{case}.trial_count is not an integer"
        )
    if declared != trials:
        raise BaselineError(
            S098_REASON_MALFORMED,
            f"aggregates.{case}.trial_count {declared} != {trials} measured trials",
        )

    fixed = [sum(series[name][index] for name in S098_FIXED_ROOT_TIMINGS) for index in range(trials)]
    other = [sum(series[name][index] for name in S098_OTHER_ROOT_TIMINGS) for index in range(trials)]
    parity_failures = [
        str(failure) for failure in report.get("parity_failures") or []
    ] + [f"{case}: {failure}" for failure in aggregate.get("parity_failures") or []]
    quiescence = _mapping(report.get("host_quiescence"), "host_quiescence")
    over_budget = [
        index + 1
        for index, (seen, allowed) in enumerate(zip(observed, registered))
        if seen > allowed
    ]
    return {
        "case": case,
        "stage": stage,
        "method": report.get("method"),
        "identity": identity_fingerprint(report, case=case, stage=stage),
        "authoritative_timing": bool(report.get("authoritative_timing")),
        "quiescent": bool(quiescence.get("quiescent")),
        "competing_cidx_indexers": list(
            quiescence.get("competing_cidx_indexers") or []
        ),
        "trial_count": trials,
        "parity_failures": parity_failures,
        "semantic_parity": bool(summary.get("semantic_parity")),
        "canonical_sha256_trials": list(summary.get("canonical_sha256_trials") or []),
        "wall_seconds_trials": wall,
        "wall_seconds_median": statistics.median(wall),
        "root_timing_trials": series,
        "symbol_root_trials": series[S098_SYMBOL_ROOT_TIMING],
        "symbol_root_median": statistics.median(series[S098_SYMBOL_ROOT_TIMING]),
        "other_root_trials": other,
        "other_root_median": statistics.median(other),
        "fixed_root_trials": fixed,
        "fixed_root_median": statistics.median(fixed),
        "registered_root_traversal_budget_trials": registered,
        "observed_root_traversal_trials": observed,
        "registered_root_traversal_budget": max(registered),
        "observed_root_traversals": max(observed),
        "traversal_budget_exceeded_trials": over_budget,
    }


def backlog_artifact_registry(
    *, task: str, project: str, backlog: str | None = None
) -> tuple[list[Any], str | None]:
    """Read one task's artifact records and the store's artifact root.

    The store location is whatever `backlog where` reports; this never assumes
    a repository `.backlog` directory or a disposable temporary path.
    """
    executable = backlog or os.environ.get("BACKLOG_CLI") or shutil.which("backlog")
    if not executable:
        raise BaselineError(
            S098_REASON_ARTIFACT_MISSING,
            "no backlog CLI on PATH and BACKLOG_CLI is unset",
        )

    def query(arguments: list[str]) -> Any:
        completed = subprocess.run(
            [executable, *arguments, "--project", project, "--json"],
            capture_output=True,
            text=True,
            check=False,
        )
        if completed.returncode != 0:
            raise BaselineError(
                S098_REASON_ARTIFACT_MISSING,
                f"backlog {' '.join(arguments)} failed: {completed.stderr.strip()}",
            )
        try:
            return json.loads(completed.stdout)
        except json.JSONDecodeError as error:
            raise BaselineError(
                S098_REASON_ARTIFACT_MISSING,
                f"backlog {' '.join(arguments)} returned no JSON: {error}",
            ) from error

    records = query(["artifact", "list", task])
    location = query(["where"])
    if not isinstance(records, list):
        raise BaselineError(
            S098_REASON_ARTIFACT_MISSING, f"{task} artifact listing is not a list"
        )
    return records, _mapping(location, "backlog where").get("artifacts_dir")


def resolve_baseline_artifact(
    *,
    task: str = S098_BASELINE_ARTIFACT_TASK,
    name: str = S098_BASELINE_ARTIFACT_NAME,
    project: str = S098_BACKLOG_PROJECT,
    registry: Any = backlog_artifact_registry,
) -> Path:
    """Locate the authoritative report through the backlog artifact registry."""
    records, artifacts_dir = registry(task=task, project=project)
    if not artifacts_dir:
        raise BaselineError(
            S098_REASON_ARTIFACT_MISSING, "backlog store reports no artifact directory"
        )
    matches = [
        record
        for record in records
        if isinstance(record, Mapping)
        and PurePosixPath(str(record.get("rel_path", ""))).name == name
    ]
    if not matches:
        raise BaselineError(
            S098_REASON_ARTIFACT_MISSING,
            f"{task} registers no artifact named {name!r}",
        )
    if len(matches) > 1:
        raise BaselineError(
            S098_REASON_ARTIFACT_MISMATCH,
            f"{task} registers {len(matches)} artifacts named {name!r}",
        )
    relative = PurePosixPath(str(matches[0]["rel_path"]))
    if relative.is_absolute() or ".." in relative.parts:
        raise BaselineError(
            S098_REASON_ARTIFACT_MISMATCH,
            f"registered artifact path escapes the store: {relative}",
        )
    root = Path(artifacts_dir)
    # Records are relative to the store root and repeat the directory segment
    # that `artifacts_dir` already names.
    if relative.parts and relative.parts[0] == root.name:
        relative = PurePosixPath(*relative.parts[1:])
    path = root.joinpath(*relative.parts)
    if not path.is_file():
        raise BaselineError(
            S098_REASON_ARTIFACT_MISSING,
            f"registered artifact is not present on disk: {path}",
        )
    return path


def verify_baseline_artifact(
    *,
    fixture: Mapping[str, Any] | None = None,
    project: str = S098_BACKLOG_PROJECT,
    registry: Any = backlog_artifact_registry,
) -> dict[str, Any]:
    """Resolve, digest-verify, and shape-verify the authoritative S-071 report.

    The size and digest checked are the ones the checked-in fixture recorded,
    measured against the bytes the registry actually resolved to. Returns the
    evidence of that verification alongside the parsed report so a caller can
    report what it verified rather than assert it.
    """
    fixture = load_baseline_fixture() if fixture is None else fixture
    expected = _mapping(fixture.get("source_artifact"), "fixture.source_artifact")
    task = str(expected.get("backlog_task", S098_BASELINE_ARTIFACT_TASK))
    name = str(expected.get("name", S098_BASELINE_ARTIFACT_NAME))
    path = resolve_baseline_artifact(
        task=task, name=name, project=project, registry=registry
    )
    size = path.stat().st_size
    if size != expected.get("bytes"):
        raise BaselineError(
            S098_REASON_ARTIFACT_MISMATCH,
            f"{path} is {size} bytes, expected {expected.get('bytes')}",
        )
    digest = _sha256(path)
    if digest != expected.get("sha256"):
        raise BaselineError(
            S098_REASON_ARTIFACT_MISMATCH,
            f"{path} digest {digest} != recorded {expected.get('sha256')}",
        )
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise BaselineError(
            S098_REASON_MALFORMED, f"{path} is not JSON: {error}"
        ) from error
    if root_observation(report) != root_observation(fixture):
        raise BaselineError(
            S098_REASON_ARTIFACT_MISMATCH,
            f"{path} does not match the checked-in baseline fixture",
        )
    return {
        "backlog_task": task,
        "name": name,
        "resolved_path": str(path),
        "sha256": digest,
        "bytes": size,
        "verified": True,
        "report": report,
    }


def load_baseline_artifact(
    *,
    fixture: Mapping[str, Any] | None = None,
    project: str = S098_BACKLOG_PROJECT,
    registry: Any = backlog_artifact_registry,
) -> dict[str, Any]:
    """The authoritative S-071 report, verified. See `verify_baseline_artifact`."""
    return verify_baseline_artifact(
        fixture=fixture, project=project, registry=registry
    )["report"]


def _decision_shell(case: str, stage: str) -> dict[str, Any]:
    return {
        "contract": S098_CONTRACT_VERSION,
        "case": case,
        "stage": stage,
        "minimum_trials": MINIMUM_TRIALS,
        "fixed_root_budget_seconds": S098_FIXED_ROOT_BUDGET_SECONDS,
        "fixed_root_budget_is_strict": True,
        "symbol_root_budget_seconds": S098_SYMBOL_ROOT_BUDGET_SECONDS,
        "symbol_root_budget_owner": S098_SYMBOL_ROOT_BUDGET_OWNER,
        "symbol_root_budget_is_advisory_here": True,
        "baseline_artifact": None,
        "baseline_artifact_verified": False,
        "ship_eligible": False,
        "reason": None,
        "reasons": [],
        "baseline": None,
        "candidate": None,
    }


def qualify(
    report: Mapping[str, Any],
    *,
    fixture: Mapping[str, Any] | None = None,
    project: str = S098_BACKLOG_PROJECT,
    registry: Any = backlog_artifact_registry,
    case: str = S098_BASELINE_CASE,
    stage: str = S098_BASELINE_STAGE,
) -> dict[str, Any]:
    """Qualify a candidate against the verified authoritative baseline.

    This is the supported entry point for a ship decision: the authoritative
    report is resolved through the backlog artifact registry and its recorded
    size, digest, and derived shape are verified *before* anything is compared.
    A failure there is a named rejection, never a silent fall back to the
    checked-in fixture. `fusion_decision` remains available for offline work on
    the fixture alone, and reports `baseline_artifact_verified: False`.
    """
    frozen = load_baseline_fixture() if fixture is None else fixture
    try:
        evidence = verify_baseline_artifact(
            fixture=frozen, project=project, registry=registry
        )
    except BaselineError as error:
        decision = _decision_shell(case, stage)
        decision["baseline_artifact"] = frozen.get("source_artifact")
        decision["reason"] = error.reason
        decision["reasons"] = [error.reason]
        decision["detail"] = f"authoritative artifact: {error.detail}"
        return decision
    report_only = {
        key: value for key, value in evidence.items() if key != "report"
    }
    decision = fusion_decision(
        report, baseline=evidence["report"], case=case, stage=stage
    )
    decision["baseline_artifact"] = report_only
    decision["baseline_artifact_verified"] = True
    return decision


def fusion_decision(
    report: Mapping[str, Any],
    *,
    baseline: Mapping[str, Any] | None = None,
    case: str = S098_BASELINE_CASE,
    stage: str = S098_BASELINE_STAGE,
) -> dict[str, Any]:
    """Decide S-098 ship eligibility for one candidate report.

    Ship eligibility requires all of S-098 AC #1720/#1721: at least three
    authoritative quiescent trials, no parity failure, observed traversals
    within the registered budget, and a median fixed routed-root time strictly
    below `S098_FIXED_ROOT_BUDGET_SECONDS`. Exactly at the threshold is a
    rejection. The T-139 symbol budget is recorded and reported, never used to
    widen this decision.
    """
    frozen = load_baseline_fixture() if baseline is None else baseline
    decision = _decision_shell(case, stage)
    decision["baseline_artifact"] = frozen.get("source_artifact")
    try:
        frozen_observation = root_observation(
            frozen, case=S098_BASELINE_CASE, stage=S098_BASELINE_STAGE
        )
    except BaselineError as error:
        decision["reason"] = error.reason
        decision["reasons"] = [error.reason]
        decision["detail"] = f"baseline: {error.detail}"
        return decision
    decision["baseline"] = frozen_observation
    try:
        candidate = root_observation(report, case=case, stage=stage)
    except BaselineError as error:
        decision["reason"] = error.reason
        decision["reasons"] = [error.reason]
        decision["detail"] = f"candidate: {error.detail}"
        return decision

    decision["candidate"] = candidate
    reasons = set()
    if candidate["identity"] != frozen_observation["identity"]:
        reasons.add(S098_REASON_IDENTITY)
    if not candidate["authoritative_timing"] or not candidate["quiescent"]:
        reasons.add(S098_REASON_CONTENDED)
    if candidate["trial_count"] < MINIMUM_TRIALS:
        reasons.add(S098_REASON_TRIALS)
    if candidate["parity_failures"] or not candidate["semantic_parity"]:
        reasons.add(S098_REASON_PARITY)
    if candidate["traversal_budget_exceeded_trials"]:
        reasons.add(S098_REASON_TRAVERSAL_BUDGET)
    if not candidate["fixed_root_median"] < S098_FIXED_ROOT_BUDGET_SECONDS:
        reasons.add(S098_REASON_FIXED_ROOT_BUDGET)

    ordered = [reason for reason in S098_REJECTION_REASONS if reason in reasons]
    decision["reasons"] = ordered
    decision["reason"] = ordered[0] if ordered else None
    decision["ship_eligible"] = not ordered
    decision["fixed_root_median_seconds"] = candidate["fixed_root_median"]
    decision["baseline_fixed_root_median_seconds"] = frozen_observation[
        "fixed_root_median"
    ]
    decision["fixed_root_improvement_seconds"] = (
        frozen_observation["fixed_root_median"] - candidate["fixed_root_median"]
    )
    decision["symbol_root_median_seconds"] = candidate["symbol_root_median"]
    decision["symbol_root_budget_met"] = (
        candidate["symbol_root_median"] < S098_SYMBOL_ROOT_BUDGET_SECONDS
    )
    return decision


def _host_quiescence() -> dict[str, Any]:
    process_text = _command_text(["ps", "-axo", "pid=,pcpu=,command="])
    competitors = [
        line.strip()
        for line in process_text.splitlines()
        if "cidx index" in line and "production.py" not in line
    ]
    load_average = os.getloadavg()
    return {
        "observed_at_unix": time.time(),
        "load_average": list(load_average),
        "competing_cidx_indexers": competitors,
        "quiescent": not competitors,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cidx", type=Path, required=True)
    parser.add_argument(
        "--uninstrumented-cidx",
        type=Path,
        help=(
            "otherwise-equivalent executable built before profiling "
            "instrumentation, required for the 1%% disabled-overhead gate"
        ),
    )
    parser.add_argument("--checkout", type=Path, default=Path.cwd())
    parser.add_argument("--self-compile-db", type=Path)
    parser.add_argument("--representative-files", type=int, default=32)
    parser.add_argument("--scale-files", type=int, default=1000)
    parser.add_argument(
        "--many-header-target", type=int, default=DEFAULT_MANY_HEADER_TARGET
    )
    parser.add_argument("--trials", type=int, default=MINIMUM_TRIALS)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--skip-self-index", action="store_true")
    parser.add_argument("--skip-sqlite-matrix", action="store_true")
    parser.add_argument(
        "--front-end-reuse",
        choices=("none",),
        default="none",
        help="front-end reuse mechanism; ADR-014 currently ships only none",
    )
    parser.add_argument(
        "--no-front-end-reuse",
        action="store_true",
        help="explicitly select the no-reuse control for diagnosis",
    )
    parser.add_argument(
        "--allow-contended-host",
        action="store_true",
        help="record non-authoritative smoke data even when cidx competitors exist",
    )
    args = parser.parse_args()

    checkout = args.checkout.resolve()
    executable = args.cidx.resolve()
    uninstrumented = (
        args.uninstrumented_cidx.resolve()
        if args.uninstrumented_cidx is not None
        else None
    )
    self_compile_db = (
        args.self_compile_db.resolve()
        if args.self_compile_db
        else checkout / "build" / "compile_commands.json"
    )
    if args.trials < MINIMUM_TRIALS:
        raise SystemExit(f"--trials must be at least {MINIMUM_TRIALS}")
    if args.representative_files < 1 or args.scale_files < 1:
        raise SystemExit("corpus sizes must be positive")
    if platform.system() == "Darwin" and not args.work_root.name.endswith(".noindex"):
        raise SystemExit("macOS --work-root must end in .noindex")
    if args.work_root.exists():
        raise SystemExit(f"work root already exists: {args.work_root}")
    if args.output.exists():
        raise SystemExit(f"output already exists: {args.output}")
    if _path_within(args.work_root, checkout) or _path_within(args.output, checkout):
        raise SystemExit("work root and output must remain outside the checkout")
    if not executable.is_file():
        raise SystemExit(f"cidx executable not found: {executable}")
    if uninstrumented is not None and not uninstrumented.is_file():
        raise SystemExit(f"uninstrumented cidx executable not found: {uninstrumented}")
    if not args.skip_self_index and not self_compile_db.is_file():
        raise SystemExit(f"self compile database not found: {self_compile_db}")

    quiescence = _host_quiescence()
    if not quiescence["quiescent"] and not args.allow_contended_host:
        raise SystemExit(
            "host is not quiescent; competing cidx indexers: "
            + "; ".join(quiescence["competing_cidx_indexers"])
        )

    args.work_root.mkdir(parents=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report: dict[str, Any] = {
        "method": "HSE-103",
        "authoritative_timing": quiescence["quiescent"],
        "host_quiescence": quiescence,
        "identity": environment_identity(
            executable,
            checkout,
            [] if args.skip_self_index else [self_compile_db],
        ),
        "corpus_contract": {
            "baseline_distinct_owned_headers": BASELINE_DISTINCT_OWNED_HEADERS,
            "many_header_target": args.many_header_target,
            "shapes": ["baseline", "header-heavy", "many-headers", "fan-in"],
            "orders": ["forward", "reverse"],
        },
        "attribution_experiments": ATTRIBUTION_EXPERIMENTS,
        "individual_trials": {},
        "aggregates": {},
        "sqlite_matrix": {},
        "disabled_profiling_overhead": None,
        "parity_failures": [],
        "front_end_reuse": qualification_contract(
            trials=args.trials, disabled=args.no_front_end_reuse
        ),
    }
    if uninstrumented is not None:
        report["identity"]["uninstrumented_executable"] = {
            "path": str(uninstrumented),
            "sha256": _sha256(uninstrumented),
            "version": _command_text([str(uninstrumented), "--version"]),
        }

    case_specs = [
        ("baseline", args.representative_files, "forward"),
        ("baseline", args.scale_files, "forward"),
        ("baseline", args.representative_files, "reverse"),
        ("baseline", args.scale_files, "reverse"),
        ("header-heavy", args.representative_files, "forward"),
        ("many-headers", args.representative_files, "forward"),
        ("fan-in", args.representative_files, "forward"),
    ]
    for shape, files, order in case_specs:
        case_key = f"{shape}:{files}:{order}"
        trials = []
        for trial in range(1, args.trials + 1):
            case_root = args.work_root / "synthetic" / case_key / f"trial-{trial}"
            case_root.mkdir(parents=True)
            result = run_synthetic_case(
                executable,
                files,
                shape,
                args.many_header_target,
                order,
                case_root,
            )
            report["individual_trials"][f"{case_key}:trial-{trial}"] = result
            trials.append(result)
        aggregate = aggregate_trials(trials)
        report["aggregates"][case_key] = aggregate
        report["parity_failures"].extend(
            f"{case_key}: {failure}" for failure in aggregate["parity_failures"]
        )

    if not args.skip_self_index:
        self_trials = []
        for trial in range(1, args.trials + 1):
            case_root = args.work_root / "self-index" / f"trial-{trial}"
            case_root.mkdir(parents=True)
            result = run_self_index_case(
                executable, checkout, self_compile_db, case_root
            )
            report["individual_trials"][f"self-index:trial-{trial}"] = result
            self_trials.append(result)
        aggregate = aggregate_trials(self_trials)
        report["aggregates"]["self-index"] = aggregate
        report["parity_failures"].extend(
            f"self-index: {failure}" for failure in aggregate["parity_failures"]
        )

    if not args.skip_sqlite_matrix:
        for name, configuration in SQLITE_EXPERIMENTS.items():
            trials = []
            for trial in range(1, args.trials + 1):
                case_root = args.work_root / "sqlite-matrix" / name / f"trial-{trial}"
                case_root.mkdir(parents=True)
                result = run_synthetic_case(
                    executable,
                    args.representative_files,
                    "baseline",
                    args.many_header_target,
                    "forward",
                    case_root,
                    sqlite_experiment=configuration,
                )
                trials.append(result)
            aggregate = aggregate_trials(trials)
            control_database = (
                args.work_root
                / "sqlite-matrix"
                / name
                / "trial-1"
                / "cache"
                / "index.db"
            )
            report["sqlite_matrix"][name] = {
                "configuration": configuration,
                "aggregate": aggregate,
                "recovery": recovery_probe(
                    executable,
                    control_database,
                    args.work_root / "sqlite-matrix" / name / "trial-1" / "corpus",
                    configuration,
                ),
                "production_change_recommended": False,
                "recommendation_gate": (
                    "Requires integrity, cidx interruption, and recovery evidence "
                    "on the target host before any production-profile change."
                ),
            }
            report["parity_failures"].extend(
                f"sqlite:{name}: {failure}" for failure in aggregate["parity_failures"]
            )

    if uninstrumented is not None:
        comparison = compare_disabled_overhead(
            executable,
            uninstrumented,
            args.scale_files,
            args.trials,
            args.work_root / "disabled-overhead",
        )
        report["disabled_profiling_overhead"] = comparison
        report["parity_failures"].extend(comparison["parity_failures"])

    report["attribution_results"] = attribution_summary(
        report["aggregates"], args.representative_files, args.scale_files
    )
    report["s098_root_fusion"] = qualify(
        report, case=f"header-heavy:{args.representative_files}:forward"
    )
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(args.output)
    return 1 if report["parity_failures"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
