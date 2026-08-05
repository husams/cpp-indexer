#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
python3 "$ROOT/spec/tla/tools/validate-manifest.py" \
  "$ROOT/spec/tla/manifest.json"

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
    "/spec/tla/tools/check-policy.sh @husams",
    "/spec/tla/tools/select-changed-gates.sh @husams",
    "/spec/tla/tools/check-gate-selection.sh @husams",
    "/spec/tla/tools/check-gate-selection-defense-regression.sh @husams",
    "/spec/tla/tools/validate-manifest.py @husams",
    "/spec/tla/tools/check.sh @husams",
    "/spec/tla/tools/check-regression.sh @husams",
    "/spec/tla/tools/check-conformance.sh @husams",
    "/spec/tla/tools/check-sidecar-conformance.sh @husams",
    "/spec/tla/tools/check-proofs.sh @husams",
    "/spec/tla/tools/check-proofs-binding.sh @husams",
    "/spec/tla/tools/check-proofs-binding-unit-test.sh @husams",
    "/spec/tla/tools/export-counterexample.sh @husams",
    "/spec/tla/tools/check-verification-tamper-regression.sh @husams",
    "/spec/tla/tools/check-self-test-tamper-regression.sh @husams",
    "/spec/tla/tools/extract-trusted-checker.sh @husams",
    "/spec/tla/ci-dependency-map.json @husams",
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

# cpp-default runs in an Ubuntu container as uid 0. Ubuntu's minimal image
# does not install sudo, so using it here prevents the job from reaching
# configure. Keep a local policy regression for the exact pinned package
# installation command even while GitHub Actions is disabled.
workflow = (root / ".github/workflows/verification.yml").read_text()
souffle_install = 'dpkg --install "$souffle_deb"'
if f"sudo {souffle_install}" in workflow:
    raise SystemExit("TLA_POLICY_STATUS=FAIL reason=root-container-uses-sudo")
if souffle_install not in workflow:
    raise SystemExit("TLA_POLICY_STATUS=FAIL reason=souffle-install-step-missing")
print(f"TLA_POLICY_STATUS=PASS protected-paths={len(required)} human-review-required=true")
PY
