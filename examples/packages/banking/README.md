# Banking extension package fixture

This bundle contains one extraction package and one analysis package.  It is
fully local and declarative: no credentials, network URL, shared library, or
machine-specific path is required.  Materialize both packages in a registry,
then resolve them into a lockfile before running the conformance cases. The
conformance matrix executes the extraction and analysis fixture contracts and
also covers excessive budgets, stale compatibility, malformed manifests, and
missing dependencies.
