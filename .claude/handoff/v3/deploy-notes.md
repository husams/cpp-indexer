run_id: tu-parse-fail-v3
stage: 7 of 8 — devops
target-context: NONE
cluster-apply: NO CLUSTER APPLY PERFORMED. This is a documentation-only release artifact.

---

# Deploy Notes — Issue 0001 Fix (tu-parse-fail)

## Scope

User-visible behavioral changes in this release:

- libclang no longer receives driver tokens (`clang`, `gcc`, etc.), `-c`/`-o` pairs, or source-file repeats in its argument list. Fixes silent total TU parse failures when `compile_commands.json` includes these.
- CLI emits a structured closing summary line: `cxg-index: done — N TUs | N partial | N failed | N nodes | N edges`.
- New `--fail-on-tu-error <RATIO|never>` flag (default `1.0`). Exit 2 when `failed/total >= RATIO`. Default is back-compatible (exit 0 unless all TUs fail).
- Daemon `GET /v1/jobs/{id}` gains two additive fields: `failed_tu_count` (integer) and `status` ("completed" / "completed_with_errors" / "failed").

---

## Back-Compatibility

### --fail-on-tu-error default (ADR-3)

Default value is `1.0` (threshold = 100% of TUs must fail to trigger exit 2). Existing callers that do not pass the flag continue to exit 0 on partial failures. No script or pipeline changes required unless stricter failure policy is desired.

### Wire schema (ADR-4, AC-7)

`failed_tu_count` and `status` are additive fields with serde defaults. Legacy daemon response JSON without these fields deserialises correctly. No rolling-restart ordering constraint between daemon and API clients.

---

## Cache Invalidation (ADR-1)

`manifest.json` hash format is unchanged. However, the sanitised arg vector written to manifest entries differs from the raw arg vector present in pre-upgrade entries (driver token and `-c`/`-o` pairs are now stripped). On first run after upgrade, all existing manifest entries will be cache misses, triggering a full re-parse.

**Operator action required:** Re-run `cxg-index` once after upgrading. Cost: one full re-parse of the project. Cache repopulates normally on subsequent runs.

---

## CI Matrix — spdlog Smoke Test (AC-3)

The spdlog integration smoke test is gated behind `#[ignore]` and the `test-mock` feature. It must be run explicitly in CI on:

- macOS arm64
- Linux x86_64

### Correct invocation

```bash
cargo test --features test-mock --test spdlog_smoke -- --ignored
```

### Runner prerequisites (must be installed and on PATH before test invocation)

- `git`
- `cmake`
- A C++ toolchain (clang++ or g++)

**Warning:** The test performs graceful skip when `git`, `cmake`, or the C++ toolchain are absent or when network is unavailable. A misconfigured runner therefore produces a **false green** (test reported as skipped, not failed). CI must assert tool availability before invoking the test — do not rely on the test to detect missing tools.

Suggested pre-flight in CI:

```bash
which git cmake c++ || { echo "Required tools missing"; exit 1; }
```

### Default cargo test — spdlog_smoke is excluded

`cargo test --workspace` does not compile or run `spdlog_smoke` (requires `--features test-mock`). The default workspace pass remains clean.

---

## Pre-Existing Failure — schema_drift

`schema_drift::schema_txt_contains_all_promoted_fields` fails on unmodified `main` HEAD. Confirmed pre-existing by developer (git stash verification on all five story passes). Do not treat as a regression of this release. Tagged @sr-dev for triage.

---

## Release Checklist

- [ ] Merge PR; confirm `cargo test --workspace` green (minus pre-existing schema_drift).
- [ ] Add CI job for `spdlog_smoke` on macOS arm64 and Linux x86_64 with tool pre-flight (see above).
- [ ] On first post-upgrade index run: verify re-parse completes and cache repopulates (ADR-1 cache invalidation).
- [ ] Verify operator tooling does not rely on the old 4-token summary line. New format: `cxg-index: done — N TUs | N partial | N failed | N nodes | N edges`.
- [ ] Review `--fail-on-tu-error` policy; document chosen threshold in operator runbooks if tightening from default.
- [ ] Daemon consumers: no changes required. Wire schema back-compat verified (AC-7).

---

## References

- design.md §3 (files to touch), §4.1 (sanitise algorithm), §9 (risk register)
- implementation-notes.md (all five stories S1–S5, story s1-qd1-versioned-drivers)
- test-report.md (QA sign-off, QD-1 resolved, PRE_EXISTING noted)
- adr-1.md (cache invalidation consequences)
- adr-3.md (exit-code policy, default 1.0)
- adr-4.md (daemon wire schema additive fields)
