# Versioned extension packages

CIDX extension packages are declarative, content-addressed data bundles.  The
Python SDK in `indexer.extensions` owns manifest validation, registry
materialization, dependency resolution, lockfile verification, provenance
identities, and conformance cases.  Installing a package never grants trust.

Hosts supply a `PackagePolicy` outside the package manifest. It can restrict
registry identities, publishers, hashes, package kinds, capabilities, sandbox
profiles, and trusted signatures. Publisher-verified and trusted packages also
require externally allowlisted publisher/hash/signature evidence; resolution
and lock verification fail closed when evidence is absent. Compatibility facts
come from the runtime catalog and artifact contracts, so omitted environment
values are not treated as compatible.

Each `package.json` declares a package kind (`cidx.extract`, `cidx.analysis`,
`cidx.query`, or `cidx.model`), qualified namespaces, entry points, schemas,
compatibility ranges, dependencies, budgets, a sandbox profile, and a
`sha256:` content hash.  Package files are limited to declarative formats;
shared libraries, shell/Python programs, SQL, and executor capabilities are
rejected before a package can enter a registry.

Registries are searched in deterministic order.  A workspace registry should
be passed first, followed by the user registry and explicitly configured
registries.  `PackageResolver.resolve()` selects one compatible version per
name, rejects missing or cyclic dependencies and namespace collisions, and
returns a lockfile whose hash covers roots and every exact package hash.

```sh
python -m indexer package validate examples/packages/banking/extract
python -m indexer package lock \
  --registry .cidx/packages \
  --require banking.extract@^1.0 \
  --require banking.analysis@^1.0 \
  --output cidx.lock.json
python -m indexer package verify cidx.lock.json --registry .cidx/packages
```

Result producers should include `ResultProvenance.as_dict()` and use
`artifact_identity()` for cache keys.  This makes a package or lockfile change
invalidate every dependent fact, analysis, query, and proof artifact while
retaining package name/version/hash, entry point, input fact sets, sandbox,
and applicability in the result evidence.

`ConformanceSDK` executes extraction plans and analysis rules against JSON
fixture inputs, including dependency-produced facts, compares normalized
facts/results, and enforces output resource limits. It covers malformed
manifests, stale compatibility, missing dependencies, and budget limits. It
does not execute package code or native plugins.
