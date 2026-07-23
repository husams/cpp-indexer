# `src/util` — foundations

[← docs index](../README.md)

Small, dependency-free building blocks used across the codebase. Many exist to
preserve the declared storage/read-query contracts and deterministic output
(see the [overview](../overview.md#design-invariants)). ~1.7k LOC.

| Module | Role |
|---|---|
| `logger` | Python-parity logging: fixed record format, **lazy** file creation (log opened on first record), a warning counter, stderr fallback |
| `env` | environment lookup + the two distinct "falsy" spelling sets (`CIDX_STRICT`/`CIDX_GNUC_VERSION` vs `INDEXER_IGNORE_SYSTEM_HEADERS`) |
| `subprocess` | `posix_spawnp`-based runner for the driver probe: empty stdin, captured stdout/stderr, 30 s timeout with SIGKILL, never throws (spawn failure → exit 127) |
| `hashing` | `md5_of` — frozen lowercase MD5 hex of a file's content, for staleness detection |
| `json_min` | minimal JSON codec for arrays-of-strings only (the `compile_options` column); read-compatible with Python `json.dumps(list[str])` |
| `pathutil` | POSIX `os.path`/`posixpath` semantics reimplemented (`normpath`, `abspath`, `join`, `relpath`, `basename`) because `std::filesystem` disagrees on DB-stored edge cases |
| `repo` | git repo discovery without shelling out; a tiny `.git/config` INI scanner, worktree-aware (follows `gitdir`/`commondir`) |
| `files` | file-argument resolution + index-state logic (the CLI skip decision is **md5-only**; mtime is stored but never consulted) |
| `errors` | `CidxError` / `UsageError` / `StorageError` exception hierarchy |
| `prof` | lightweight profiling hooks |

## Notable consumers

- `subprocess` + `env` power [`toolchain/`](toolchain.md)'s driver
  introspection (`<driver> -E -v`).
- `hashing` + `files` drive the incremental-index skip decision (see the
  [per-file interleave](../data-flow.md#the-per-file-interleave)).
- `pathutil` + `json_min` keep `file.compile_options` and stored paths
  deterministic and compatible with the Python SDK's declared read contract.
