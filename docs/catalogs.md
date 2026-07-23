# Semantic catalogs and artifact compatibility

`catalogs/core.json` is the versioned source for CIDX public semantic names,
stable IDs, relation metadata, result statuses, evidence classes, unknown
reasons, and artifact kinds. Run `python3 scripts/generate_catalogs.py --write`
after changing it; CI and the normal CMake build run `--check` and reject drift.

Generated C++, Python, SQL, JSON Schema, Soufflé, documentation, and golden
manifest artifacts carry the same catalog hash. CIDX index and AST-graph
artifacts record that hash in their metadata, and read-only consumers reject a
missing or incompatible hash with an actionable regeneration message.

Stable IDs are recorded in `catalogs/compatibility.json`. Renaming or reusing
an existing ID requires a migration entry in `core.json`; adding a new core
entry without such a migration is allowed. Extension packages use qualified
IDs and do not require edits to core dispatch, storage seed, graph query, or
Python constants.
