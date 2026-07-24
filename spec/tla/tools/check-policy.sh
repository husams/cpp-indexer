#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
python3 - "$ROOT" <<'PY'
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
codeowners = (root / ".github/CODEOWNERS").read_text()
policy = (root / "spec/tla/POLICY.md").read_text()
required = (
    "/spec/tla/protected/ @husams",
    "/spec/tla/trusted/ @husams",
    "/spec/tla/modules/CidxTypes.tla @husams",
    "/spec/tla/models/*.cfg @husams",
)
missing = [entry for entry in required if entry not in codeowners]
if missing:
    raise SystemExit("TLA_POLICY_STATUS=FAIL reason=missing-codeowner:" + ",".join(missing))
for phrase in ("AI-generated implementation changes", "explicit human review"):
    if phrase not in policy:
        raise SystemExit("TLA_POLICY_STATUS=FAIL reason=missing-policy-language:" + phrase)
print("TLA_POLICY_STATUS=PASS protected-paths=4 human-review-required=true")
PY
