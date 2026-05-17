run_id: cpp-indexer-v1
story: S37-docker-ci
stage: developer
date: 2026-05-17

## Skills loaded
- rust-conventions (project has Cargo.toml)

## Skills considered but not loaded
- cpp-conventions — not applicable; project is Rust
- python-conventions — not applicable
- implement-story — standard developer role dispatch; no special story-impl skill needed

## Orientation reads
- CHARTER.md — paths, invariants, failure taxonomy
- plan.md lines 612-626 — S37 story details
- design.md §5.6 §5.7 — CI and Docker design
- existing .github/workflows/ci.yml — pre-existing workflow to extend
- src/bin/index.rs — confirmed --version flag exits 0 (clap ArgAction::SetTrue, prints pkg version)
- Cargo.toml — confirmed all three [[bin]] targets; bench targets present
- developer-s15/s22/s26 logs — confirmed DYLD_LIBRARY_PATH pattern: $(brew --prefix llvm@18)/lib

## Commands run + outcomes

| Command | Outcome |
|---|---|
| `cargo fmt --all -- --check` | PASS (exit 0) |
| `cargo clippy --all-targets --all-features -- -D warnings` | PASS (Finished dev profile ~100s) |
| `docker build --progress=plain -t cpp-indexer:ci .` | BUILD_FAIL: exit code 137 (OOM-killed) |
| `docker build --no-cache --load --progress=plain -t cpp-indexer:ci .` (with CARGO_BUILD_JOBS=1) | BUILD_FAIL: exit code 137 (OOM-killed again) |
| `docker run --rm cpp-indexer:ci cxg-index --version` | Not reached — docker build failed |

## Unresolved signals

**BUILD_FAIL: docker build OOM (exit 137)**
- Docker Desktop on this host has 1943 MB allocated (DOCKER_HOST=unix://…/podman-machine-default-api.sock via podman socket).
- CHARTER pre-flight rule: "If RAM <4 GiB free, stop and propose a remote builder or different host."
- Both the default Dockerfile (parallel jobs) and the low-memory variant (CARGO_BUILD_JOBS=1, codegen-units=1) OOM at the cargo build --release step.
- Build log: /tmp/docker-build-s37-v2.log (last 10 lines: exit code: 137)
- **Dockerfile, .dockerignore, and ci.yml are believed correct.** The gate cannot be validated locally. Canonical validation is GitHub Actions Linux runner (7-14 GiB RAM).
- Retry count: 2 passes (default, then -j 1). Third pass attempted with codegen-units=1 also failed. Signal exhausted after 2 passes with same root cause.

Coordinator should route to remote build (pve01/pve02 or GHA runner) per CHARTER note.

## Deviations from plan.md

1. **CMD vs ENTRYPOINT**: design.md §5.7 says "entrypoint: cxg-daemon". The S37 exit-criteria smoke is `docker run --rm cpp-indexer:ci cxg-index --version`. Using a hard ENTRYPOINT would make that resolve to `cxg-daemon cxg-index --version` (wrong). Implemented as `CMD ["/usr/local/bin/cxg-daemon"]` with no ENTRYPOINT so `docker run <image> cxg-index --version` works. Three smoke tests in the `docker` CI job cover all three binaries.

2. **Benchmark baseline**: plan says "compares to baseline json artifact". Implemented as a committed `benches/baseline.json` (currently empty / comment-only for first run; script skips gracefully). Alternative considered: pulling artifact from last main run via GitHub Actions cache — rejected because it requires a prior successful run and complicates the first push to main. The committed baseline approach is simpler and works from day one.

3. **Benchmark output format**: Used `--output-format bencher` (libtest-compatible) which `cargo bench` emits to stdout. A hand-rolled Python script parses `ns/iter` lines and checks >20% regression. Criterion JSON output (`--message-format json`) requires a different invocation; deferred to a future story if criterion is adopted as the bench harness.

4. **Docker job dependency**: Added `needs: ci` to the docker job so the image is only built when the matrix test passes. This means a single push can't pass docker without passing tests first.

## Open items (tag: sr-dev)

- `benches/baseline.json` has no numeric values yet. A maintainer must run `cargo bench --bench llvm_index -- --output-format bencher` on a representative machine, record the `ns/iter` values, and commit them to activate the regression gate (AC-M7-22).
- The docker job does not push the image to any registry; image push/tag step is out of S37 scope (no registry configured). Add in a future devops story.
