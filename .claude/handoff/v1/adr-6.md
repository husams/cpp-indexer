# ADR-6: git2 workspace clone manager — host allowlist, PAT-via-env, shallow by default

Status: accepted
Date: 2026-05-17
Resolves: M7-S3 git-URL ingestion; AC-M7-12..16

## Context

`POST /v1/ingest` with `{"git_url": "..."}` must clone (or fetch) into a configured workspace directory, auto-detect `compile_commands.json`, and start indexing. Requirements:

- Host allowlist from `[workspace].allowed_hosts` (suffix match). Reject 403 immediately on miss (AC-M7-14).
- HTTPS only for v1 (SSH deferred Q6/v2).
- PAT from env var named in `[workspace].git_credentials_env`; never log it or include it in API responses (AC-M7-15).
- Re-ingest re-uses the existing clone via `git fetch` (AC-M7-13).
- Shallow by default: `default_clone_depth = 1` (AC-M7-16).
- Clone path layout: `[workspace].dir/<repo-name>-<short-sha>/` (AC-M7-12).

## Decision

Module `src/workspace/` using `git2 = 0.19` (libgit2 binding).

### Allowlist

- `[workspace].allowed_hosts = ["github.com", "gitlab.senussi.me"]`. Match: case-insensitive host suffix (`foo.github.com` matches `github.com`). Implemented in `workspace::allowlist::is_allowed(url)`.
- Allowlist check runs **before** any network call. On miss, the handler returns `403 Forbidden` with RFC-7807 body naming the disallowed host. AC-M7-14.

### Auth

- `git2::Cred::userpass_plaintext(username="x-access-token", password=<PAT>)` where `<PAT>` is read from `std::env::var(<git_credentials_env>)` at clone/fetch time (not cached in memory across calls).
- For unauthenticated public repos, omit credentials entirely.
- `tracing` field redaction strips any field named `git_url` of a `user:pass@` prefix before logging. API responses echo only the bare URL.

### Layout

- Short sha is computed from the resolved `ref` (default `HEAD`). For a non-frozen ref like `main`, we resolve to commit SHA first via `git2::Remote::ls_remotes`, then `<repo-name>-<short_sha[..12]>`.
- Conflict resolution: if a directory at the target path exists and its `git rev-parse HEAD` matches the resolved sha, treat as already-cloned and run `git fetch` (AC-M7-13). If sha differs (shouldn't happen given naming), append `-<UUID>` suffix and log WARN.

### Clone vs fetch

- `clone(url, depth=1, ref="main")` on first ingest.
- Re-ingest: `git fetch origin <ref> --depth=1`, then `git reset --hard FETCH_HEAD`. AC-M7-13.
- `default_clone_depth = 0` (config) means full history; passed as no `depth` arg.

### Workspace housekeeping

- `[workspace].dir` is created on daemon startup (`0700` perms) if missing.
- No automatic GC of old clones in v1. Operator clears via `POST /v1/reset` (which also clears stage cache) or manually. Documented in runbook.

### Failure cases

- Network failure during clone: job moves to `failed` state with `Problem.detail` including the git2 error code. PAT scrubbed.
- Auth failure (HTTP 401 from upstream): job → `failed`; response says `git authentication failed` without echoing credentials.

## Alternatives considered

- **gix (gitoxide)** instead of git2: rejected for v1. gix HTTPS protocol support is improving but git2 is stable, battle-tested, and matches the team's existing tooling. Revisit when gix `transport-https` is recommended for production.
- **Shelling out to `git clone`**: rejected. Adds a subprocess dependency, complicates auth (would need `GIT_ASKPASS`), and makes PAT redaction harder.
- **Open allowlist by default**: rejected. Daemon would clone arbitrary URLs from any caller with the bearer token, expanding the attack surface.
- **Persistent credentials store (libsecret)**: rejected. Env-var indirection is simpler, matches `[sink]` credential pattern, and works in containers without a secret-store daemon.
- **No depth=1 default**: rejected. Full history adds minutes per clone on large repos and provides nothing the indexer uses.

## Consequences

Positive:
- Single allowlist + env-var pattern is uniform with the sink credential model.
- Re-ingest is cheap (fetch + reset).
- Clone path layout makes "which version of which repo" obvious from `ls`.

Negative:
- Stale clones accumulate without GC; runbook must call this out.
- Resolving `ref` to SHA before clone adds one `ls-remote` round-trip.

Follow-ups:
- v2: SSH auth via `Cred::ssh_key` from `~/.ssh/` keys; behind a config flag.
- v2: GC policy for clones older than N days.
- After M7: consider sparse-checkout for monorepos.

Revisit if: PAT scope/granularity becomes a per-repo concern (today: one PAT covers all clones).

## References

- requirements.md AC-M7-12..16
- engineering plan v1.1 §workspace/
- PRD v1.1 §6.7 FR-API-2, FR-API-11
- Cognee tags: `task:cpp-indexer role:architect`
