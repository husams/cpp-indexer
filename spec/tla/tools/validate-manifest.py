#!/usr/bin/env python3
"""Validate the checked-in TLA+ contract manifest without third-party packages."""

from __future__ import annotations

import json
import pathlib
import re
import sys
from typing import NoReturn


def fail(reason: str) -> NoReturn:
    print(f"TLA_MANIFEST_STATUS=FAIL reason={reason}", file=sys.stderr)
    raise SystemExit(1)


def require_object(value: object, field: str) -> dict[str, object]:
    if not isinstance(value, dict):
        fail(f"{field}-must-be-object")
    return value


def require_string(value: object, field: str) -> str:
    if not isinstance(value, str) or not value.strip():
        fail(f"{field}-must-be-nonempty-string")
    return value


def require_string_list(
    value: object, field: str, *, allow_empty: bool = False
) -> list[str]:
    if not isinstance(value, list) or (not value and not allow_empty):
        fail(
            f"{field}-must-be-{'string-list' if allow_empty else 'nonempty-string-list'}"
        )
    result: list[str] = []
    for index, item in enumerate(value):
        result.append(require_string(item, f"{field}[{index}]"))
    if len(result) != len(set(result)):
        fail(f"{field}-must-not-contain-duplicates")
    return result


def required(mapping: dict[str, object], field: str, parent: str) -> object:
    if field not in mapping:
        fail(f"{parent}.{field}-is-required")
    return mapping[field]


def checked_model_names(check_script: pathlib.Path) -> list[str]:
    try:
        script = check_script.read_text(encoding="utf-8")
    except OSError as error:
        fail(f"check-script-unreadable:{error.__class__.__name__}")
    match = re.search(r"for model in \$\{TLA_MODELS:-([^}]+)\}; do", script)
    if match is None:
        fail("check-script-default-models-unreadable")
    names = match.group(1).split()
    if not names or len(names) != len(set(names)):
        fail("check-script-default-models-invalid")
    return names


def config_entries(config: pathlib.Path, directive: str) -> list[str]:
    try:
        lines = config.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        fail(f"model-config-unreadable:{config.name}:{error.__class__.__name__}")
    return [
        parts[1]
        for line in lines
        if len(parts := line.split()) == 2 and parts[0] == directive
    ]


def validate_model_inventory(
    manifest_path: pathlib.Path,
    models: list[tuple[str, str, list[str], list[str]]],
) -> None:
    root = manifest_path.parent
    checked_names = checked_model_names(root / "tools/check.sh")
    manifest_names: list[str] = []
    for index, (module, config, invariants, properties) in enumerate(models):
        name = pathlib.PurePosixPath(module).stem
        if module != f"models/{name}.tla":
            fail(f"models[{index}].module-must-name-a-model-tla-file")
        if config != f"models/{name}.cfg":
            fail(f"models[{index}].config-must-match-module")
        manifest_names.append(name)

        config_path = root / config
        checked_invariants = config_entries(config_path, "INVARIANT")
        checked_properties = config_entries(config_path, "PROPERTY")
        if invariants != checked_invariants:
            fail(f"models[{index}].invariants-must-match-config")
        if properties != checked_properties:
            fail(f"models[{index}].properties-must-match-config")

    if manifest_names != checked_names:
        fail(
            "models-inventory-mismatch:"
            f"manifest={','.join(manifest_names)}:"
            f"check={','.join(checked_names)}"
        )


def validate_manifest(manifest: object, manifest_path: pathlib.Path) -> None:
    root = require_object(manifest, "manifest")

    version = require_string(required(root, "specVersion", "manifest"), "specVersion")
    if re.fullmatch(r"\d+\.\d+\.\d+", version) is None:
        fail("specVersion-must-be-semver")
    require_string(required(root, "normativeBoundary", "manifest"), "normativeBoundary")
    require_string_list(required(root, "modules", "manifest"), "modules")
    require_string(required(root, "assurance", "manifest"), "assurance")
    require_string_list(required(root, "protectedPaths", "manifest"), "protectedPaths")

    proofs = required(root, "proofs", "manifest")
    if not isinstance(proofs, list) or not proofs:
        fail("proofs-must-be-nonempty-object-list")
    for index, value in enumerate(proofs):
        proof = require_object(value, f"proofs[{index}]")
        for field in ("module", "extends", "theorem", "checker"):
            require_string(
                required(proof, field, f"proofs[{index}]"),
                f"proofs[{index}].{field}",
            )
        require_string_list(
            required(proof, "provesInvariants", f"proofs[{index}]"),
            f"proofs[{index}].provesInvariants",
        )
        # HSE-89 acceptance-review fix: check-proofs-binding.sh rejects any
        # ASSUME in the proof module that is not listed here, verbatim, so
        # this allowlist must be an explicit, reviewable part of the
        # manifest -- present (even if empty, for a proof module with no
        # free constants) rather than silently defaulted.
        require_string_list(
            required(proof, "trustedAssumptions", f"proofs[{index}]"),
            f"proofs[{index}].trustedAssumptions",
            allow_empty=True,
        )

    models = required(root, "models", "manifest")
    if not isinstance(models, list) or not models:
        fail("models-must-be-nonempty-object-list")
    validated_models: list[tuple[str, str, list[str], list[str]]] = []
    for index, value in enumerate(models):
        model = require_object(value, f"models[{index}]")
        module = require_string(
            required(model, "module", f"models[{index}]"),
            f"models[{index}].module",
        )
        config = require_string(
            required(model, "config", f"models[{index}]"),
            f"models[{index}].config",
        )
        invariants = require_string_list(
            required(model, "invariants", f"models[{index}]"),
            f"models[{index}].invariants",
        )
        properties = require_string_list(
            required(model, "properties", f"models[{index}]"),
            f"models[{index}].properties",
            allow_empty=True,
        )
        validated_models.append((module, config, invariants, properties))
    validate_model_inventory(manifest_path, validated_models)

    coverage = require_object(required(root, "coverage", "manifest"), "coverage")
    if not coverage:
        fail("coverage-must-not-be-empty")
    for name, value in coverage.items():
        require_string(name, "coverage-key")
        entry = require_object(value, f"coverage.{name}")
        require_string_list(
            required(entry, "modules", f"coverage.{name}"),
            f"coverage.{name}.modules",
        )
        require_string_list(
            required(entry, "invariants", f"coverage.{name}"),
            f"coverage.{name}.invariants",
        )
        if "coverage" in entry:
            require_string(entry["coverage"], f"coverage.{name}.coverage")
        if "notModeled" in entry:
            require_string_list(
                entry["notModeled"],
                f"coverage.{name}.notModeled",
                allow_empty=True,
            )

    conformance = require_object(
        required(root, "conformance", "manifest"), "conformance"
    )
    for field in (
        "operations",
        "observations",
        "scenarios",
        "sidecarOperations",
        "sidecarObservations",
        "sidecarScenarios",
        "sidecarChecker",
    ):
        require_string(
            required(conformance, field, "conformance"),
            f"conformance.{field}",
        )

    counterexample = require_object(
        required(root, "counterexampleExport", "manifest"),
        "counterexampleExport",
    )
    for field in ("tool", "golden"):
        require_string(
            required(counterexample, field, "counterexampleExport"),
            f"counterexampleExport.{field}",
        )
    seeds = require_object(
        required(
            counterexample,
            "cppRegressionSeededFrom",
            "counterexampleExport",
        ),
        "counterexampleExport.cppRegressionSeededFrom",
    )
    if not seeds:
        fail("counterexampleExport.cppRegressionSeededFrom-must-not-be-empty")
    for source, target in seeds.items():
        require_string(source, "counterexampleExport.cppRegressionSeededFrom-key")
        require_string(
            target,
            f"counterexampleExport.cppRegressionSeededFrom.{source}",
        )


def main() -> None:
    if len(sys.argv) != 2:
        fail("usage-validate-manifest.py-manifest-json")
    path = pathlib.Path(sys.argv[1])
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        fail(f"unreadable:{error.__class__.__name__}")
    except json.JSONDecodeError as error:
        fail(f"invalid-json:line-{error.lineno}-column-{error.colno}")
    validate_manifest(manifest, path)
    print("TLA_MANIFEST_STATUS=PASS")


if __name__ == "__main__":
    main()
