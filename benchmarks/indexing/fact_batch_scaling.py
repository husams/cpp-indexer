#!/usr/bin/env python3
"""Reject a measurable quadratic component in FactBatch emitter time."""

from __future__ import annotations

import argparse
import json
import statistics
import subprocess
from pathlib import Path


def solve(matrix: list[list[float]], values: list[float]) -> list[float]:
    size = len(values)
    augmented = [row[:] + [value] for row, value in zip(matrix, values)]
    for column in range(size):
        pivot = max(range(column, size), key=lambda row: abs(augmented[row][column]))
        augmented[column], augmented[pivot] = augmented[pivot], augmented[column]
        divisor = augmented[column][column]
        if abs(divisor) < 1e-12:
            return [0.0] * size
        augmented[column] = [value / divisor for value in augmented[column]]
        for row in range(size):
            if row == column:
                continue
            factor = augmented[row][column]
            augmented[row] = [
                current - factor * selected
                for current, selected in zip(augmented[row], augmented[column])
            ]
    return [augmented[row][-1] for row in range(size)]


def quadratic_fit(samples: list[tuple[int, float]]) -> tuple[float, float, float]:
    features = [(1.0, float(size), float(size * size)) for size, _ in samples]
    matrix = [
        [sum(row[i] * row[j] for row in features) for j in range(3)]
        for i in range(3)
    ]
    values = [
        sum(row[i] * elapsed for row, (_, elapsed) in zip(features, samples))
        for i in range(3)
    ]
    constant, linear, quadratic = solve(matrix, values)
    return constant, linear, quadratic


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--benchmark", required=True, type=Path)
    parser.add_argument("--sizes", default="1000,2000,4000,8000")
    parser.add_argument("--trials", type=int, default=7)
    parser.add_argument("--quadratic-tolerance", type=float, default=0.35)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if args.trials < 5:
        parser.error("--trials must be at least 5")
    sizes = [int(value) for value in args.sizes.split(",")]
    if len(sizes) < 4 or any(size <= 0 for size in sizes):
        parser.error("--sizes must contain at least four positive values")

    rows = []
    samples = []
    for size in sizes:
        trials = []
        canonical = []
        fingerprints: set[int] = set()
        for _ in range(args.trials):
            completed = subprocess.run(
                [str(args.benchmark), "--symbols", str(size)],
                check=True,
                capture_output=True,
                text=True,
            )
            measurement = json.loads(completed.stdout)
            trials.append(int(measurement["emission_ns"]))
            canonical.append(int(measurement["canonicalization_ns"]))
            fingerprints.add(int(measurement["canonical_fingerprint"]))
        if len(fingerprints) != 1:
            raise RuntimeError(
                f"canonical output changed across trials for {size} symbols"
            )
        emission_median = float(statistics.median(trials))
        canonical_median = float(statistics.median(canonical))
        samples.append((size, emission_median))
        rows.append(
            {
                "symbols": size,
                "trials": args.trials,
                "emission_median_ns": emission_median,
                "canonicalization_median_ns": canonical_median,
                "canonical_fingerprint": next(iter(fingerprints)),
            }
        )

    constant, linear, quadratic = quadratic_fit(samples)
    largest_size, largest_elapsed = samples[-1]
    quadratic_fraction = max(0.0, quadratic) * largest_size**2 / max(
        largest_elapsed, 1.0
    )
    passed = quadratic_fraction <= args.quadratic_tolerance
    report = {
        "schema": "cidx.fact-batch-scaling/v1",
        "measurements": rows,
        "fit": {
            "constant_ns": constant,
            "linear_ns_per_symbol": linear,
            "quadratic_ns_per_symbol2": quadratic,
            "quadratic_fraction_at_max": quadratic_fraction,
            "tolerance": args.quadratic_tolerance,
        },
        "passed": passed,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps(report, sort_keys=True))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
