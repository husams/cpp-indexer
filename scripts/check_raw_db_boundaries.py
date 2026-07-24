#!/usr/bin/env python3
"""Keep raw SQLite access inside persistence and query adapter boundaries."""

from __future__ import annotations

import re
import sys
from pathlib import Path


RAW_DB = re.compile(r"\braw_db\s*\(")
ALLOWED = (
    "src/storage/",
    "src/query/",
    "src/graph/",
    "src/cli/commands_graph.cpp",
)


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    violations: list[str] = []
    for path in sorted((root / "src").rglob("*.cpp")):
        relative = path.relative_to(root).as_posix()
        if not RAW_DB.search(path.read_text(encoding="utf-8")):
            continue
        if not relative.startswith(ALLOWED):
            violations.append(relative)
    if violations:
        print("RAW_DB_BOUNDARY_STATUS=FAIL files=" + ",".join(violations))
        return 1
    print("RAW_DB_BOUNDARY_STATUS=PASS allowed=persistence,query-adapters")
    return 0


if __name__ == "__main__":
    sys.exit(main())
