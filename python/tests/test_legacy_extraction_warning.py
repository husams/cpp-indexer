from __future__ import annotations

import pytest

try:
    from indexer.clang import LegacyPythonExtractionWarning
    from indexer.clang import ast as legacy_ast
except Exception as error:  # pragma: no cover - depends on host libclang
    pytest.skip(f"legacy libclang adapter unavailable: {error}", allow_module_level=True)


def test_legacy_python_extraction_warns(monkeypatch):
    sentinel = object()
    monkeypatch.setattr(legacy_ast, "parse", lambda *args, **kwargs: sentinel)
    monkeypatch.setattr(legacy_ast, "collect_diagnostics", lambda *args: [])
    monkeypatch.setattr(legacy_ast, "index_symbols", lambda *args: (1, 2))
    monkeypatch.setattr(legacy_ast, "index_headers", lambda *args, **kwargs: 3)
    monkeypatch.setattr(legacy_ast, "index_edges", lambda *args, **kwargs: None)

    with pytest.warns(LegacyPythonExtractionWarning, match="C\\+\\+23 LibTooling"):
        result = legacy_ast.index_source(None, "fixture.cpp", [], 1)

    assert result == {
        "symbols": 1,
        "skipped": 2,
        "headers": 3,
        "diagnostics": [],
    }
