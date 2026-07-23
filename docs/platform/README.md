# CIDX platform contracts

These documents are the M0 boundary for the C++ core and Python SDK.

- [Capability ownership](ownership.md) assigns authority and compatibility promises.
- [Versioning and compatibility](versioning-and-compatibility.md) defines independent product, database, catalog, artifact, and API versions.
- [Legacy Python extraction](legacy-python-extraction.md) records the replacement path and removal gate.

The machine-readable inputs are `spec/platform/version.json` and
`spec/contracts/compatibility-manifest.json`. Run
`uv run --project python python scripts/check_release_contract.py` before a
release.
