run_id: cpp-indexer-v1
task-slug: cpp-indexer
stage: 7 of 8 — devops
date: 2026-05-17
story: Release pipeline — GHCR image publish + GitHub Release + local docker run runbook

## Pre-conditions verified

- CHARTER.md read; invariant I4 checked: test-report.md exists, 0 open QA_DEFECT entries.
- target: none (no cluster deploy). DEPLOY_DRIFT not applicable.
- kubectl context check skipped (no cluster target; story explicitly "image + release only").

## Inputs read

- CHARTER.md
- design.md (§5.6 CI + §5.7 Docker)
- plan.md (S37-docker-ci, S38-runbook story specs)
- Dockerfile (builder: rust:1-bookworm + llvm-18; runtime: debian:bookworm-slim + libclang-cpp18)
- .github/workflows/ci.yml (existing; not modified; docker job already present)
- logs/developer-s37-docker-ci.md (OOM context: 1943 MiB Docker Desktop; Dockerfile and CI believed correct)
- logs/developer-s38-runbook.md (sha256_hex("ALL") verified; daemon default listen 127.0.0.1:7878)
- docs/runbooks/staging-recovery.md, daemon-ops.md (for runbook cross-references)
- test-report.md (334 passed / 0 failed / 27 deferred; 0 QA_DEFECT)

## Orientation notes

- The ci.yml docker job (ubuntu-latest, needs: ci) already smoke-tests Dockerfile on every PR. No modification needed.
- S37 developer log confirmed: docker build OOM locally (exit 137) with 1943 MiB Docker Desktop; GHA ubuntu-latest (7-16 GiB) is the canonical build target.
- release.yml is a net-new file; no existing workflow is modified.
- GHCR image name derived from github.repository via docker/metadata-action (lowercase enforced).

## Files changed

| File | Action |
|------|--------|
| `.github/workflows/release.yml` | Created |
| `.claude/handoff/v1/deploy-notes.md` | Created |
| `.claude/handoff/v1/runbook.md` | Created |
| `.claude/handoff/v1/logs/devops.md` | Created (this file) |

## Key decisions

1. **Three-job structure** (binaries → image → release) matches advisor recommendation. `image` and `binaries` run in parallel; `release` `needs` both.
2. **macOS Intel excluded from v1**: `macos-latest` is arm64; Intel row is commented in the matrix with a v1.1 note. Avoids `macos-13` runner cost/complexity for v1.
3. **CMD not ENTRYPOINT**: maintains S37 decision so `docker run <image> cxg-index --version` resolves correctly without `--entrypoint` override.
4. **`:latest` gated to stable tags**: `enable=${{ !contains(github.ref_name, '-') }}` in metadata-action excludes pre-release tags from `:latest`.
5. **`softprops/action-gh-release@v2`**: used for GitHub Release creation (better asset upload DX than raw `gh release create`).
6. **Pre-flight resource check**: GHA ubuntu-latest satisfies ≥4 GiB RAM (CHARTER requirement). Flagged in deploy-notes.md for local builds.

## Deviations from plan / notes

- plan.md S37 open item: "the docker job does not push to any registry; add in a future devops story." This devops story is that future story — release.yml adds the push step.
- No cluster ops, no Vault, no ESO, no cert-manager, no ArgoCD, no NetPol, no kubectl. All are out of scope for this story by design.

## QA deferred items acknowledged

The 27 `#[ignore]`d tests in test-report.md require live Neo4j/IndraDB/network. These are not release blockers for the image/release pipeline itself. DevOps should provision `tests/compose/neo4j.yml` + `tests/compose/indradb.yml` before the M7 soak gate (S39-m7-soak-gate).

## Status

clear — all deliverables written; no cluster context; no open failure codes.
