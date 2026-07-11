---
name: cidx-dual-implementation
description: Keep the Python and C++ implementations of cidx behaviorally compatible. Use when changing indexing, query, storage, CLI, JSON output, schema, or migrations — any observable behavior — to ensure the change lands in both languages in the same change, and to know what the shared contract and change discipline require.
---

# cidx dual-implementation parity

`cidx` has two implementations of the same indexer:

| Implementation | Location | Role |
|---|---|---|
| Python | `python/indexer/` | Canonical behavior and public Python API |
| C++23 | `src/` | Performance implementation built by CMake |

Treat **Python behavior as the reference**, but do not leave C++ parity for a
later change. Any change to the observable contract (SQLite schema/migrations,
indexing and query semantics, CLI flags and text output, JSON shapes, path
handling, exit behavior) must be implemented and tested in **both** languages in
the **same** change.

The main database schema version lives in `python/indexer/storage.py` and
`src/storage/storage.cpp` — update both together.

For the full contract surface, change-discipline rules, and how to report
validation, see [references/contract.md](references/contract.md).
