# cidx Python API

This directory contains the canonical Python implementation and query API for
`cidx`. It is an independent Python project and can be installed directly:

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
