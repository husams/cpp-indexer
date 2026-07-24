# CIDX platform contracts

These documents define the modular platform boundary for the C++ core and
Python SDK.

- [Platform architecture and extension model](architecture.md) defines the
  layers, ports, artifact contract, extension surfaces, and delivery policy.
- [Capability ownership](ownership.md) assigns authority and compatibility promises.
- [Versioning and compatibility](versioning-and-compatibility.md) defines independent product, database, catalog, artifact, and API versions.
- [Legacy Python extraction](legacy-python-extraction.md) records the replacement path and removal gate.

The machine-readable inputs are `spec/platform/architecture.json`,
`spec/platform/version.json`, and `spec/contracts/compatibility-manifest.json`.
Run `python3 scripts/check_platform_contract.py --module-manifest
architecture/cidx-module-manifest.json` and
`uv run --project python python scripts/check_release_contract.py` before a
release.
