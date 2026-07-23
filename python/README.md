# cidx Python API

This directory contains the supported Python storage/read-query SDK for `cidx`.
Production C/C++ extraction is owned by the C++23 LibTooling core under `src/`.
The legacy `indexer.clang` extractor is retained only as a deprecation-boundary
adapter and emits `LegacyPythonExtractionWarning`.

It is an independent Python project and can be installed directly:

```bash
python -m pip install ./python
```

The installation provides the `indexer` and `cidx-python` commands and the
top-level `indexer` import package.

For development and tests:

```bash
cd python
uv sync --dev
uv run pytest
```
