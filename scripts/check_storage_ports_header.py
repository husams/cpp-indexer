#!/usr/bin/env python3
"""Prove that the focused storage-port header is SQLite-independent."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


SQLITE_INCLUDE = re.compile(r"^\s*#\s*include\s*[\"<].*(?:sqlite\.hpp|sqlite3\.h)")


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    header = root / "src/storage/ports.hpp"
    text = header.read_text(encoding="utf-8")
    if SQLITE_INCLUDE.search(text):
        print("STORAGE_PORTS_HEADER_STATUS=FAIL direct-sqlite-include")
        return 1

    configured_compiler = os.environ.get("CXX")
    compiler = configured_compiler or shutil.which("clang++")
    if compiler is not None:
        compiler = shutil.which(compiler) or (
            compiler if Path(compiler).is_file() else None
        )
    if compiler is None:
        print("STORAGE_PORTS_HEADER_STATUS=FAIL clang++-unavailable")
        return 1

    with tempfile.TemporaryDirectory(prefix="cidx-storage-ports-") as directory:
        probe = Path(directory) / "probe.cpp"
        probe.write_text('#include "storage/ports.hpp"\nint main() {}\n', encoding="utf-8")
        result = subprocess.run(
            [
                compiler,
                "-std=c++23",
                "-I",
                str(root / "src"),
                "-H",
                "-fsyntax-only",
                str(probe),
            ],
            cwd=root,
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            print("STORAGE_PORTS_HEADER_STATUS=FAIL compile-contract")
            sys.stderr.write(result.stderr)
            return 1
        included = result.stderr
        if re.search(r"(?:storage[/\\]sqlite\.hpp|sqlite3\.h)", included):
            print("STORAGE_PORTS_HEADER_STATUS=FAIL transitive-sqlite-include")
            sys.stderr.write(included)
            return 1

    print("STORAGE_PORTS_HEADER_STATUS=PASS sqlite-free-preprocessor-contract")
    return 0


if __name__ == "__main__":
    sys.exit(main())
