#!/usr/bin/env bash
#
# Manifest validation plus the one workflow regression that has actually
# bitten this repository. The former CODEOWNERS-row and doc-phrase assertions
# were removed: they proved only that particular lines of text still existed
# in .github/CODEOWNERS, POLICY.md and ASSURANCE.md, which is ceremony rather
# than verification, and they cost a CI failure every time a document was
# reworded.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
python3 "$ROOT/spec/tla/tools/validate-manifest.py" \
  "$ROOT/spec/tla/manifest.json"

python3 - "$ROOT" <<'PY'
import pathlib
import sys

root = pathlib.Path(sys.argv[1])

# cpp-default runs in an Ubuntu container as uid 0. Ubuntu's minimal image
# does not install sudo, so using it here prevents the job from reaching
# configure.
workflow = (root / ".github/workflows/verification.yml").read_text()
souffle_install = 'dpkg --install "$souffle_deb"'
if f"sudo {souffle_install}" in workflow:
    raise SystemExit("TLA_POLICY_STATUS=FAIL reason=root-container-uses-sudo")
if souffle_install not in workflow:
    raise SystemExit("TLA_POLICY_STATUS=FAIL reason=souffle-install-step-missing")
print("TLA_POLICY_STATUS=PASS")
PY
