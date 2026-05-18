# ADR-11: SCHEMA_VERSION bump to v5 and field-promotion handshake policy for M8

Status: accepted
Date: 2026-05-18
Resolves: requirements OQ-1 (dual-write of is_virtual/is_pure_virtual/is_static), AC-S40-1/5/6, PRD §AC-S3
Supersedes: none (extends ADR-9)

## Context

ADR-9 governs schema versioning: monotonic `u32` `SCHEMA_VERSION`, hard-refuse on mismatch, no in-place migration. Current value is `SCHEMA_VERSION = 4` / tag `cxg-schema-v4` (`src/schema/version.rs:16,22`). M8 promotes ten node fields and two edge fields from `attrs_json` into native columns. Three booleans (`is_virtual`, `is_pure_virtual`, `is_static`) currently live inside `attrs_json` (PRD §2.3 sample). The PRD AC-S3.1 enumeration is ambiguous about whether the booleans move fully out or are dual-written during a transition window.

Dual-writing creates exactly the silent-divergence failure mode ADR-9 was designed to prevent: two sources of truth for the same value, with no compiler-enforced equality. The whole point of the version bump is to make pre-M8 and post-M8 graphs incompatible — there is no "transition window" to support because mixed graphs are refused at handshake.

## Decision

1. Bump `SCHEMA_VERSION` from `4` to `5` in `src/schema/version.rs`. Update `SCHEMA_VERSION_TAG` to `cxg-schema-v5` and `PARQUET_MAGIC` to `cxg_parquet_v5`. Update the bump-log doc-comment with an `M8 (S40)` entry.
2. **Full promotion, no dual-write.** `is_virtual`, `is_pure_virtual`, `is_static`, `return_type`, `params`, `signature`, `code`, `code_truncated`, `template_params`, `template_args` are removed from `attrs_json` for the kinds that carry them. AC-S40-6 is interpreted strictly: a promoted field MUST NOT appear in both places.
3. `attrs_json` is retained as the long-tail bag for `exception_spec`, `control_flow`, `bit_field`, `template_kind`, MACRO `params`/`is_function_like`/`is_builtin`, REPO `root_path`/`commit_sha`/`commit_date`/`sink`, and any future not-yet-promoted attributes.
4. The schema-handshake test (`m6_agent_gate`) and Phase 5 cross-repo gate already refuse on mismatch (`src/resolve/cross_repo.rs:188`). No new code is required for AC-S40-5; the existing `Error::Schema` path covers `SchemaVersionMismatch`. The S40 work is the bump itself plus updating `tests/schema-baseline.txt` to match the new `nodes.rs`/`edges.rs` Blake3.
5. CI bump-gate (`tests/schema_version_bump.rs`) is the enforcement; the ADR-9 "additive optional" exemption does **not** apply to M8 because we are *moving* fields out of `attrs_json`, which is a breaking change.

## Alternatives considered

- **Dual-write during a transition window.** Rejected: violates ADR-9's single-source-of-truth principle; creates a divergence bug surface (one sink path forgets to update one of the two locations); no operational benefit because the version bump already forbids mixed graphs.
- **Promote only the booleans, leave `code`/`params` in `attrs_json` until M9.** Rejected: would force agents to keep parsing JSON for the highest-cardinality queries (Q1 in PRD §6) — defeats G1/G5.
- **No bump; rely on additive-optional exemption.** Rejected: removal from `attrs_json` is non-additive. An old query like `attrs_json CONTAINS '"virtual":true'` returns wrong results against a v5 graph if the bump is skipped.

## Consequences

Positive:
- Single source of truth per field; no divergence bug surface.
- Hard handshake refusal makes the upgrade boundary observable; operators re-index per the runbook (AC-S46-2).
- Existing CI gate continues to enforce that future schema PRs bump deliberately.

Negative:
- Every pre-M8 graph (including the live dev cluster's leveldb graph from 2026-05-17) must be wiped and re-indexed before M8 binaries can write to it. Mitigated by AC-S46-2 runbook recipe and by `reset_repo` already supporting targeted wipes (Phase 4).
- Tests that previously asserted `attrs_json` contained `"virtual":true` must be migrated to assert the native column. Tracked as part of S40 implementation.

Follow-ups:
- After M8 lands, monitor for `attrs_json` fields that become hot enough to justify their own promotion (PRD §9: `exception_spec`, `control_flow`). Defer until a real query motivates it.

## References

- `[[pages/planning/cpp-indexer-structured-attrs-prd]]` §AC-S3, §3 G6
- ADR-9 (cross-repo schema versioning, accepted 2026-05-17)
- `src/schema/version.rs:16-25`
- `src/resolve/cross_repo.rs:188`
- requirements.md AC-S40-1..S40-6, OQ-1
- Cognee tags: `task:cpp-indexer-m8 role:architect`
