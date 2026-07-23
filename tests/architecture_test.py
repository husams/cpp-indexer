#!/usr/bin/env python3
"""Positive and mutation tests for the bootstrap architecture checker."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from scripts.check_architecture import check_manifest  # noqa: E402


def _fixture() -> tuple[Path, Path]:
    temp = Path(tempfile.mkdtemp(prefix="cidx-architecture-", dir="/tmp"))
    (temp / "src/model").mkdir(parents=True)
    (temp / "src/cli").mkdir(parents=True)
    (temp / "src/extraction").mkdir(parents=True)
    (temp / "src/model/value.hpp").write_text("#pragma once\n", encoding="utf-8")
    (temp / "src/cli/format.hpp").write_text("#pragma once\n", encoding="utf-8")
    (temp / "src/extraction/pass.cpp").write_text(
        '#include "model/value.hpp"\n', encoding="utf-8"
    )
    (temp / "CMakeLists.txt").write_text(
        "add_library(core STATIC src/extraction/pass.cpp)\n"
        "target_link_libraries(core PRIVATE model_lib)\n",
        encoding="utf-8",
    )
    manifest = {
        "sourceRoots": ["src"],
        "cmakeFiles": ["CMakeLists.txt"],
        "modules": [
            {"id": "model", "paths": ["src/model/**"], "allowedDependencies": ["model"]},
            {"id": "cli", "paths": ["src/cli/**"], "allowedDependencies": ["cli"]},
            {
                "id": "extraction",
                "paths": ["src/extraction/**"],
                "allowedDependencies": ["extraction", "model"],
            },
        ],
        "targets": [
            {"name": "core", "module": "extraction", "links": ["model_lib"]}
        ],
        "contracts": [],
        "exceptions": [],
    }
    manifest_path = temp / "manifest.json"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    return temp, manifest_path


class ArchitectureCheckerTests(unittest.TestCase):
    def test_clean_fixture_passes(self) -> None:
        root, manifest = _fixture()
        self.assertEqual(check_manifest(root, manifest), [])

    def test_forbidden_upward_include_is_rejected(self) -> None:
        root, manifest = _fixture()
        (root / "src/extraction/pass.cpp").write_text(
            '#include "../cli/format.hpp"\n', encoding="utf-8"
        )
        errors = check_manifest(root, manifest)
        self.assertTrue(any("undeclared dependency extraction -> cli" in error for error in errors))

    def test_module_wide_clang_include_confinement_is_rejected(self) -> None:
        root, manifest = _fixture()
        (root / "src/extraction/pass.cpp").write_text(
            '#include "clang/AST/Decl.h"\n', encoding="utf-8"
        )
        errors = check_manifest(root, manifest)
        self.assertTrue(any("clang include is outside module confinement" in error for error in errors))

    def test_cmake_target_edges_and_cycles_are_checked(self) -> None:
        root, manifest = _fixture()
        data = json.loads(manifest.read_text(encoding="utf-8"))
        data["modules"][1]["allowedDependencies"] = ["cli"]
        data["targets"].append({"name": "app", "module": "cli", "links": ["core"]})
        data["targets"][0]["links"] = ["model_lib", "app"]
        manifest.write_text(json.dumps(data), encoding="utf-8")
        (root / "CMakeLists.txt").write_text(
            "add_library(core STATIC src/extraction/pass.cpp)\n"
            "target_link_libraries(core PRIVATE model_lib app)\n"
            "add_library(app STATIC src/cli/format.hpp)\n"
            "target_link_libraries(app PRIVATE core)\n",
            encoding="utf-8",
        )
        errors = check_manifest(root, manifest)
        self.assertTrue(any("undeclared target dependency extraction -> cli" in error for error in errors))
        self.assertTrue(any("CMake target cycle" in error for error in errors))

    def test_empty_or_stale_exception_is_rejected(self) -> None:
        root, manifest = _fixture()
        data = json.loads(manifest.read_text(encoding="utf-8"))
        data["exceptions"] = [{
            "source": "src/extraction/pass.cpp",
            "fromModule": "extraction",
            "toModule": "model",
            "boundary": "test",
            "owner": "test",
            "rationale": "test",
            "expiresOn": "2099-01-01",
            "removalIssue": "HSE-58",
        }]
        manifest.write_text(json.dumps(data), encoding="utf-8")
        errors = check_manifest(root, manifest)
        self.assertTrue(any("does not match an actual violation" in error for error in errors))

    def test_python_and_souffle_sources_are_classified(self) -> None:
        root, manifest = _fixture()
        (root / "python/indexer/rules").mkdir(parents=True)
        (root / "python/indexer/query.py").parent.mkdir(parents=True, exist_ok=True)
        (root / "python/indexer/query.py").write_text("value = 1\n", encoding="utf-8")
        (root / "python/indexer/rules/callgraph.dl").write_text(".decl Edge(x:symbol)\n", encoding="utf-8")
        data = json.loads(manifest.read_text(encoding="utf-8"))
        data["sourceRoots"].append("python/indexer")
        data["modules"].extend([
            {"id": "sdk", "paths": ["python/indexer/**"], "sourceSuffixes": [".py"], "allowedDependencies": ["sdk"]},
            {"id": "rules", "paths": ["python/indexer/rules/**"], "sourceSuffixes": [".dl"], "allowedDependencies": ["rules"]},
        ])
        manifest.write_text(json.dumps(data), encoding="utf-8")
        self.assertEqual(check_manifest(root, manifest), [])

    def test_declared_cycle_is_rejected(self) -> None:
        root, manifest = _fixture()
        data = json.loads(manifest.read_text(encoding="utf-8"))
        data["modules"][0]["allowedDependencies"].append("extraction")
        manifest.write_text(json.dumps(data), encoding="utf-8")
        errors = check_manifest(root, manifest)
        self.assertTrue(any("declared dependency cycle" in error for error in errors))

    def test_unclassified_source_is_rejected(self) -> None:
        root, manifest = _fixture()
        (root / "src/unowned.hpp").write_text("#pragma once\n", encoding="utf-8")
        errors = check_manifest(root, manifest)
        self.assertTrue(any("source src/unowned.hpp" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
