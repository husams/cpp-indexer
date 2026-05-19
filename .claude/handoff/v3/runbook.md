run_id: tu-parse-fail-v3
stage: 7 of 8 — devops
audience: operators running cxg-index or querying the daemon

---

# Operator Runbook — Issue 0001 Fix (tu-parse-fail)

## Trigger

Use this runbook when:
- Interpreting the `cxg-index` closing summary line after upgrading to this release.
- Investigating a non-zero exit code from `cxg-index`.
- Querying daemon job status via `GET /v1/jobs/{id}` and inspecting `status` / `failed_tu_count`.
- First run after upgrade (cache invalidation — see Cache section).

---

## Prerequisites

- `cxg-index` binary built from this release (tu-parse-fail fix).
- For daemon status: daemon process running, `GET /v1/jobs/{id}` accessible.
- No cluster apply involved. No Kubernetes or ArgoCD operations required.

---

## 1. Closing Summary Line

After `cxg-index` completes, a summary line is emitted to stderr:

```
cxg-index: done — N TUs | N partial | N failed | N nodes | N edges
```

Token order is fixed (AC-4, ADR-2). Fields:

| Token | Meaning |
|-------|---------|
| `N TUs` | Total translation units attempted |
| `N partial` | TUs parsed with warnings / partial AST |
| `N failed` | TUs that failed completely (libclang could not parse) |
| `N nodes` | Graph nodes written |
| `N edges` | Graph edges written |

### Interpreting "failed: N > 0 but exit 0"

This is expected when the failure ratio is below the configured `--fail-on-tu-error` threshold. With the default threshold (`1.0`), the process exits 0 unless every TU fails. Check the `--fail-on-tu-error` flag in use and compare `failed / total` against it.

---

## 2. Exit Code Reference

| Exit code | Meaning |
|-----------|---------|
| `0` | Success: `failed == 0`, or `failed / total < threshold`, or `total == 0` |
| `2` | Threshold breach: `failed / total >= --fail-on-tu-error` value |

**Collision note:** clap argument parse errors also exit `2` (accepted risk, ADR-3 §Risk register). Disambiguate via stderr — a threshold breach emits the closing summary line and no "error:" prefix; a clap parse error emits a usage message with "error:" prefix and no summary line.

---

## 3. --fail-on-tu-error Flag

```
--fail-on-tu-error <RATIO|never>   [default: 1.0]
```

- `never` — always exit 0 regardless of failures.
- `0.0` — exit 2 if any TU fails (`failed >= 1`).
- `0.5` — exit 2 if half or more TUs fail.
- `1.0` (default) — exit 2 only if all TUs fail.

### Threshold semantics

```
exit_code = 2  iff  failed > 0  AND  (failed / total) >= RATIO
exit_code = 0  iff  failed == 0  (short-circuit; covers total == 0)
```

Zero TUs processed (`total == 0`) always exits 0 regardless of ratio.

---

## 4. Daemon GET /v1/jobs/{id} — New Fields

Two fields have been added to the job record (additive; AC-6, AC-7, ADR-4):

### failed_tu_count

Type: integer. Always present. Defaults to `0` in legacy records that pre-date this release.

Counts translation units that failed completely during the indexing run.

### status

Type: string enum. **Omitted from the response while the job is queued or running** (`skip_serializing_if Option::is_none`). Present only after the job finishes.

| `status` value | Condition |
|----------------|-----------|
| `"completed"` | `failed_tu_count == 0` (including zero-TU runs) |
| `"completed_with_errors"` | `0 < failed_tu_count < total_tu_count` |
| `"failed"` | `failed_tu_count >= total_tu_count` |

The existing `state` field lifecycle is unchanged.

### Example response (completed with errors)

```json
{
  "id": "abc123",
  "state": "done",
  "status": "completed_with_errors",
  "failed_tu_count": 3,
  "tus_ok": 4,
  "tus_total": 7
}
```

### Example response (in-flight — status absent)

```json
{
  "id": "abc123",
  "state": "running",
  "failed_tu_count": 0,
  "tus_ok": 2,
  "tus_total": 7
}
```

---

## 5. Cache Invalidation After Upgrade

On first `cxg-index` run after upgrading, all existing `manifest.json` cache entries will be misses. Reason: manifest entries were hashed with raw args; they now differ from the sanitised arg vectors written by the new version.

**Action:** Allow the first post-upgrade run to complete a full re-parse. Subsequent runs use the populated cache normally. No manual manifest cleanup required.

---

## 6. Rollback

No cluster resources to roll back. If the new binary must be reverted:

1. `git revert <release-commit>` and rebuild.
2. Downgrade daemon binary; restart daemon process.
3. Re-run `cxg-index` to repopulate manifest cache (same one-shot cost as upgrade).
4. Consumers polling `GET /v1/jobs/{id}`: `failed_tu_count` and `status` fields will no longer appear; ensure clients handle absent fields gracefully (serde default / `Option`).

---

## On-Call Notes

- `failed: N > 0, exit 0` with default threshold is expected and not an incident.
- `failed: N > 0, exit 2` indicates threshold was breached; inspect the compiler toolchain and `compile_commands.json` for malformed entries. After sanitisation fix, remaining failures are genuine libclang parse errors.
- Daemon `status` absent in response = job still queued or running. Not an error.
- Pre-existing `schema_drift::schema_txt_contains_all_promoted_fields` test failure: confirmed pre-dates this release; not an incident introduced by this change.

---

## References

- deploy-notes.md: /Users/husam/workspace/cpp-indexer/.claude/handoff/v3/deploy-notes.md
- design.md §4.2 (closing_summary), §4.3 (exit_code), §4.4 (JobOutcome derivation)
- adr-1.md (sanitise algorithm and cache consequences)
- adr-2.md (closing summary format)
- adr-3.md (exit-code policy)
- adr-4.md (daemon wire schema)
- test-report.md (QA sign-off)
