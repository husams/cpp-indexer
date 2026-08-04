#!/usr/bin/env python3
"""Keep raw SQLite access inside the declared persistence/read adapters."""

from __future__ import annotations

import re
import sys
from pathlib import Path


RAW_DB = re.compile(r"\braw_db\s*\(")
ALLOWED_FILES = {
    Path("src/storage/storage.hpp"): "persistence-service-api",
    Path("src/storage/storage_query.cpp"): "persistence-service",
    Path("src/storage/artifacts.cpp"): "persistence-service",
    Path("src/storage/fact_batch_writer.cpp"): "persistence-service",
    Path("src/query/exec.hpp"): "query-read-adapter-api",
    Path("src/query/exec.cpp"): "query-read-adapter",
    Path("src/graph/query.cpp"): "graph-read-adapter",
    Path("src/cli/commands_graph.cpp"): "graph-command-adapter",
}


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    violations: list[str] = []
    source_files = [*(root / "src").rglob("*.cpp"), *(root / "src").rglob("*.hpp")]
    for path in sorted(source_files):
        if not RAW_DB.search(path.read_text(encoding="utf-8")):
            continue
        relative = path.relative_to(root)
        if relative not in ALLOWED_FILES:
            violations.append(relative.as_posix())
    if violations:
        print("RAW_DB_BOUNDARY_STATUS=FAIL files=" + ",".join(violations))
        return 1
    print(
        "RAW_DB_BOUNDARY_STATUS=PASS "
        f"allowlist={len(ALLOWED_FILES)} declared-files"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
