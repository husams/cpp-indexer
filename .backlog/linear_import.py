#!/usr/bin/env python3
"""Compatibility shim: forwards to the `backlog` skill's built-in Linear sync.

The importer that used to live here has moved into the skill, where it grew a
push direction, a link table and conflict detection. This wrapper keeps the old
invocation working:

    python3 .backlog/linear_import.py --vault-path secret/api-keys/linear \
        --team HSE --project cidx --actor linear-sync

The command it now runs is:

    backlog linear pull --vault-path ... --team HSE --project cidx \
        --actor linear-sync

Prefer calling the skill directly; it also offers `linear status` (read-only
diff), `linear push --apply` (local -> Linear) and `linear sync`. See
~/.claude/skills/backlog/references/linear.md
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

def _find_backlog_bin() -> Path:
    """The binary that implements the Linear sync.

    The backlog plugin removed its Linear integration on purpose (commit
    33f0f92), so the sync now lives in the standalone `backlog-linear` tool
    beside it. $BACKLOG_LINEAR_BIN overrides.
    """
    env = os.environ.get("BACKLOG_LINEAR_BIN")
    candidates = [Path(env)] if env else []
    candidates.append(Path.home() / ".local" / "bin" / "backlog-linear")
    which = shutil.which("backlog-linear")
    if which:
        candidates.append(Path(which))
    for cand in candidates:
        if cand.exists():
            return cand
    sys.exit(
        "no Linear-capable backlog tool found.\n"
        "  expected ~/.local/bin/backlog-linear (the plugin build has no `linear` command)"
    )


BACKLOG_BIN = _find_backlog_bin()

# old flag -> new flag. `--project` used to mean the Linear project; the skill
# now uses `--project` for the *backlog* project and `--linear-project` for the
# Linear one, so it is renamed on the way through.
PASSTHROUGH = {
    "--vault-path", "--vault-field", "--token-file", "--from-json",
    "--team", "--actor", "--save-fetch",
}
RENAME = {"--project": "--linear-project"}


def main(argv: list[str]) -> int:
    if not BACKLOG_BIN.exists():
        sys.exit(f"backlog skill not found at {BACKLOG_BIN}")

    args, subcommand = [], "pull"
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--dry-run":
            # `status` is the read-only equivalent: it reports what differs in
            # both directions and writes to neither side.
            subcommand = "status"
        elif arg == "--reset":
            sys.exit(
                "--reset is not forwarded: it wipes the store, and the sync no "
                "longer needs it (identity lives in the link table, so a re-pull "
                "is idempotent).\nIf you really want to start over:\n"
                f"  rm {Path.cwd() / '.backlog' / 'backlog.db'} && {BACKLOG_BIN} init ."
            )
        elif arg == "--backlog-bin":
            i += 2
            continue
        elif arg.split("=", 1)[0] in PASSTHROUGH or arg.split("=", 1)[0] in RENAME:
            head, _, tail = arg.partition("=")
            args.append(RENAME.get(head, head) + (f"={tail}" if tail else ""))
            if not tail and i + 1 < len(argv):
                i += 1
                args.append(argv[i])
        else:
            sys.exit(f"unknown flag {arg!r}; call `{BACKLOG_BIN} linear --help` instead")
        i += 1

    print(
        f"note: linear_import.py forwards to `backlog-linear {subcommand}`; "
        "call it directly in future.",
        file=sys.stderr,
    )
    cmd = [str(BACKLOG_BIN), subcommand, *args]
    os.execv(shutil.which(str(BACKLOG_BIN)) or str(BACKLOG_BIN), cmd)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
