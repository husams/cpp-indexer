#!/usr/bin/env python3
"""S-078 integrated qualification matrix for the production indexing SLO.

The earlier PERF-002 harnesses each qualified one mechanism in isolation:
`run.py` the writer commit A/B, `production.py` the HSE-103 corpus matrix,
`parallel_matrix.py` the S-074 serial/parallel arms. This module is the
integration step. It answers the questions that only exist once every
mechanism is in the same binary:

* do the *modes* publish the same facts? Serial against parallel, a fresh
  extraction against a translation-unit cache hit, front-end reuse enabled
  against explicitly disabled, inline transforms against deferred ones, an
  in-place update against a clean rebuild -- each pair is declared equivalent
  by its own story, and this is where all of them are compared at once, on one
  corpus, with one binary;
* do the *bounds* hold when they are actually squeezed? `--jobs`,
  `--max-queue-bytes`, `--max-queue-items` and `--memory-budget-bytes` are
  operating knobs, so the matrix runs the tightest settings the CLI accepts
  and requires the same facts out of them;
* does the *pre-feature* comparison hold up? The A/B against the commit before
  PERF-002 started is a timing comparison with an enumerated semantic delta,
  not a parity claim -- the candidate legitimately emits facts the pre-feature
  binary lost, and that difference is characterised here rather than hidden.

Two deliberate non-goals. This module does not reimplement guarantees that
already have executable owners: per-point clean-rebuild failure injection,
worker-completion reordering, artifact tampering and schema migration are all
qualified by named tests in the repository, and the matrix *runs* those tests
and records their result instead of writing a second, weaker version of them.
And it does not measure the SLO itself: the arithmetic lives in `slo.py`, so
the contract can be tested with no binary, no corpus and no store.

Generated corpora, caches, databases and reports all live under an explicit
`--work-root` outside the checkout. The committed `index.db` is never opened.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import sqlite3
import subprocess
import sys
import time
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

# Sibling modules, imported the same way whichever entry point is used, so a
# test that imports this as `benchmarks.indexing.integrated` sees the same
# module objects -- and therefore the same exception classes -- as a direct
# script run does.
try:  # imported as `benchmarks.indexing.integrated`
    from . import production as PRODUCTION
    from . import run as HSE95
    from . import slo as SLO
except ImportError:  # run directly as a script
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import production as PRODUCTION  # type: ignore[no-redef]
    import run as HSE95  # type: ignore[no-redef]
    import slo as SLO  # type: ignore[no-redef]

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
DUMP_LAYER0 = REPOSITORY_ROOT / "scripts" / "dump_layer0.sh"

#: Row counts that legitimately differ between two runs of the same corpus.
#: `meta` carries timestamps and run identities; the artifact tables hold the
#: optional translation-unit cache, which is an accelerator and not a fact --
#: a parallel run does not populate it at all. Both are reported separately so
#: the difference stays visible instead of being folded into a parity claim.
VOLATILE_TABLES = frozenset({"meta"})
ACCELERATOR_TABLES = frozenset(
    {"artifact", "artifact_relation", "artifact_lease", "artifact_pin",
     "artifact_identity_map"}
)

#: Executable qualification this matrix cites rather than reimplements. Each
#: entry names the ctest test, its label, and exactly which acceptance
#: criterion its existence is evidence for.
CITED_QUALIFICATION = (
    {
        "test": "clean_rebuild_process_test",
        "label": "clang",
        "covers": (
            "per-point clean-rebuild failure injection, SIGINT inside and "
            "after the publication window, sidecar refusal, and the "
            "backup/restore round trip"
        ),
        "criteria": ["AC11", "AC10"],
    },
    {
        "test": "clean_rebuild_test",
        "label": "default",
        "covers": "clean-rebuild capture, verification and publication contract",
        "criteria": ["AC11"],
    },
    {
        "test": "parallel_index_database_test",
        "label": "clang",
        "covers": (
            "worker completion forced into the reverse of dispatch order, "
            "delete/re-emit, repeated declarations and per-file routed "
            "association, compared row by row"
        ),
        "criteria": ["AC8", "AC7"],
    },
    {
        "test": "parallel_extraction_test",
        "label": "default",
        "covers": "bounded scheduler queue, reorder buffer and worker budgets",
        "criteria": ["AC12"],
    },
    {
        "test": "tu_fact_cache_test",
        "label": "default",
        "covers": (
            "cache decision taxonomy: hit, missing, stale, corrupt, "
            "incompatible, partial, truncated, untrusted, unavailable"
        ),
        "criteria": ["AC10"],
    },
    {
        "test": "tu_fact_cache_integration_test",
        "label": "clang",
        "covers": "end-to-end cache replay against a real extraction",
        "criteria": ["AC10", "AC7"],
    },
    {
        "test": "storage_artifact_test",
        "label": "default",
        "covers": "artifact retention, leases, pins and recovery bounds",
        "criteria": ["AC12", "AC10"],
    },
    {
        "test": "storage_migration_test",
        "label": "default",
        "covers": "schema migration from older databases, in place",
        "criteria": ["AC10"],
    },
    {
        "test": "extraction_plan_test",
        "label": "default",
        "covers": "incremental versus full transform planning and generations",
        "criteria": ["AC7"],
    },
    {
        "test": "fact_batch_complexity_test",
        "label": "default",
        "covers": "FactBatch emitter scaling gate",
        "criteria": ["AC13"],
    },
    {
        "test": "cli_test",
        "label": "default",
        "covers": (
            "registered provider workload, multi-rule whole-TU traversal "
            "budget aggregation, and refusal of an undeclared walk"
        ),
        "criteria": ["AC13", "AC12"],
    },
)


#: Affordable sizes that still exercise every axis and every bound. The
#: expensive part of a full run is the pre-feature A/B, which indexes the same
#: corpus with an executable that predates every PERF-002 change and is an
#: order of magnitude slower cold. Its job is a *ratio*, and a ratio does not
#: need the largest corpus available -- the absolute figures the acceptance
#: criteria name come from `production.py`, which is measured separately.
#:
#: `--profile quick` is what an ordinary change should run. `--profile full`
#: is for the scheduled production-scale job. Whichever is used, the corpus
#: size lands in the report as part of the case key, so a decision can never
#: read as though it were measured at a scale it was not.
SCALE_PROFILES: dict[str, dict[str, int]] = {
    "quick": {
        "equivalence_files": 6,
        "bounds_files": 8,
        "pre_feature_files": 64,
    },
    "full": {
        "equivalence_files": 12,
        "bounds_files": 24,
        "pre_feature_files": 1000,
    },
}


class MatrixError(RuntimeError):
    """The matrix cannot produce trustworthy evidence."""


# --- environment and identity ----------------------------------------------


def probe_capabilities(executable: Path) -> dict[str, Any]:
    """Which index options an executable advertises.

    The pre-feature baseline predates `--profile-json`, `--jobs`, `--clean`
    and `--defer-transforms`, so the matrix has to know what it may pass to a
    given binary. Recording the probe keeps a later reader from assuming the
    baseline was run with telemetry it never had.
    """
    completed = subprocess.run(
        [str(executable), "index", "--help"],
        capture_output=True,
        text=True,
        check=False,
    )
    help_text = completed.stdout + completed.stderr
    flags = (
        "--profile-json",
        "--jobs",
        "--clean",
        "--defer-transforms",
        "--no-front-end-reuse",
        "--max-queue-bytes",
        "--max-queue-items",
        "--memory-budget-bytes",
    )
    return {
        "executable": str(executable),
        "sha256": PRODUCTION._sha256(executable),
        "version": PRODUCTION._command_text([str(executable), "--version"]),
        "index_options": {flag: flag in help_text for flag in flags},
    }


def _run(
    command: Sequence[str],
    *,
    cwd: Path,
    environment: Mapping[str, str],
    sink: Path,
    label: str,
) -> dict[str, Any]:
    """Run one child with wall, child-CPU and peak-RSS measurement.

    The measurement wrapper is `run.py --measure-child`, the same one the
    HSE-95 harness uses, launched fresh per command so its
    `RUSAGE_CHILDREN` high-water mark belongs to this command alone. It is
    launched with `cwd`, which the measured child inherits. Unlike
    `run.run_timed` this returns a non-zero result instead of raising, because
    parts of the matrix deliberately exercise commands that must fail.
    """
    sink.mkdir(parents=True, exist_ok=True)
    stdout_path = sink / f"{label}.stdout"
    stderr_path = sink / f"{label}.stderr"
    text_command = [str(part) for part in command]
    started = time.monotonic()
    measurement = subprocess.run(
        [
            sys.executable, str(Path(HSE95.__file__).resolve()),
            "--measure-child",
            "--stdout", str(stdout_path),
            "--stderr", str(stderr_path),
            "--", *text_command,
        ],
        env=dict(environment),
        cwd=str(cwd),
        capture_output=True,
        text=True,
        check=False,
    )
    if not measurement.stdout.strip():
        raise MatrixError(
            f"{label}: measurement wrapper produced no metrics: "
            f"{measurement.stderr[-2000:]}"
        )
    metrics = json.loads(measurement.stdout)
    metrics.update(
        {
            "command": text_command,
            "measured_wall_seconds": time.monotonic() - started,
            "stdout": stdout_path.read_text(encoding="utf-8"),
            "stderr": stderr_path.read_text(encoding="utf-8"),
        }
    )
    return metrics


def _environment(cache: Path, extra: Mapping[str, str] | None = None) -> dict[str, str]:
    environment = dict(os.environ)
    environment["INDEXER_CACHE"] = str(cache)
    # A stray value from the developer's shell would silently change which
    # mechanism is under test, so the axes always set their own.
    for name in ("CIDX_TU_FACT_CACHE", "CIDX_TU_FACT_CACHE_ROOT",
                 "CIDX_CLEAN_REBUILD_FAIL_AT"):
        environment.pop(name, None)
    if extra:
        environment.update(extra)
    return environment


# --- observation ------------------------------------------------------------


def table_counts(database: Path) -> dict[str, int]:
    """Row count for every user table, including those Layer-0 omits."""
    with sqlite3.connect(database) as connection:
        names = [
            row[0]
            for row in connection.execute(
                "SELECT name FROM sqlite_master WHERE type='table' "
                "AND name NOT LIKE 'sqlite_%' ORDER BY name"
            )
        ]
        return {
            name: connection.execute(f"SELECT COUNT(*) FROM {name}").fetchone()[0]
            for name in names
            if name not in VOLATILE_TABLES
        }


def _sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def normalized_layer0_digest(database: Path, corpus_root: Path, cache: Path) -> str:
    """`scripts/dump_layer0.sh` output, path-normalised, hashed.

    Two arms run against the same corpus but their own cache directory, so the
    cache path has to be normalised out before the projections can be
    compared. The corpus path is normalised too, so the same digest also holds
    across a re-run under a different work root.
    """
    completed = subprocess.run(
        ["sh", str(DUMP_LAYER0), str(database)],
        capture_output=True,
        text=True,
        check=True,
    )
    text = re.sub(
        r"build:[0-9a-f]{40}",
        "build:<build>",
        completed.stdout.replace(str(cache), "<CACHE>").replace(
            str(corpus_root), "<CORPUS>"
        ),
    )
    return _sha256_text(text)


def observe(cache: Path, corpus_root: Path, *, require_coverage: bool = True
            ) -> dict[str, Any]:
    """Everything an equivalence comparison needs from one finished arm."""
    database = cache / "index.db"
    snapshot = HSE95.database_snapshot(
        database, corpus_root, require_coverage=require_coverage,
        capture_canonical=True,
    )
    counts = table_counts(database)
    return {
        "canonical_sha256": snapshot["canonical_sha256"],
        "normalized_layer0_sha256": normalized_layer0_digest(
            database, corpus_root, cache
        ),
        "table_counts": {
            name: value
            for name, value in counts.items()
            if name not in ACCELERATOR_TABLES
        },
        "accelerator_table_counts": {
            name: counts[name] for name in sorted(ACCELERATOR_TABLES & counts.keys())
        },
        "integrity_check": snapshot["integrity_check"],
        "foreign_key_check": snapshot["foreign_key_check"],
        "schema_version": snapshot["schema_version"],
        "catalog_version": snapshot["catalog_version"],
        "catalog_hash": snapshot["catalog_hash"],
        "canonical_row_counts": snapshot["canonical_row_counts"],
    }


def profile_counters(path: Path) -> dict[str, Any]:
    """Counters from an index profile, or an empty mapping when absent."""
    if not path.is_file():
        return {}
    document = json.loads(path.read_text(encoding="utf-8"))
    summary = document.get("summary")
    if not isinstance(summary, Mapping):
        return {}
    counters = summary.get("counters")
    timings = summary.get("timings")
    return {
        "counters": dict(counters) if isinstance(counters, Mapping) else {},
        "timings": dict(timings) if isinstance(timings, Mapping) else {},
    }


# --- arms -------------------------------------------------------------------


def run_arm(
    executable: Path,
    corpus: Any,
    arm_root: Path,
    *,
    arm: str,
    index_args: Sequence[str] = (),
    environment: Mapping[str, str] | None = None,
    defer_transforms: bool = False,
    clean_rebuild: bool = False,
    profile: bool = True,
) -> dict[str, Any]:
    """Import, index and resolve one corpus under one mode, then observe it.

    Every arm gets a private cache, so no arm can read another's database. The
    corpus is shared on purpose: the translation-unit cache identity is keyed
    on the canonical source path, so a cached arm and a fresh arm have to see
    the same paths for the comparison to mean anything.
    """
    cache = arm_root / "cache"
    cache.mkdir(parents=True)
    logs = arm_root / "logs"
    steps: list[dict[str, Any]] = []
    environment_extra = dict(environment or {})

    imported = _run(
        [
            str(executable), "import", "--db", str(corpus.compile_commands),
            "--name", f"s078-{corpus.shape}", "--force",
        ],
        cwd=corpus.root,
        environment=_environment(cache),
        sink=logs,
        label="import",
    )
    imported["step"] = "import"
    steps.append(imported)
    if imported["returncode"] != 0:
        raise MatrixError(f"{arm}: import failed\n{imported['stderr'][-2000:]}")

    profile_path = arm_root / "index.profile.json"
    index_command: list[str] = [str(executable), "index", *index_args]
    if clean_rebuild:
        index_command = [str(executable), "index", "rebuild", "--clean", *index_args]
    if defer_transforms:
        index_command.append("--defer-transforms")
    if profile:
        index_command.extend(["--profile-json", str(profile_path)])

    if clean_rebuild:
        # A clean rebuild publishes over a database in service, so the arm
        # first builds an ordinary index and then rebuilds it. Anything else
        # would compare a rebuild against nothing.
        seed = _run(
            [str(executable), "index"],
            cwd=corpus.root,
            environment=_environment(cache, environment_extra),
            sink=logs,
            label="seed-index",
        )
        seed["step"] = "seed-index"
        steps.append(seed)
        if seed["returncode"] != 0:
            raise MatrixError(f"{arm}: seed index failed\n{seed['stderr'][-2000:]}")

    indexed = _run(
        index_command,
        cwd=corpus.root,
        environment=_environment(cache, environment_extra),
        sink=logs,
        label="index",
    )
    indexed["step"] = "index"
    steps.append(indexed)
    if indexed["returncode"] != 0:
        raise MatrixError(f"{arm}: index failed\n{indexed['stderr'][-2000:]}")

    resolved = _run(
        [str(executable), "resolve"],
        cwd=corpus.root,
        environment=_environment(cache, environment_extra),
        sink=logs,
        label="resolve",
    )
    resolved["step"] = "resolve"
    steps.append(resolved)
    if resolved["returncode"] != 0:
        raise MatrixError(f"{arm}: resolve failed\n{resolved['stderr'][-2000:]}")

    observation = observe(cache, corpus.root)
    observation.update(
        {
            "arm": arm,
            "index_command": index_command,
            "environment_overrides": environment_extra,
            "index_wall_seconds": indexed["wall_seconds"],
            "index_cpu_seconds": indexed["cpu_seconds"],
            "index_peak_rss_bytes": indexed["peak_rss_bytes"],
            "resolve_wall_seconds": resolved["wall_seconds"],
            "header_counts": HSE95.parse_header_counts(
                indexed["stdout"] + indexed["stderr"]
            ),
            "profile": profile_counters(profile_path),
            "steps": [
                {key: value for key, value in step.items()
                 if key not in {"stdout", "stderr"}}
                for step in steps
            ],
        }
    )
    return observation


#: Forced re-extraction rounds a replay arm performs. The first round is a
#: settling round: the workspace identity that keys a cache slot includes the
#: index's own freshness and source fingerprint, so it changes when the cold
#: index completes and the entries written by the cold run are keyed under an
#: identity no later run observes. From the second round on the identity is
#: stable and a cache entry written by round one is found by round two.
REPLAY_ROUNDS = 2


def replay_arm(
    executable: Path,
    corpus: Any,
    source_arm_root: Path,
    *,
    arm: str,
    environment: Mapping[str, str] | None = None,
    rounds: int = REPLAY_ROUNDS,
) -> dict[str, Any]:
    """Force an existing database to re-extract its unchanged sources.

    This is how the translation-unit cache is reached end to end. Nothing in
    the CLI asks for re-extraction of unchanged input -- content-hash
    currentness means an untouched file is simply skipped -- so currentness is
    cleared directly, exactly as `production.recovery_probe` does, which is
    also the state a killed indexer leaves behind.

    A replay arm is only ever compared against another replay arm that went
    through the identical number of rounds. Forced re-registration of an
    already-indexed owned header re-emits its namespace `contains` edge, and
    that edge kind accumulates its count by design, so a replayed database is
    legitimately not identical to a cold one. Holding the history constant on
    both sides isolates the cache, which is what the axis is for.
    """
    if rounds < 1:
        raise MatrixError(f"{arm}: a replay arm needs at least one round")
    cache = source_arm_root / "cache"
    database = cache / "index.db"
    if not database.is_file():
        raise MatrixError(f"{arm}: no database to replay at {database}")

    arm_root = source_arm_root.parent / arm
    arm_root.mkdir(parents=True)
    logs = arm_root / "logs"
    rounds_recorded: list[dict[str, Any]] = []
    indexed: dict[str, Any] = {}
    profile_path = arm_root / "index.profile.json"
    for round_index in range(1, rounds + 1):
        with sqlite3.connect(database) as connection:
            connection.execute("UPDATE file SET indexed = 0")
        profile_path = arm_root / f"round-{round_index}.profile.json"
        indexed = _run(
            [str(executable), "index", "--jobs", "1",
             "--profile-json", str(profile_path)],
            cwd=corpus.root,
            environment=_environment(cache, environment),
            sink=logs,
            label=f"index-round-{round_index}",
        )
        if indexed["returncode"] != 0:
            raise MatrixError(
                f"{arm}: replay index round {round_index} failed\n"
                f"{indexed['stderr'][-2000:]}"
            )
        rounds_recorded.append(
            {
                "round": round_index,
                "wall_seconds": indexed["wall_seconds"],
                "cache": _cache_decisions({"profile": profile_counters(profile_path)}),
            }
        )

    resolved = _run(
        [str(executable), "resolve"],
        cwd=corpus.root,
        environment=_environment(cache, environment),
        sink=logs,
        label="resolve",
    )
    if resolved["returncode"] != 0:
        raise MatrixError(f"{arm}: replay resolve failed\n{resolved['stderr'][-2000:]}")

    observation = observe(cache, corpus.root)
    observation.update(
        {
            "arm": arm,
            "index_command": indexed["command"],
            "environment_overrides": dict(environment or {}),
            "replayed_from": str(source_arm_root),
            "replay_rounds": rounds_recorded,
            "index_wall_seconds": indexed["wall_seconds"],
            "index_cpu_seconds": indexed["cpu_seconds"],
            "index_peak_rss_bytes": indexed["peak_rss_bytes"],
            "resolve_wall_seconds": resolved["wall_seconds"],
            "header_counts": HSE95.parse_header_counts(
                indexed["stdout"] + indexed["stderr"]
            ),
            "profile": profile_counters(profile_path),
        }
    )
    return observation


def equivalence_matrix(
    executable: Path,
    work_root: Path,
    *,
    files: int,
    shape: str,
    many_header_target: int,
    capabilities: Mapping[str, Any],
) -> dict[str, Any]:
    """Every declared-equivalent axis, on one shared corpus.

    Returns the raw arm observations plus one `slo.equivalence_verdict` per
    axis. An axis whose distinguishing evidence is missing -- a warm-cache arm
    that recorded no cache hit, for example -- is failed rather than reported
    as equivalent, because two arms that ran the same code path prove nothing.
    """
    supported = dict(capabilities.get("index_options") or {})
    corpus_root = work_root / "corpus"
    corpus = PRODUCTION.generate_corpus(
        corpus_root, files, shape, many_header_target, "forward"
    )
    # Explicit, per-axis artifact roots. The default root lives beside the
    # corpus, which every arm shares, so leaving it to the default would let
    # one arm's cache objects reach another's run.
    cache_root = work_root / "tu-fact-cache"

    axes: dict[str, list[dict[str, Any]]] = {}
    arms: dict[str, dict[str, Any]] = {}
    notes: list[str] = []
    failures: list[str] = []

    def record(name: str, **kwargs: Any) -> dict[str, Any]:
        observation = run_arm(executable, corpus, work_root / "arms" / name,
                              arm=name, **kwargs)
        arms[name] = observation
        return observation

    # --- worker topology: serial against parallel, and completion order -----
    topology_arms = ["serial"]
    record("serial", index_args=["--jobs", "1"],
           environment={"CIDX_TU_FACT_CACHE": "0"})
    if supported.get("--jobs"):
        for jobs in (2, 4):
            name = f"parallel-{jobs}"
            record(name, index_args=["--jobs", str(jobs)],
                   environment={"CIDX_TU_FACT_CACHE": "0"})
            topology_arms.append(name)
        record("automatic", environment={"CIDX_TU_FACT_CACHE": "0"})
        topology_arms.append("automatic")
    else:
        notes.append("executable does not advertise --jobs; topology axis is serial only")
        failures.append("worker-topology: --jobs is unavailable on this executable")
    axes["worker-topology"] = [arms[name] for name in topology_arms]

    # --- cache state, cold: the cache must not change what a cold run says --
    #
    # These arms pin `--jobs 1` on purpose. The cache decision lives in the
    # serial `TuFactCacheIndexer` wrapper, and the bounded parallel scheduler
    # bypasses it, so a cache arm left on the automatic policy would silently
    # measure the uncached path and "prove" equivalence by running the same
    # code twice. The parallel-with-cache-enabled arm records that interaction
    # explicitly instead of hiding it.
    disabled_environment = {"CIDX_TU_FACT_CACHE": "0"}
    cache_environment = {
        "CIDX_TU_FACT_CACHE": "1",
        "CIDX_TU_FACT_CACHE_ROOT": str(cache_root / "cold"),
    }
    record("cache-disabled", index_args=["--jobs", "1"],
           environment=disabled_environment)
    record("cache-cold", index_args=["--jobs", "1"],
           environment=cache_environment)
    cache_axis = [arms["cache-disabled"], arms["cache-cold"]]
    if supported.get("--jobs"):
        record("cache-enabled-parallel", index_args=["--jobs", "4"],
               environment={
                   "CIDX_TU_FACT_CACHE": "1",
                   "CIDX_TU_FACT_CACHE_ROOT": str(cache_root / "parallel"),
               })
        cache_axis.append(arms["cache-enabled-parallel"])
    axes["cache-state"] = cache_axis

    # --- cache state, replayed: cached against fresh, same history ----------
    #
    # Both arms perform the identical forced re-extraction rounds; only the
    # cache differs. Comparing a replayed cached arm against a *cold* fresh
    # arm would compare index history, not the cache.
    arms["replay-fresh"] = replay_arm(
        executable, corpus, work_root / "arms" / "cache-disabled",
        arm="replay-fresh", environment=disabled_environment,
    )
    arms["replay-cached"] = replay_arm(
        executable, corpus, work_root / "arms" / "cache-cold",
        arm="replay-cached", environment=cache_environment,
    )
    axes["cache-replay"] = [arms["replay-fresh"], arms["replay-cached"]]

    cold_replays = _counter(arms["cache-cold"], "tu_fact_cache.parser_calls_avoided")
    fresh_replays = _counter(arms["replay-fresh"], "tu_fact_cache.parser_calls_avoided")
    warm_replays = _counter(arms["replay-cached"], "tu_fact_cache.parser_calls_avoided")
    if warm_replays <= 0:
        failures.append(
            "cache-replay: the cached arm avoided no parser call, so it did not "
            "exercise cache replay and its equivalence proves nothing"
        )
    if fresh_replays != 0:
        failures.append(
            f"cache-replay: the fresh arm avoided {fresh_replays} parser calls "
            "with the cache disabled"
        )
    if cold_replays != 0:
        failures.append(
            f"cache-state: the cold arm avoided {cold_replays} parser calls; it "
            "was expected to populate an empty cache"
        )

    # --- acceleration: shipped reuse identity against the explicit control --
    if supported.get("--no-front-end-reuse"):
        record("front-end-reuse-default", environment={"CIDX_TU_FACT_CACHE": "0"})
        record("front-end-reuse-disabled", index_args=["--no-front-end-reuse"],
               environment={"CIDX_TU_FACT_CACHE": "0"})
        axes["front-end-reuse"] = [arms["front-end-reuse-default"],
                                   arms["front-end-reuse-disabled"]]
    else:
        failures.append(
            "front-end-reuse: --no-front-end-reuse is unavailable on this executable"
        )

    # --- transforms: inline against deferred --------------------------------
    if supported.get("--defer-transforms"):
        record("transforms-inline", environment={"CIDX_TU_FACT_CACHE": "0"})
        record("transforms-deferred", defer_transforms=True,
               environment={"CIDX_TU_FACT_CACHE": "0"})
        axes["transform-mode"] = [arms["transforms-inline"],
                                  arms["transforms-deferred"]]
    else:
        failures.append(
            "transform-mode: --defer-transforms is unavailable on this executable"
        )

    # --- publication: in-place update against clean rebuild -----------------
    if supported.get("--clean"):
        record("publication-in-place", environment={"CIDX_TU_FACT_CACHE": "0"})
        record("publication-clean-rebuild", clean_rebuild=True, profile=False,
               environment={"CIDX_TU_FACT_CACHE": "0"})
        axes["publication-mode"] = [arms["publication-in-place"],
                                    arms["publication-clean-rebuild"]]
    else:
        failures.append(
            "publication-mode: --clean is unavailable on this executable"
        )

    verdicts = [
        SLO.equivalence_verdict(axis, members, reference=members[0]["arm"])
        for axis, members in axes.items()
    ]
    for verdict in verdicts:
        failures.extend(f"{verdict['axis']}: {reason}"
                        for reason in verdict["differences"])

    return {
        "corpus": {
            "root": str(corpus.root),
            "shape": shape,
            "files": files,
            "order": "forward",
            "target_distinct_owned_headers": corpus.target_distinct_owned_headers,
        },
        "arms": arms,
        "axes": {axis: [arm["arm"] for arm in members]
                 for axis, members in axes.items()},
        "verdicts": verdicts,
        "cache_evidence": {
            "replay_rounds": REPLAY_ROUNDS,
            "cold_arm_parser_calls_avoided": cold_replays,
            "fresh_replay_parser_calls_avoided": fresh_replays,
            "cached_replay_parser_calls_avoided": warm_replays,
            "cold_arm_decisions": _cache_decisions(arms["cache-cold"]),
            "cached_replay_decisions": _cache_decisions(arms["replay-cached"]),
            "cached_replay_rounds": arms["replay-cached"].get("replay_rounds"),
            "parallel_arm_decisions": (
                _cache_decisions(arms["cache-enabled-parallel"])
                if "cache-enabled-parallel" in arms
                else None
            ),
            "parallel_bypasses_cache": (
                _cache_decisions(arms["cache-enabled-parallel"]) == {}
                if "cache-enabled-parallel" in arms
                else None
            ),
        },
        "notes": notes,
        "failures": failures,
        "ok": not failures,
    }


def _counter(arm: Mapping[str, Any], name: str) -> float:
    counters = (arm.get("profile") or {}).get("counters") or {}
    value = counters.get(name, 0)
    return float(value) if isinstance(value, (int, float)) else 0.0


def _cache_decisions(arm: Mapping[str, Any]) -> dict[str, float]:
    counters = (arm.get("profile") or {}).get("counters") or {}
    return {
        name: float(value)
        for name, value in sorted(counters.items())
        if name.startswith("tu_fact_cache.") and isinstance(value, (int, float))
    }


# --- operating bounds -------------------------------------------------------


def bounds_matrix(
    executable: Path,
    work_root: Path,
    *,
    files: int,
    many_header_target: int,
    reference: Mapping[str, Any],
) -> dict[str, Any]:
    """Squeeze the documented operating limits and require the same facts.

    A bound that only holds at its default value is not a bound. Each setting
    here is deliberately far below what the corpus needs in one go, so the
    scheduler has to actually block, spill or serialise -- and the published
    facts still have to match the unconstrained reference arm.
    """
    corpus_root = work_root / "corpus"
    corpus = PRODUCTION.generate_corpus(
        corpus_root, files, "baseline", many_header_target, "forward"
    )
    settings = (
        {
            "name": "single-item-queue",
            "args": ["--jobs", "4", "--max-queue-items", "1"],
            "bound": "extracted-but-unpublished translation units",
        },
        {
            "name": "small-queue-bytes",
            "args": ["--jobs", "4", "--max-queue-bytes", "65536"],
            "bound": "extracted-but-unpublished payload bytes",
        },
        {
            "name": "small-memory-budget",
            "args": ["--memory-budget-bytes", str(64 * 1024 * 1024)],
            "bound": "resident-memory ceiling used to derive the worker count",
        },
    )

    control = run_arm(executable, corpus, work_root / "arms" / "unbounded",
                      arm="unbounded", index_args=["--jobs", "4"],
                      environment={"CIDX_TU_FACT_CACHE": "0"})
    arms = [control]
    failures: list[str] = []
    for setting in settings:
        observation = run_arm(
            executable, corpus, work_root / "arms" / setting["name"],
            arm=setting["name"], index_args=list(setting["args"]),
            environment={"CIDX_TU_FACT_CACHE": "0"},
        )
        observation["bound"] = setting["bound"]
        arms.append(observation)

    verdict = SLO.equivalence_verdict("operating-bounds", arms, reference="unbounded")
    failures.extend(verdict["differences"])

    peak_rss = {arm["arm"]: arm["index_peak_rss_bytes"] for arm in arms}
    return {
        "documented_limits": [dict(setting) for setting in settings],
        "arms": {arm["arm"]: arm for arm in arms},
        "verdict": verdict,
        "peak_rss_bytes": peak_rss,
        "reference_case": dict(reference),
        "failures": failures,
        "ok": not failures,
    }


# --- cited executable qualification -----------------------------------------


def qualification_tests(
    build_dir: Path, *, entries: Sequence[Mapping[str, Any]] = CITED_QUALIFICATION
) -> dict[str, Any]:
    """Run each cited ctest test and record its result.

    Citing a test without running it is an assertion about the past. Running
    it here ties the integrated report to this build of this binary.
    """
    results: list[dict[str, Any]] = []
    failures: list[str] = []
    for entry in entries:
        name = str(entry["test"])
        started = time.monotonic()
        completed = subprocess.run(
            ["ctest", "--test-dir", str(build_dir), "-R", f"^{name}$",
             "--output-on-failure"],
            capture_output=True, text=True, check=False,
        )
        elapsed = time.monotonic() - started
        passed = completed.returncode == 0
        results.append(
            {
                **{key: value for key, value in entry.items()},
                "returncode": completed.returncode,
                "wall_seconds": elapsed,
                "passed": passed,
                "output_tail": (completed.stdout + completed.stderr)[-2000:]
                if not passed
                else "",
            }
        )
        if not passed:
            failures.append(f"{name} did not pass")
    return {"results": results, "failures": failures, "ok": not failures}


# --- pre-feature A/B --------------------------------------------------------


def semantic_delta(
    baseline: Mapping[str, Any], candidate: Mapping[str, Any]
) -> dict[str, Any]:
    """Characterise the candidate's fact set against the pre-feature one.

    PERF-002 is not fact-neutral against the commit it started from: resolving
    cross-translation-unit identity at publication rather than from a pre-run
    snapshot recovers `uses` edges into header-owned namespaces that the
    pre-feature binary dropped. That is a superset, and the contract is that
    it stays a superset -- any section the candidate *shrinks* is a regression
    and fails, while a section it grows is reported with its size.
    """
    baseline_counts = dict(baseline.get("canonical_row_counts") or {})
    candidate_counts = dict(candidate.get("canonical_row_counts") or {})
    sections = sorted(set(baseline_counts) | set(candidate_counts))
    grew: dict[str, dict[str, int]] = {}
    shrank: dict[str, dict[str, int]] = {}
    for section in sections:
        before = int(baseline_counts.get(section, 0))
        after = int(candidate_counts.get(section, 0))
        if after > before:
            grew[section] = {"baseline": before, "candidate": after,
                             "delta": after - before}
        elif after < before:
            shrank[section] = {"baseline": before, "candidate": after,
                               "delta": after - before}
    failures = [
        f"canonical section {section} shrank from {values['baseline']} to "
        f"{values['candidate']}"
        for section, values in shrank.items()
    ]
    return {
        "identical": not grew and not shrank,
        "grew": grew,
        "shrank": shrank,
        "superset": not shrank,
        "failures": failures,
        "ok": not failures,
    }


def pre_feature_ab(
    candidate: Path,
    baseline: Path,
    work_root: Path,
    *,
    files: int,
    shape: str,
    many_header_target: int,
    order: str,
    trials: int,
) -> dict[str, Any]:
    """Paired synthetic runs of the pre-feature and candidate executables.

    The baseline executable predates the telemetry flags, so its arm runs with
    profiling off. Only the timings are compared as an A/B; the semantic
    difference is characterised by `semantic_delta` instead of being asserted
    equal.
    """
    baseline_trials: list[dict[str, Any]] = []
    candidate_trials: list[dict[str, Any]] = []
    for trial in range(1, trials + 1):
        # The pre-feature executable predates the current schema by
        # construction, so its databases are stamped with the predecessor
        # version. Refusing them would make the comparison unmeasurable rather
        # than catch drift; the arm that knows it is running such a binary says
        # which version it expects, and the observed version is recorded in
        # every snapshot either way.
        for label, executable, sink, profile, schema in (
            ("baseline", baseline, baseline_trials, False,
             HSE95.PREDECESSOR_SCHEMA_VERSION),
            ("candidate", candidate, candidate_trials, True,
             HSE95.EXPECTED_SCHEMA_VERSION),
        ):
            case_root = work_root / label / f"trial-{trial}"
            case_root.mkdir(parents=True)
            sink.append(
                PRODUCTION.run_synthetic_case(
                    executable, files, shape, many_header_target, order,
                    case_root, profile=profile, require_writer_metrics=False,
                    expected_schema_version=schema,
                )
            )

    baseline_aggregate = PRODUCTION.aggregate_trials(baseline_trials)
    candidate_aggregate = PRODUCTION.aggregate_trials(candidate_trials)

    stages = sorted(
        set(baseline_aggregate["stages"]) & set(candidate_aggregate["stages"])
    )
    timing = {
        stage: {
            "baseline_wall_seconds_trials":
                baseline_aggregate["stages"][stage]["wall_seconds_trials"],
            "candidate_wall_seconds_trials":
                candidate_aggregate["stages"][stage]["wall_seconds_trials"],
            "baseline_wall_seconds_median":
                baseline_aggregate["stages"][stage]["wall_seconds"],
            "candidate_wall_seconds_median":
                candidate_aggregate["stages"][stage]["wall_seconds"],
            "speedup": SLO.speedup(
                baseline_aggregate["stages"][stage]["wall_seconds_trials"],
                candidate_aggregate["stages"][stage]["wall_seconds_trials"],
            ),
            "baseline_peak_rss_bytes_median":
                baseline_aggregate["stages"][stage]["peak_rss_bytes"],
            "candidate_peak_rss_bytes_median":
                candidate_aggregate["stages"][stage]["peak_rss_bytes"],
        }
        for stage in stages
    }

    baseline_final = _final_snapshot(baseline_trials[-1])
    candidate_final = _final_snapshot(candidate_trials[-1])
    delta = semantic_delta(baseline_final, candidate_final)

    failures = list(delta["failures"])
    failures.extend(
        f"baseline: {failure}" for failure in baseline_aggregate["parity_failures"]
    )
    failures.extend(
        f"candidate: {failure}" for failure in candidate_aggregate["parity_failures"]
    )
    return {
        "case": f"{shape}:{files}:{order}",
        "trials": trials,
        "schema_versions": {
            "baseline": HSE95.PREDECESSOR_SCHEMA_VERSION,
            "candidate": HSE95.EXPECTED_SCHEMA_VERSION,
        },
        "baseline": baseline_aggregate,
        "candidate": candidate_aggregate,
        "timing": timing,
        "semantic_delta": delta,
        "failures": failures,
        "ok": not failures,
    }


def _final_snapshot(case: Mapping[str, Any]) -> dict[str, Any]:
    """The database snapshot after the last stage of one synthetic case."""
    stages = list(case.get("stages") or [])
    if not stages:
        raise MatrixError("synthetic case has no stages")
    return dict(stages[-1]["sqlite"]["snapshot"])


# --- report assembly --------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cidx", type=Path, required=True)
    parser.add_argument(
        "--pre-feature-cidx",
        type=Path,
        help="executable built from the commit before PERF-002 started",
    )
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--profile",
        choices=tuple(SCALE_PROFILES),
        default="quick",
        help=(
            "corpus sizes: 'quick' for an ordinary change, 'full' for the "
            "scheduled production-scale run. An explicit --*-files overrides "
            "the profile."
        ),
    )
    parser.add_argument("--equivalence-files", type=int)
    parser.add_argument("--equivalence-shape", default="header-heavy")
    parser.add_argument("--bounds-files", type=int)
    parser.add_argument("--pre-feature-files", type=int)
    parser.add_argument("--pre-feature-shape", default="baseline")
    parser.add_argument("--trials", type=int, default=SLO.MINIMUM_TRIALS)
    parser.add_argument(
        "--many-header-target", type=int,
        default=PRODUCTION.DEFAULT_MANY_HEADER_TARGET,
    )
    parser.add_argument("--skip-equivalence", action="store_true")
    parser.add_argument("--skip-bounds", action="store_true")
    parser.add_argument("--skip-qualification-tests", action="store_true")
    parser.add_argument(
        "--allow-contended-host", action="store_true",
        help="record non-authoritative smoke data on a busy host",
    )
    args = parser.parse_args()
    profile = SCALE_PROFILES[args.profile]
    for name, key in (
        ("equivalence_files", "equivalence_files"),
        ("bounds_files", "bounds_files"),
        ("pre_feature_files", "pre_feature_files"),
    ):
        if getattr(args, name) is None:
            setattr(args, name, profile[key])

    checkout = REPOSITORY_ROOT
    executable = args.cidx.resolve()
    if not executable.is_file():
        raise SystemExit(f"cidx executable not found: {executable}")
    if args.trials < SLO.MINIMUM_TRIALS:
        raise SystemExit(f"--trials must be at least {SLO.MINIMUM_TRIALS}")
    if platform.system() == "Darwin" and not args.work_root.name.endswith(".noindex"):
        raise SystemExit("macOS --work-root must end in .noindex")
    if args.work_root.exists():
        raise SystemExit(f"work root already exists: {args.work_root}")
    if args.output.exists():
        raise SystemExit(f"output already exists: {args.output}")
    if PRODUCTION._path_within(args.work_root, checkout) or PRODUCTION._path_within(
        args.output, checkout
    ):
        raise SystemExit("work root and output must remain outside the checkout")

    quiescence = PRODUCTION._host_quiescence()
    if not quiescence["quiescent"] and not args.allow_contended_host:
        raise SystemExit(
            "host is not quiescent; competing cidx indexers: "
            + "; ".join(quiescence["competing_cidx_indexers"])
        )

    args.work_root.mkdir(parents=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)

    capabilities = probe_capabilities(executable)
    report: dict[str, Any] = {
        "method": "S-078 integrated qualification",
        "authoritative_timing": quiescence["quiescent"],
        "host_quiescence": quiescence,
        "identity": PRODUCTION.environment_identity(executable, checkout, []),
        "capabilities": {"candidate": capabilities},
        "cited_qualification": [dict(entry) for entry in CITED_QUALIFICATION],
        "scale": {
            "profile": args.profile,
            "equivalence_files": args.equivalence_files,
            "bounds_files": args.bounds_files,
            "pre_feature_files": args.pre_feature_files,
            "trials": args.trials,
        },
        "failures": [],
    }

    if args.pre_feature_cidx is not None:
        report["capabilities"]["pre_feature"] = probe_capabilities(
            args.pre_feature_cidx.resolve()
        )

    if not args.skip_equivalence:
        report["equivalence"] = equivalence_matrix(
            executable, args.work_root / "equivalence",
            files=args.equivalence_files, shape=args.equivalence_shape,
            many_header_target=args.many_header_target,
            capabilities=capabilities,
        )
        report["failures"].extend(
            f"equivalence: {failure}"
            for failure in report["equivalence"]["failures"]
        )
    else:
        report["equivalence"] = {"status": "skipped", "ok": False,
                                 "failures": ["equivalence matrix skipped"]}
        report["failures"].append("equivalence: matrix skipped")

    if not args.skip_bounds:
        report["bounds"] = bounds_matrix(
            executable, args.work_root / "bounds",
            files=args.bounds_files,
            many_header_target=args.many_header_target,
            reference={"files": args.bounds_files, "shape": "baseline"},
        )
        report["failures"].extend(
            f"bounds: {failure}" for failure in report["bounds"]["failures"]
        )
    else:
        report["bounds"] = {"status": "skipped", "ok": False,
                            "failures": ["bounds matrix skipped"]}
        report["failures"].append("bounds: matrix skipped")

    if not args.skip_qualification_tests:
        report["qualification_tests"] = qualification_tests(args.build_dir.resolve())
        report["failures"].extend(
            f"qualification: {failure}"
            for failure in report["qualification_tests"]["failures"]
        )
    else:
        report["qualification_tests"] = {
            "status": "skipped", "ok": False,
            "failures": ["cited qualification tests skipped"],
        }
        report["failures"].append("qualification: cited tests skipped")

    if args.pre_feature_cidx is not None:
        report["pre_feature_ab"] = pre_feature_ab(
            executable, args.pre_feature_cidx.resolve(),
            args.work_root / "pre-feature",
            files=args.pre_feature_files, shape=args.pre_feature_shape,
            many_header_target=args.many_header_target, order="forward",
            trials=args.trials,
        )
        report["failures"].extend(
            f"pre-feature: {failure}"
            for failure in report["pre_feature_ab"]["failures"]
        )
    else:
        report["pre_feature_ab"] = {
            "status": "skipped", "ok": False,
            "failures": ["no --pre-feature-cidx executable was supplied"],
        }
        report["failures"].append("pre-feature: no baseline executable supplied")

    report["ok"] = not report["failures"]
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(args.output)
    return 1 if report["failures"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
