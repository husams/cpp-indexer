---
name: cidx-dual-implementation
description: The C++/Python split of cidx and its change discipline. Use when changing storage schema, migrations, or read-query behavior — the areas still shared with the Python tree — to know what must land in both languages and what is C++-only since the indexer cutover.
---

# cidx dual-implementation discipline

The byte-identical dual-implementation contract is **retired**. The split
today:

| Implementation | Location | Role |
|---|---|---|
| C++23 | `src/` | The SOLE indexer (Clang C++ API) + CLI |
| Python | `python/indexer/` | Storage + graph read-query API (its libclang indexer is legacy pending removal — do not extend it) |

Indexing/query/CLI/schema changes land in **C++ with C++ tests**. Only the
still-shared surface must land in both languages in the same change: SQLite
schema + migrations (with old-database tests) and storage/read-query
semantics the Python API exposes.

The main database schema version lives in `python/indexer/storage.py` and
`src/storage/storage.cpp` — update both together.

For the full contract surface, change-discipline rules, and how to report
validation, see [references/contract.md](references/contract.md).
