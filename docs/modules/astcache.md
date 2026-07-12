# `src/astcache` — the AST cache

[← docs index](../README.md) · related: [cli](cli.md)

An on-disk cache of libclang translation units for the `cidx ast` commands, so
repeated on-demand AST queries against the same file don't re-parse. ~0.4k LOC.
Byte-parity with the Python cache, so `.ast` files interoperate.

## Files

`astcache.hpp` / `astcache.cpp`. Cached TUs are `.ast` files under
`~/.cache/cidx/files/`, each with a JSON validity sidecar.

## Types & functions (`astcache.hpp`)

| Symbol | Role |
|---|---|
| `AstTarget` `:26` | abspath + flags + optional driver + focus USR/name |
| **`cache_key(t)`** `:57` | SHA-1 hex over `abspath\0flags[\0drv\0driver]` — the file identity (frozen doctest vector) |
| **`flags_hash(t)`** `:51` | SHA-1 hex over flags (+driver) only — no abspath; stored in the sidecar |
| `cache_dir` `:42`, `files_dir` `:46`, `libclang_version` `:63` | paths + version stamp |
| `Sidecar` `:73` | abspath, flags_hash, src_mtime, libclang_version |
| `read_sidecar` `:81`, `write_sidecar` `:84`, **`is_valid`** `:90` | validity check: src accessible + flags_hash + src_mtime + libclang_version + abspath all match |
| `load_ast` `:100`, `reparse` `:104`, `try_save` `:109` | TU I/O |
| **`load_or_parse(t, use_cache, err)`** `:118` | the entry point: return a `ParsedTu` from cache when fresh, else live reparse |

## Relationship to indexing

This cache serves the interactive `cidx ast dump/locals/conditions` commands
(see [cli](cli.md)); it is independent of the stored graph and of the two
[indexing engines](../indexing-engines.md). `cidx cache …` manages it.

> Note: the `cache_key` frozen doctest vector is path-dependent, so the
> `astcache_test` unit test fails on any checkout not at the author's original
> path — a known environmental test artifact, unrelated to indexing.
