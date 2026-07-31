"""Pure qualification contracts for the S-075 front-end reuse decision."""

from __future__ import annotations

import hashlib
import json
import statistics
from collections.abc import Iterable, Mapping
from typing import Any

IDENTITY_VERSION = "front-end-reuse/v1"
CANDIDATES = (
    "no-reuse",
    "generated-umbrella-pch",
    "precompiled-preamble-astunit",
)
REQUIRED_TRIAL_FIELDS = frozenset(
    {
        "wall_seconds",
        "cpu_seconds",
        "peak_rss_bytes",
        "clang_front_end_seconds",
        "disk_bytes",
    }
)


def none_identity(configuration: Mapping[str, Any]) -> dict[str, str]:
    canonical = json.dumps(
        {
            "version": IDENTITY_VERSION,
            "mechanism": "none",
            "configuration": configuration,
            "prefixes": [],
        },
        sort_keys=True,
        separators=(",", ":"),
    )
    return {
        "version": IDENTITY_VERSION,
        "mechanism": "none",
        "canonical_bytes": canonical,
        "sha256": "sha256:" + hashlib.sha256(canonical.encode()).hexdigest(),
    }


def retain_trials(trials: Iterable[Mapping[str, Any]]) -> dict[str, Any]:
    retained = [dict(trial) for trial in trials]
    if len(retained) < 3:
        raise ValueError("front-end reuse qualification requires at least three trials")
    missing = [sorted(REQUIRED_TRIAL_FIELDS.difference(trial)) for trial in retained]
    if any(missing):
        raise ValueError(f"front-end reuse trial is incomplete: {missing}")
    return {
        "trial_count": len(retained),
        "trials": retained,
        "medians": {
            field: statistics.median(trial[field] for trial in retained)
            for field in sorted(REQUIRED_TRIAL_FIELDS)
        },
    }


def qualification_contract(trials: int = 3, *, disabled: bool = False) -> dict[str, Any]:
    if trials < 3:
        raise ValueError("front-end reuse qualification requires at least three trials")
    return {
        "identity_version": IDENTITY_VERSION,
        "mechanism": "none",
        "explicitly_disabled": disabled,
        "candidates": [
            {
                "name": candidate,
                "decision": "control" if candidate == "no-reuse" else "reject",
                "requires_adr": candidate != "no-reuse",
            }
            for candidate in CANDIDATES
        ],
        "required_trial_fields": sorted(REQUIRED_TRIAL_FIELDS),
        "trial_count_minimum": trials,
        "parity": ["semantic", "diagnostics", "catalog", "integrity"],
        "artifact_construction": False,
        "artifact_injection": False,
    }
