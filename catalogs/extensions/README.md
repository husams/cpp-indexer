# CIDX catalog extensions

Extensions are package-owned catalog entries and are never added directly to
the core ID ranges in `catalogs/core.json`. A package must use a qualified
identifier such as `acme.security/relation/taints` and ship its own manifest,
compatibility ledger, and catalog hash. Promotion into the core catalog is an
explicit migration with a new catalog version and a compatibility entry.

The generator intentionally accepts only declarative JSON metadata; executable
SQL, dispatch code, and arbitrary callbacks do not belong in an extension
catalog.
