#!/usr/bin/env python3
"""Capture and summarize a reproducible HSE-95 Time Profiler run."""

from __future__ import annotations

import argparse
from collections import Counter
import html
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys


FRAME_RE = re.compile(r"<frame\b([^>]*)/?>")
ROW_RE = re.compile(r"<row\b.*?</row>", re.DOTALL)
BACKTRACE_RE = re.compile(
    r"<backtrace\b([^>]*)(?:>(.*?)</backtrace>|/>)", re.DOTALL
)
ATTRIBUTE_RE = re.compile(r"([A-Za-z_:][\w:.-]*)=\"([^\"]*)\"")
FRAME_LABELS = (
    "sqlite3Prepare",
    "sqlite3LockAndPrepare",
    "SqliteStmt::SqliteStmt",
    "StorageSymbolSink::emit",
    "TranslationUnitIndexer::run_symbol_pass",
    "SymbolVisitor::VisitNamedDecl",
)


def _load_benchmark() -> object:
    path = Path(__file__).with_name("run.py")
    spec = importlib.util.spec_from_file_location("hse95_benchmark", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load benchmark helper: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _attrs(text: str) -> dict[str, str]:
    return {key: html.unescape(value) for key, value in ATTRIBUTE_RE.findall(text)}


def _canonical_frame_label(name: str) -> str:
    name = name.split("(", 1)[0]
    for label in FRAME_LABELS:
        if name.endswith(label):
            return label
    return name


def inclusive_frame_counts(exported_xml: Path) -> dict[str, int]:
    """Count each frame appearance in every exported time-profile backtrace."""

    frame_names: dict[str, str] = {}
    backtraces: dict[str, list[str]] = {}
    counts: Counter[str] = Counter()
    content = exported_xml.read_text(encoding="utf-8")
    for row in ROW_RE.finditer(content):
        row_counts: list[str] = []
        for backtrace in BACKTRACE_RE.finditer(row.group(0)):
            attributes = _attrs(backtrace.group(1))
            body = backtrace.group(2)
            if body is None:
                row_counts.extend(backtraces.get(attributes.get("ref", ""), []))
                continue
            frames: list[str] = []
            for frame in FRAME_RE.finditer(body):
                frame_attributes = _attrs(frame.group(1))
                frame_id = frame_attributes.get("id")
                name = frame_attributes.get("name")
                if name is None:
                    name = frame_names.get(frame_attributes.get("ref", ""))
                if name is not None:
                    if frame_id:
                        frame_names[frame_id] = name
                    frames.append(_canonical_frame_label(name))
            if attributes.get("id"):
                backtraces[attributes["id"]] = frames
            row_counts.extend(frames)
        counts.update(row_counts)
    return dict(counts.most_common(25))


def run(command: list[str], *, env: dict[str, str]) -> None:
    print(shlex.join(command))
    subprocess.run(command, env=env, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cidx", type=Path, required=True)
    parser.add_argument("--label", choices=("baseline", "current"), required=True)
    parser.add_argument("--files", type=int, default=8)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--xml", type=Path, required=True)
    parser.add_argument("--summary", type=Path, required=True)
    args = parser.parse_args()
    if args.files < 1:
        raise SystemExit("--files must be positive")
    if args.work_root.exists():
        raise SystemExit(f"work root already exists: {args.work_root}")
    if args.trace.exists() or args.xml.exists() or args.summary.exists():
        raise SystemExit("trace, XML, and summary outputs must not already exist")

    benchmark = _load_benchmark()
    corpus = args.work_root / "corpus"
    compile_commands, _, _ = benchmark.generate_corpus(corpus, args.files)
    cache = args.work_root / "cache"
    cache.mkdir(parents=True)
    args.trace.parent.mkdir(parents=True, exist_ok=True)
    args.xml.parent.mkdir(parents=True, exist_ok=True)
    args.summary.parent.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ)
    environment["INDEXER_CACHE"] = str(cache)
    import_command = [
        str(args.cidx), "import", "--db", str(compile_commands), "--name", "hse95"
    ]
    index_command = [str(args.cidx), "index"]
    record_command = [
        "xcrun", "xctrace", "record", "--template", "Time Profiler",
        "--output", str(args.trace), "--launch", "--", *index_command,
    ]
    export_command = [
        "xcrun", "xctrace", "export", "--input", str(args.trace),
        "--xpath", "/trace-toc/run[@number=\"1\"]/data/table[@schema=\"time-profile\"]",
        "--output", str(args.xml),
    ]
    run(import_command, env=environment)
    run(record_command, env=environment)
    run(export_command, env=environment)
    summary = {
        "label": args.label,
        "files": args.files,
        "corpus": str(corpus),
        "compile_commands": str(compile_commands),
        "cache": str(cache),
        "trace": str(args.trace),
        "xml": str(args.xml),
        "import_command": import_command,
        "record_command": record_command,
        "export_command": export_command,
        "inclusive_frame_counts": inclusive_frame_counts(args.xml),
    }
    args.summary.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(args.summary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
