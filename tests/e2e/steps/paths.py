"""Filesystem anchors shared by the whole e2e suite."""

from __future__ import annotations

import sys
from pathlib import Path

E2E_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = E2E_DIR.parent.parent
FIXTURES_DIR = E2E_DIR / "fixtures"

# The query API lives in the python/ tree and is imported, not installed, so the
# suite runs against the working copy.
if str(REPO_ROOT / "python") not in sys.path:
    sys.path.insert(0, str(REPO_ROOT / "python"))
