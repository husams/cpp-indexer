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

    def test_process_include_confinement_is_rejected(self) -> None:
        root, manifest = _fixture()
        (root / "src/extraction/pass.cpp").write_text(
            '#include <spawn.h>\n', encoding="utf-8"
        )
        errors = check_manifest(root, manifest)
        self.assertTrue(any("process include is outside module confinement" in error for error in errors))

    def test_cmake_target_edges_and_cycles_are_checked(self) -> None:
        root, manifest = _fixture()
        data = json.loads(manifest.read_text(encoding="utf-8"))
        data["modules"][1]["allowedDependencies"] = ["cli"]
        data["targets"].append({"name": "app", "module": "cli", "links": ["core"]})
        data["targets"][0]["links"] = ["model_lib", "app"]
        data["targetExceptions"] = [{
            "target": "app",
            "dependencies": ["core", "future-app"],
            "boundary": "test",
            "owner": "test",
            "rationale": "test",
            "expiresOn": "2099-01-01",
            "removalIssue": "HSE-58",
        }]
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
        self.assertTrue(any("not every listed dependency matches" in error for error in errors))

    def test_generator_expression_target_edges_are_checked(self) -> None:
        root, manifest = _fixture()
        data = json.loads(manifest.read_text(encoding="utf-8"))
        data["targets"].append({"name": "app", "module": "cli", "links": ["core"]})
        data["targets"][0]["links"] = ["model_lib", "$<$<BOOL:1>:app>"]
        manifest.write_text(json.dumps(data), encoding="utf-8")
        (root / "CMakeLists.txt").write_text(
            "add_library(core STATIC src/extraction/pass.cpp)\n"
            "target_link_libraries(core PRIVATE $<$<BOOL:1>:app> model_lib)\n"
            "add_library(app STATIC src/cli/format.hpp)\n"
            "target_link_libraries(app PRIVATE core)\n",
            encoding="utf-8",
        )
        errors = check_manifest(root, manifest)
        self.assertTrue(any("undeclared target dependency extraction -> cli" in error for error in errors))
        self.assertTrue(any("CMake target cycle" in error for error in errors))

    def test_unsupported_generator_expression_fails_closed(self) -> None:
        root, manifest = _fixture()
        data = json.loads(manifest.read_text(encoding="utf-8"))
        data["targets"][0]["links"] = ["model_lib", "$<TARGET_PROPERTY:unknown,NAME>"]
        manifest.write_text(json.dumps(data), encoding="utf-8")
        (root / "CMakeLists.txt").write_text(
            "add_library(core STATIC src/extraction/pass.cpp)\n"
            "target_link_libraries(core PRIVATE model_lib $<TARGET_PROPERTY:unknown,NAME>)\n",
            encoding="utf-8",
        )
        errors = check_manifest(root, manifest)
        self.assertTrue(any("unsupported CMake generator expression" in error for error in errors))

    def test_duplicate_target_ownership_is_rejected(self) -> None:
        root, manifest = _fixture()
        data = json.loads(manifest.read_text(encoding="utf-8"))
        data["targets"].append({"name": "core", "module": "model", "links": []})
        manifest.write_text(json.dumps(data), encoding="utf-8")
        errors = check_manifest(root, manifest)
        self.assertTrue(any("duplicate target ownership for core" in error for error in errors))

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

    def test_python_cross_module_and_external_imports_are_rejected(self) -> None:
        root, manifest = _fixture()
        (root / "python/indexer/legacy").mkdir(parents=True)
        (root / "python/indexer/sdk.py").parent.mkdir(parents=True, exist_ok=True)
        (root / "python/indexer/sdk.py").write_text(
            "import sqlite3\nfrom indexer.legacy import value\n", encoding="utf-8"
        )
        (root / "python/indexer/legacy/__init__.py").write_text("value = 1\n", encoding="utf-8")
        data = json.loads(manifest.read_text(encoding="utf-8"))
        data["sourceRoots"].append("python/indexer")
        data["modules"].extend([
            {"id": "sdk", "paths": ["python/indexer/**"], "excludePaths": ["python/indexer/legacy/**"], "sourceSuffixes": [".py"], "allowedDependencies": ["sdk"], "allowedExternal": []},
            {"id": "legacy", "paths": ["python/indexer/legacy/**"], "sourceSuffixes": [".py"], "allowedDependencies": ["legacy"], "allowedExternal": []},
        ])
        manifest.write_text(json.dumps(data), encoding="utf-8")
        errors = check_manifest(root, manifest)
        self.assertTrue(any("sqlite is outside module confinement" in error for error in errors))
        self.assertTrue(any("undeclared dependency sdk -> legacy" in error for error in errors))

    def test_rule_include_is_checked(self) -> None:
        root, manifest = _fixture()
        (root / "python/indexer/rules").mkdir(parents=True)
        (root / "python/indexer/rules/a.dl").write_text('.include "b.dl"\n', encoding="utf-8")
        (root / "python/indexer/rules/b.dl").write_text(".decl Edge(x:symbol)\n", encoding="utf-8")
        data = json.loads(manifest.read_text(encoding="utf-8"))
        data["sourceRoots"].append("python/indexer")
        data["modules"].extend([
            {"id": "rules-a", "paths": ["python/indexer/rules/a.dl"], "sourceSuffixes": [".dl"], "allowedDependencies": ["rules-a"]},
            {"id": "rules-b", "paths": ["python/indexer/rules/b.dl"], "sourceSuffixes": [".dl"], "allowedDependencies": ["rules-b"]},
        ])
        manifest.write_text(json.dumps(data), encoding="utf-8")
        errors = check_manifest(root, manifest)
        self.assertTrue(any("undeclared dependency rules-a -> rules-b" in error for error in errors))

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
