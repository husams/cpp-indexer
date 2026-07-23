# Versioning and compatibility policy

`spec/platform/version.json` is the only release input. The generator produces
`src/cli/version.hpp` and `python/indexer/_version.py`; Python packaging reads
the latter through Hatch's dynamic version configuration. Never edit generated
files or copy a product version into a second hand-maintained constant.

The version families are deliberately independent:

| Family | Current | Meaning | Compatibility rule |
|---|---:|---|---|
| Product | `0.53.0` | C++ binary and Python distribution release | Must match across generated outputs; prerelease channel/tag is machine-readable |
| Database | schema `34` | SQLite tables and migrations | Writers migrate v2–v34; readers refuse outside the declared window; future schemas are never downgraded |
| Catalog | `1` + generated hash | Names, numeric IDs, fields, and relation descriptors | A changed hash requires a catalog version/compatibility entry; HSE-59 owns generation |
| Artifact | `1` | Persisted JSON/TSV/result artifact shapes | Readers accept only the declared range and report an actionable mismatch |
| API | `1` | Public SDK/CLI protocol surface | Additive changes stay within the major; breaking changes require a new major and migration guide |

Stable releases use `channel: stable` and `prerelease: null`. A prerelease is
an object such as `{ "tag": "rc1" }`, not an unparseable suffix hidden in a
free-form string. The generator validates this and emits both the base and full
product version.

## Release gate

`uv run --project python python scripts/check_release_contract.py` verifies:

- generated C++/Python outputs are current;
- package metadata reads the generated Python version;
- C++ and Python storage constants agree with the source contract;
- the committed `index.db` carries the current database schema;
- every compatibility-manifest schema and golden vector exists;
- every dual-executor contract names both implementations.

The same check runs in `.github/workflows/contract-check.yml`. A release job
must run it before building binaries, wheels, catalogs, or documentation. A
catalog hash mismatch therefore fails before publication rather than producing
mixed C++/Python artifacts.

## Support windows and owners

- Database migrations: Storage owners support the migration floor through the
  current schema; read-only tools support only the declared reader range.
- QueryPlan canonical JSON: Query owners preserve the v1 shape and golden
  vectors; changes require a manifest/schema review.
- CLI and legacy Python APIs: Product owners keep compatibility adapters until
  the usage/removal gate in `legacy-python-extraction.md` and the corresponding
  API migration guide are satisfied.
- Artifacts and catalogs: Platform/Catalog owners reject incompatible hashes or
  versions with stable error codes; no silent coercion is allowed.
