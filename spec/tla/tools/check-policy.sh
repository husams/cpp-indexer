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
    "/spec/tla/proofs/ @husams",
    "/spec/tla/counterexamples/golden/ @husams",
    "/.github/CODEOWNERS @husams",
    "/spec/tla/manifest.json @husams",
    "/spec/tla/tools/check-protected-review.sh @husams",
    "/spec/tla/tools/check-policy.sh @husams",
    "/spec/tla/tools/select-changed-gates.sh @husams",
    "/.github/workflows/verification.yml @husams",
)
missing = [entry for entry in required if entry not in codeowners]
if missing:
    raise SystemExit("TLA_POLICY_STATUS=FAIL reason=missing-codeowner:" + ",".join(missing))
for phrase in ("AI-generated implementation changes", "explicit human review"):
    if phrase not in policy:
        raise SystemExit("TLA_POLICY_STATUS=FAIL reason=missing-policy-language:" + phrase)
assurance = (root / "spec/tla/ASSURANCE.md").read_text()
for phrase in ("TLC", "TLAPS", "Conformance replay"):
    if phrase not in assurance:
        raise SystemExit("TLA_POLICY_STATUS=FAIL reason=missing-assurance-language:" + phrase)
print(f"TLA_POLICY_STATUS=PASS protected-paths={len(required)} human-review-required=true")
PY
