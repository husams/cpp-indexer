run_id: cpp-indexer-v1
stage: 7 of 8 — devops
date: 2026-05-17
target: none (image + release only; no cluster deploy)
story: Release pipeline — GHCR image publish + GitHub Release + local docker run runbook

---

# Deploy Notes

## Scope

This deployment delivers:
1. A new GitHub Actions release workflow (`.github/workflows/release.yml`) that triggers on semver tag pushes.
2. A GHCR Docker image (`ghcr.io/husams/cpp-indexer`) published on every tag.
3. Binary tarballs (Linux x86_64, macOS arm64) attached to a GitHub Release.
4. No Kubernetes manifests. No cluster context. `DEPLOY_DRIFT` does not apply.

## Cluster gate

The CHARTER invariant "verify kubectl config current-context matches target-context before any apply" is satisfied trivially: target is `none`. No `kubectl` commands are issued.

## What the release workflow does

| Job | Runner | Output |
|-----|--------|--------|
| `binaries` (matrix: 2 platforms) | ubuntu-latest / macos-latest | `cpp-indexer-vX.Y.Z-<target>.tar.gz` artifacts |
| `image` | ubuntu-latest | `ghcr.io/husams/cpp-indexer:vX.Y.Z` pushed to GHCR |
| `release` (needs both) | ubuntu-latest | GitHub Release created, tarballs attached |

## Secrets required

No secrets need to be added to the repository. The workflow uses only `secrets.GITHUB_TOKEN`, which GitHub automatically provisions for every Actions run. It has `contents: write` and `packages: write` permissions (declared at the workflow level).

## One-time bootstrap (REQUIRED before first release)

### A. Enable "Read and write permissions" for Actions

If the repository's default Actions token is read-only, the `contents: write` grant will be rejected. Verify at:
`https://github.com/husams/cpp-indexer/settings/actions` → "Workflow permissions" → select "Read and write permissions".

### B. Toggle GHCR package to public after first tag push

GHCR packages created by a personal account default to **private**. After pushing the first tag and confirming the `image` job succeeded:

1. Go to `https://github.com/users/husams/packages/container/cpp-indexer/settings`.
2. Scroll to "Danger Zone" → "Change package visibility" → Public.

This is a one-time step. All subsequent tags push to the now-public package automatically.

## Tagging strategy

Format: `vMAJOR.MINOR.PATCH` (stable) or `vMAJOR.MINOR.PATCH-<pre>` (pre-release, e.g., `v1.0.0-rc.1`).

- Stable tags: image tagged `:vX.Y.Z`, `:X.Y.Z`, `:X.Y`, `:X`, and `:latest`.
- Pre-release tags: image tagged `:vX.Y.Z-<pre>` only; `:latest` is NOT updated.
- GitHub Release: `prerelease: true` when tag contains `-`.

## Resource note

The `image` job builds the Docker image on a GitHub-hosted `ubuntu-latest` runner (7–16 GiB RAM, ≥2 cores). This satisfies the CHARTER pre-flight RAM requirement (≥4 GiB). Local Docker Desktop builds may fail with OOM if memory is below 4 GiB (confirmed in S37 developer log — 1943 MiB allocation OOM-killed). Do not build the image locally on a constrained host; use the GHA job or pve01/pve02.

## Binary tarball runtime dependency

All three binaries (`cxg-index`, `cxg-resolve-cross-repo`, `cxg-daemon`) link dynamically to libclang-18. Users of the binary tarballs MUST install libclang-18 on their host. The release body links to `docs/runbooks/libclang-setup.md`. The Docker image bundles the runtime libraries and does not require a host libclang.

## Rollback procedure

### Delete a GitHub Release (keep tag)

```bash
gh release delete vX.Y.Z --repo husams/cpp-indexer --yes
```

### Delete the tag

```bash
git push origin :refs/tags/vX.Y.Z
```

### Delete the GHCR image version

```bash
# List versions
gh api /user/packages/container/cpp-indexer/versions --jq '.[] | {id:.id, tags:.metadata.container.tags}'

# Delete by version ID (replace <VERSION_ID>)
gh api -X DELETE /user/packages/container/cpp-indexer/versions/<VERSION_ID>
```

## Files changed

| File | Action |
|------|--------|
| `.github/workflows/release.yml` | Created (new) |

No application code modified. No existing workflow modified.

## References

- `docs/runbooks/libclang-setup.md` — libclang-18 installation on Linux + macOS
- `.github/workflows/ci.yml` — CI workflow (unchanged; docker smoke job already validates the Dockerfile)
- CHARTER: `/Users/husam/workspace/cpp-indexer/.claude/handoff/v1/CHARTER.md`
- S37 developer log: `logs/developer-s37-docker-ci.md` (confirms OOM on <4 GiB Docker Desktop)
