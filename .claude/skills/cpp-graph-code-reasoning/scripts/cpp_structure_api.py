#!/usr/bin/env python3
"""Thin CLI over the reusable cpp_agent_nav API.

Agents should import ``cpp_agent_nav`` for task-specific scripts. Keep this CLI
for smoke tests and simple manual navigation.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from cpp_agent_nav import (
    DEFAULT_BUDGET,
    DEFAULT_DB,
    DEFAULT_SESSION,
    DEFAULT_SNIPPET_CHARS,
    NavigationSession,
)


def dump_json(value: Any) -> None:
    print(json.dumps(value, indent=2, sort_keys=True, default=str))


def add_common(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--db", default=DEFAULT_DB, help="SQLite navigation DB")
    parser.add_argument("--session", default=DEFAULT_SESSION)
    parser.add_argument("--budget", type=int, default=DEFAULT_BUDGET)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Stateful C++ structure navigation CLI")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("init")
    add_common(p)
    p.add_argument("--repo-root")

    p = sub.add_parser("status")
    add_common(p)

    p = sub.add_parser("resolve")
    add_common(p)
    p.add_argument("--source", required=True)
    p.add_argument("--name", required=True)
    p.add_argument("--compile-commands")
    p.add_argument("--focus", action="store_true")

    p = sub.add_parser("snippet")
    add_common(p)
    p.add_argument("--source", required=True)
    p.add_argument("--name", required=True)
    p.add_argument("--compile-commands")
    p.add_argument("--char-budget", type=int, default=DEFAULT_SNIPPET_CHARS)

    p = sub.add_parser("indradb-lookup")
    add_common(p)
    p.add_argument("--uri", required=True)
    p.add_argument("--property", required=True)
    p.add_argument("--value", required=True)
    p.add_argument("--focus", action="store_true")

    p = sub.add_parser("lookahead")
    add_common(p)
    p.add_argument("--uri", required=True)
    p.add_argument("--node-key", default="current")
    p.add_argument("--direction", choices=["outbound", "inbound"], required=True)
    p.add_argument("--edge-kind")
    p.add_argument("--reason")

    p = sub.add_parser("frontier")
    add_common(p)
    p.add_argument("--state", default="open")

    p = sub.add_parser("step")
    add_common(p)
    p.add_argument("--frontier-id", type=int, required=True)
    p.add_argument("--checkpoint")

    p = sub.add_parser("checkpoint")
    add_common(p)
    p.add_argument("--label", required=True)
    p.add_argument("--note")

    p = sub.add_parser("backtrack")
    add_common(p)
    p.add_argument("--checkpoint-id", type=int, required=True)

    p = sub.add_parser("profile-frames")
    add_common(p)
    p.add_argument("--profile", required=True)
    p.add_argument("--project-root")

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        nav = NavigationSession(
            db_path=args.db,
            session=args.session,
            compile_commands=getattr(args, "compile_commands", None),
            default_budget=args.budget,
        )
        try:
            if args.command == "init":
                out = {"ok": True, "operation": "init", **nav.init(args.repo_root)}
            elif args.command == "status":
                out = {"ok": True, "operation": "status", **nav.status(args.budget)}
            elif args.command == "resolve":
                nodes = nav.resolve_symbol(args.source, args.name, focus=args.focus, budget=args.budget)
                out = {"ok": True, "operation": "resolve", "candidates": [n.brief() for n in nodes]}
            elif args.command == "snippet":
                out = {"ok": True, "operation": "snippet", **nav.snippet(args.source, args.name, args.char_budget)}
            elif args.command == "indradb-lookup":
                nodes = nav.graph_lookup(args.uri, args.property, args.value, focus=args.focus, budget=args.budget)
                out = {"ok": True, "operation": "indradb-lookup", "nodes": [n.brief() for n in nodes]}
            elif args.command == "lookahead":
                out = {
                    "ok": True,
                    "operation": "lookahead",
                    **nav.lookahead(args.uri, args.node_key, args.direction, args.edge_kind, args.reason, args.budget),
                }
            elif args.command == "frontier":
                out = {"ok": True, "operation": "frontier", "items": nav.frontier(args.state, args.budget)}
            elif args.command == "step":
                node = nav.step(args.frontier_id, args.checkpoint)
                out = {"ok": True, "operation": "step", "current": node.brief() if node else None, "snapshot": nav.status(args.budget)}
            elif args.command == "checkpoint":
                out = {"ok": True, "operation": "checkpoint", "checkpoint_id": nav.checkpoint(args.label, args.note), "snapshot": nav.status(args.budget)}
            elif args.command == "backtrack":
                out = {"ok": True, "operation": "backtrack", "restored": nav.backtrack(args.checkpoint_id), "snapshot": nav.status(args.budget)}
            elif args.command == "profile-frames":
                frames = nav.record_profile(args.profile, args.project_root, args.budget)
                out = {
                    "ok": True,
                    "operation": "profile-frames",
                    "frames": frames,
                    "project_owned_count": sum(1 for f in frames if f.get("project_owned")),
                }
            else:
                parser.error(f"unknown command {args.command}")
        finally:
            nav.close()
        dump_json(out)
        return 0
    except Exception as exc:
        dump_json({"ok": False, "operation": getattr(args, "command", None), "error": str(exc)})
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
