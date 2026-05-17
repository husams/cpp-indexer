run_id: cpp-indexer-v1
stage: 7 of 8 — devops
date: 2026-05-17
story: Release pipeline — GHCR image publish + GitHub Release + local docker run runbook
references:
  - .github/workflows/release.yml (source of truth for release steps)
  - docs/runbooks/staging-recovery.md (corrupted staging recovery)
  - docs/runbooks/daemon-ops.md (daemon start/stop/config)
  - docs/runbooks/sink-failover.md (Neo4j/IndraDB failover)
  - docs/runbooks/observability.md (metrics + RUST_LOG)
  - docs/runbooks/libclang-setup.md (libclang-18 install, required for binary tarballs)

---

# Release Runbook: GHCR Image + GitHub Release

## Trigger

Push a semver-format tag to `main`:

```bash
git tag -s v1.0.0 -m "v1.0.0: first GA release"
git push origin v1.0.0
```

This starts the three-job `Release` workflow (`.github/workflows/release.yml`). The CI workflow (`ci.yml`) is NOT re-triggered by tag pushes; it only runs on branch pushes and PRs.

## Prerequisites

Before cutting the first release:

1. **Actions write permission** — confirm at `https://github.com/husams/cpp-indexer/settings/actions` → "Workflow permissions" → "Read and write permissions".
2. **GHCR public toggle (one-time)** — after the first tag is pushed and the `image` job succeeds, go to `https://github.com/users/husams/packages/container/cpp-indexer/settings` and set visibility to Public.
3. **Clean main branch** — all stories merged; CI green on `main`.
4. **`benches/baseline.json` populated** (optional but recommended) — run `cargo bench --bench llvm_index -- --output-format bencher` on a representative Linux machine and commit the `ns/iter` values to activate the regression gate (AC-M7-22).

## Steps — Cutting a release

1. Ensure you are on `main` with a clean working tree:

   ```bash
   git checkout main
   git pull --ff-only origin main
   git status   # must be clean
   ```

2. Create a signed annotated tag (preferred) or lightweight tag:

   ```bash
   # Stable release
   git tag -s v1.0.0 -m "v1.0.0: first GA release"

   # Pre-release (RC)
   git tag -s v1.0.0-rc.1 -m "v1.0.0-rc.1: release candidate"
   ```

3. Push the tag:

   ```bash
   git push origin v1.0.0
   ```

4. Monitor the workflow at `https://github.com/husams/cpp-indexer/actions` — three jobs run:
   - `Build binaries (x86_64-unknown-linux-gnu)` — ~10-15 min
   - `Build binaries (aarch64-apple-darwin)` — ~15-20 min
   - `Build + push GHCR image` — ~20-30 min (parallel with binaries)
   - `Create GitHub Release` — ~1 min (runs after both above pass)

## Verification

After the workflow completes:

1. **GitHub Release exists** — check `https://github.com/husams/cpp-indexer/releases`. Expected assets:
   - `cpp-indexer-v1.0.0-x86_64-unknown-linux-gnu.tar.gz`
   - `cpp-indexer-v1.0.0-aarch64-apple-darwin.tar.gz`

2. **GHCR image is pullable**:

   ```bash
   docker pull ghcr.io/husams/cpp-indexer:v1.0.0
   docker run --rm ghcr.io/husams/cpp-indexer:v1.0.0 cxg-index --version
   # Expected: cpp-indexer 1.0.0 (or similar)
   ```

3. **Binary tarball smoke test** (Linux):

   ```bash
   curl -L -o /tmp/cxg.tar.gz \
     https://github.com/husams/cpp-indexer/releases/download/v1.0.0/cpp-indexer-v1.0.0-x86_64-unknown-linux-gnu.tar.gz
   tar -xzf /tmp/cxg.tar.gz -C /tmp
   /tmp/x86_64-unknown-linux-gnu/cxg-index --version
   # Requires libclang-18 on host — see docs/runbooks/libclang-setup.md
   ```

4. **Image tags** (stable release should have `:latest`):

   ```bash
   docker pull ghcr.io/husams/cpp-indexer:latest
   ```

   For a pre-release tag (`v1.0.0-rc.1`), `:latest` is NOT updated — only `:v1.0.0-rc.1`.

## Local Docker Run Quickstart

### Pull the image

```bash
docker pull ghcr.io/husams/cpp-indexer:v1.0.0
# or use :latest for the most recent stable release
docker pull ghcr.io/husams/cpp-indexer:latest
```

### Run the daemon

The daemon requires a config file and a bearer token env var:

```bash
# Minimal config (save to cxg-daemon.toml):
# [sink]
# backend = "neo4j"
# [sink.neo4j]
# uri = "bolt://host.docker.internal:7687"
# user = "neo4j"
# password_env = "NEO4J_PASSWORD"
# [api]
# listen = "0.0.0.0:7878"
# auth_token_env = "CXG_API_TOKEN"
# job_queue_max = 64
# [index]
# stage_dir = "/var/lib/cxg-daemon/stage"

export CXG_API_TOKEN="$(openssl rand -hex 32)"

docker run -d \
  --name cxg-daemon \
  -p 7878:7878 \
  -e CXG_API_TOKEN="$CXG_API_TOKEN" \
  -e NEO4J_PASSWORD="<neo4j-password>" \
  -e RUST_LOG=info \
  -v "$PWD/cxg-daemon.toml:/etc/cxg-daemon/cxg-daemon.toml:ro" \
  -v /var/lib/cxg-daemon:/var/lib/cxg-daemon \
  ghcr.io/husams/cpp-indexer:latest \
  /usr/local/bin/cxg-daemon --config /etc/cxg-daemon/cxg-daemon.toml
```

Check daemon health:

```bash
curl -s http://127.0.0.1:7878/v1/status | jq .
```

### Run cxg-index (one-shot, no daemon)

```bash
docker run --rm \
  -e LIBCLANG_PATH=/usr/lib/llvm-18/lib \
  -v /path/to/repo:/repo \
  ghcr.io/husams/cpp-indexer:latest \
  cxg-index /repo
```

Note: `compile_commands.json` must exist at or above `/repo` (auto-detected by upward walk). The config file must reference a reachable sink endpoint (host Neo4j or IndraDB). Pass `-e` flags for sink credentials matching your config's `password_env` / `token_env` values.

### Run cxg-resolve-cross-repo (Phase 5)

```bash
docker run --rm \
  -v /path/to/repo:/repo \
  -e NEO4J_PASSWORD="<password>" \
  ghcr.io/husams/cpp-indexer:latest \
  cxg-resolve-cross-repo --config /repo/cxg-index.toml
```

### Override the default command

All three binaries are on PATH inside the image. Override CMD:

```bash
docker run --rm ghcr.io/husams/cpp-indexer:latest cxg-resolve-cross-repo --help
docker run --rm ghcr.io/husams/cpp-indexer:latest cxg-daemon --help
```

## Rollback

### Delete a release (keep tag)

```bash
gh release delete v1.0.0 --repo husams/cpp-indexer --yes
```

### Delete the tag

```bash
git push origin :refs/tags/v1.0.0
```

A tag deletion does NOT re-trigger the release workflow.

### Delete a GHCR image version

```bash
# List versions and their IDs
gh api /user/packages/container/cpp-indexer/versions \
  --jq '.[] | {id:.id, tags:.metadata.container.tags}'

# Delete the specific version (replace <ID>)
gh api -X DELETE /user/packages/container/cpp-indexer/versions/<ID>
```

## Tagging strategy reference

| Tag format | Example | Image tags applied |
|---|---|---|
| Stable | `v1.2.3` | `:v1.2.3`, `:1.2.3`, `:1.2`, `:1`, `:latest` |
| Pre-release | `v1.0.0-rc.1` | `:v1.0.0-rc.1` only |

## On-call notes

- **Release workflow fails at `binaries`**: most likely a libclang-18 APT install failure (upstream LLVM repo sometimes returns 503). Re-run the failed job from the GitHub Actions UI — retries succeed once the repo recovers.
- **Release workflow fails at `image` with OOM**: GHA ubuntu-latest provides 7-16 GiB; this should not occur. If it does, check if the runner assignment changed and file an issue.
- **GHCR push fails with 403**: token permissions issue. Ensure `packages: write` is in the workflow permissions block and that the repository's Actions setting allows write tokens.
- **Binary tarball crashes on `libclang-18.so.1 not found`**: user needs libclang-18 installed. Point them to `docs/runbooks/libclang-setup.md`.
- **Daemon in-container cannot reach Neo4j on host**: use `host.docker.internal:7687` (Docker Desktop) or the host's Tailscale IP. The `bolt://localhost:7687` config is wrong when inside a container.

## Operator runbook cross-references

All operational procedures for the daemon (start/stop, config, job lifecycle, staging recovery, sink failover, observability, libclang setup) live in `docs/runbooks/`:

| Runbook | Covers |
|---|---|
| `docs/runbooks/daemon-ops.md` | Start/stop, minimal config, REST endpoints, job lifecycle |
| `docs/runbooks/staging-recovery.md` | Corrupted `.cxg-cache/` recovery, `POST /v1/reset`, re-index loop |
| `docs/runbooks/sink-failover.md` | Neo4j / IndraDB failover, preflight errors |
| `docs/runbooks/observability.md` | Prometheus metrics, `RUST_LOG`, structured logging |
| `docs/runbooks/libclang-setup.md` | libclang-18 install on Linux + macOS (required for binary tarballs) |

The 7-day soak checklist is at `tests/integration/m7_soak_checklist.md` (AC-M7-25).
