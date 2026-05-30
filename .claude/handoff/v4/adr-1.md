# ADR-1: ID namespace — per-repo integer IDs; cross-repo resolution stays in USR space

Status: accepted

## Context

The CodexGraph today keys every node on a libclang **USR string** (`usr`) and every edge on
`src_usr`/`dst_usr` (see `src/schema/nodes.rs`, `src/schema/edges.rs`). USRs are long and repeated
on every node and edge — the primary graph-size bloat this feature removes. We are replacing the
USR strings *stored in the durable graph* with compact integer IDs.

Two forces collide:

- **Compactness** wants small monotone integers, allocated densely starting from 1, so the graph
  shrinks. Dense per-repo counters give the smallest possible IDs.
- **Cross-repo correctness.** Phase 5 (`src/resolve/cross_repo.rs`, `EXTERNAL_REF`) matches
  references across repos by USR string. A symbol defined in repo A and referenced from repo B has
  the *same USR* in both, but there is no global integer authority that would give it the same
  integer in both repos without a distributed allocator.

Decision D1 (requirements.md, ACCEPTED, not open for discussion) already fixes the namespace as
per-repo. This ADR records the decision and its full consequence chain so downstream stages do not
re-derive it.

## Decision

1. **Integer IDs are allocated per indexed repo.** Each repo has its own SQLite database (ADR-3)
   with its own independent `id` counters for `symbols` and `files`. Integer IDs from two repos are
   **never** compared, joined, or assumed equal (D1).
2. **Cross-repo *matching* stays entirely in USR-string space.** Phase 5 continues to build its
   global map keyed by USR (`build_global_usr_map` in `src/resolve/cross_repo.rs`) and *match*
   `cross_repo_candidate` edges by `dst_usr`. The matching logic MUST NOT be changed to compare
   integer IDs across repos.
3. **The staging/Parquet records carry BOTH `usr` and the integer `id`.** Allocation happens during
   the parallel libclang visit (ADR-3 §allocator placement); the integer is attached to each
   `NodeRecord`/`EdgeRecord` alongside the existing USR. The sink (Phase 4/6) writes the integer and
   drops the USR string from the durable graph. This is the seam that satisfies D1 *and* the S4
   references to `arrow.rs`/`nodes.rs`/`edges.rs` *and* the S2-SC-10 "allocate during parallel TU
   visit" scenario.
4. **EXTERNAL_REF edges resolve `dst_id` through the DESTINATION repo's SQLite map (Phase 5 DOES
   change at the write step).** An EXTERNAL_REF edge links a source symbol in repo A to a destination
   symbol in repo B. Because integers are repo-local (point 1), the destination integer MUST come
   from **repo B's** allocator, not repo A's. Concretely, after `global_map.get(dst_usr)` yields the
   destination `repo_name` (already computed today), Phase 5 opens that repo's
   `<stage_dir>/cxg-symbols.db` (locatable: Phase 5 already holds every repo's `stage_dirs`), resolves
   `dst_usr → dst_id` there, and emits an integer-keyed EXTERNAL_REF. The *matching* is still by USR
   (point 2); only the final id resolution and the emitted edge change. This is the one place the
   earlier "Phase 5 is unchanged" framing was wrong — Phase 5's USR *matching* is unchanged, but its
   edge *emission* must resolve cross-repo integers.
5. **Cross-repo edges are two-repo-scoped.** `EdgeRecord` gains a `dst_repo_name` (today `repo_name`
   is single-valued; an EXTERNAL_REF spans two distinct repos). The sink keys the source endpoint on
   `(src_id, repo_name)` and the destination endpoint on `(dst_id, dst_repo_name)`. For intra-repo
   edges `dst_repo_name == repo_name`. This keeps S6-SC-03 satisfied (no USR strings on any edge)
   while making EXTERNAL_REF correctly addressable in the integer world.

## Alternatives considered

- **(a) Global cross-repo integer namespace (one shared SQLite / a daemon-level allocator).**
  Would let `EXTERNAL_REF` match on integers directly. Rejected: forbidden by D1; requires a
  distributed/locked allocator shared across independent `cxg-index` invocations, reintroducing the
  Phase-5 lock contention this project avoids, and breaking the "each repo gets its own independent
  database" acceptance criterion (S1-SC-03).
- **(b) Translate USR→ID only at the sink boundary; keep Parquet USR-only.** Tidier on paper (no
  schema change to staging records). Rejected: it strands S2-SC-10 (allocation is anchored to the
  parallel visit phase, the whole reason the LRU exists) and leaves `arrow.rs`/`nodes.rs`/`edges.rs`
  — explicitly listed in S4 — untouched, breaking scenario→design→test traceability that the QA gate
  (CHARTER I4) checks. The sink never runs on the parallel path, so the allocator would have no
  natural home there.
- **(c) Hash-based IDs (e.g. truncated blake3 of the USR).** No SQLite needed, deterministic across
  repos. Rejected: collisions are non-zero at graph scale and would silently merge distinct symbols
  (a C7 silent-corruption violation); 64-bit hashes are also not as compact as dense small integers
  and defeat part of the size goal.

## Consequences

- Positive: graph shrinks (USR strings removed from durable nodes/edges); per-repo allocation needs
  no cross-process coordination; Phase 5 logic is untouched and stays correct.
- Positive: re-index ID stability (C3, S7-SC-12) is a local property of one repo's SQLite file.
- Negative: the same symbol has different integers in different repos. Any future consumer that
  wants a cross-repo integer identity must resolve through USR; integers are repo-local handles only.
- Negative: staging Parquet records grow by one (nodes) / two (edges) integer columns. This is
  ephemeral storage; the durable graph still shrinks. Net size win is in the graph, measured per
  S4-SC-06 (≥30% node+edge byte reduction).
- Negative: Phase 5 must open destination-repo SQLite maps to resolve EXTERNAL_REF `dst_id` (point 4),
  and `EdgeRecord` grows a `dst_repo_name` (point 5). This is a localized change at Phase 5's edge
  emission and the sink's edge-MATCH; the USR *matching* core is untouched.
- Follow-up: read path (S5) must always resolve integers through the *correct repo's* SQLite map;
  the resolver must be repo-scoped, never global. For an EXTERNAL_REF edge endpoint, the read path
  resolves `src_id` via `repo_name`'s map and `dst_id` via `dst_repo_name`'s map.

## References

- requirements.md D1, D2; scenarios S1-SC-03, S2-SC-10, S4-SC-06; CHARTER.md (cross-repo facts)
- `src/resolve/cross_repo.rs` (`build_global_usr_map`, `check_schema_version`)
- `src/schema/nodes.rs`, `src/schema/edges.rs`, `src/schema/arrow.rs`
- ADR-3 (this run, allocator + SQLite); ADR-2 (this run, schema bump)
- cognee tags: `task:graph-symbol-ids`, `role:architect`
