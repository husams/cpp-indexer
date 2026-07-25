#!/usr/bin/env bash
#
# Changed-spec/changed-code dependency selection (HSE-89). Given a base and
# head git ref, decide which CI gates in ../ci-dependency-map.json a change
# actually requires -- driven by an explicit, reviewable map of flow ->
# {spec paths, real C++ implementation paths} -> gates, not by naively
# assuming only files literally under spec/tla/ matter.
#
# usage: select-changed-gates.sh <base-ref> <head-ref>
#        select-changed-gates.sh --all
#
# Emits one `run_<gate>=true|false` line per gate in ci-dependency-map.json's
# allGates list, to $GITHUB_OUTPUT if set, otherwise stdout.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MAP="$ROOT/ci-dependency-map.json"

if [[ "${1:-}" == "--all" ]]; then
  MODE="all"
else
  [[ $# -eq 2 ]] || { echo "usage: $0 <base-ref> <head-ref> | --all" >&2; exit 2; }
  BASE_REF="$1"
  HEAD_REF="$2"
  MODE="diff"
fi

OUT="${GITHUB_OUTPUT:-/dev/stdout}"

if [[ "$MODE" == "all" ]]; then
  python3 - "$MAP" <<'PY' >>"$OUT"
import json
import sys

manifest = json.loads(open(sys.argv[1]).read())
for gate in manifest["allGates"]:
    print(f"run_{gate.replace('-', '_')}=true")
PY
  exit 0
fi

CHANGED_FILE="$(mktemp "${TMPDIR:-/tmp}/cidx-tla-changed.XXXXXX")"
trap 'rm -f "$CHANGED_FILE"' EXIT
git -C "$(cd "$ROOT/../.." && pwd)" diff --name-only "$BASE_REF" "$HEAD_REF" >"$CHANGED_FILE"

python3 - "$MAP" "$CHANGED_FILE" <<'PY' >>"$OUT"
import json
import sys

manifest_path, changed_path = sys.argv[1:3]
manifest = json.loads(open(manifest_path).read())
changed = [line.strip() for line in open(changed_path) if line.strip()]

required_gates = set()
for flow in manifest["flows"]:
    prefixes = flow["paths"]
    if any(path == prefix or path.startswith(prefix) for path in changed for prefix in prefixes):
        required_gates.update(flow["gates"])

for gate in manifest["allGates"]:
    value = "true" if gate in required_gates else "false"
    print(f"run_{gate.replace('-', '_')}={value}")

print(f"TLA_GATE_SELECTION_STATUS=PASS changed={len(changed)} required={','.join(sorted(required_gates)) or 'none'}", file=sys.stderr)
PY
