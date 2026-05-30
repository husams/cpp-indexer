# Architect log — pcm-support

Stage 3 of 8. Read: CHARTER.md, requirements.md (S1–S5), scenarios.md (Gherkin + 3 open
questions), source (modules_cpp20.rs, parallel.rs, pipeline/mod.rs, bin/index.rs). Loaded
rust-conventions.

## Deliverables
- design.md
- adr-1.md (dispatch + routing) — accepted
- adr-2.md (is_module_tu detection) — accepted
- adr-3.md (loud diagnostic + failure contract) — accepted

## Key findings / decisions
1. **Lead finding (advisor-confirmed):** existing `parse_module_tu` is itself an Issue-0001
   trap. libclang returns `Ok(tu)` + Fatal diagnostic on bad `.pcm`; current code lumps
   Error|Fatal, logs warn, WRITES partial nodes, returns Ok(true) → counted as tu_partial,
   exit 0. ADR-3 routes load failures to tu_error before any write.
2. Settled scenarios open-question #3 (exit-code OR summary): NOT a real disjunction.
   `failed_tu_count` already feeds both `closing_summary()` and `FailOnTuError::exit_code()`.
   Feed it → both branches satisfied. `FailOnTuError::default() == Ratio(1.0)`, so mixed-fixture
   tests must assert `failed_tu_count > 0` or pass `--fail-on-tu-error 0.0` (default exits 0).
3. Settled scenarios open-question #1 partially: do NOT rely on a distinct libclang error code.
   Pre-parse stat (attributable, deterministic for `-fmodule-file=`) + post-parse Fatal-severity
   gate. Developer must empirically verify missing/corrupt .pcm surfaces as Fatal.
4. Settled scenarios open-question #2: fixture pattern is
   tests/integration/{symbol_id_integration,parallel_phase1,cli_fail_on_tu_error}.rs.
5. Settled the `assumed` flag-routing question (scenarios.md:19): `is_module_tu` detects by
   flag-presence INDEPENDENT of language standard; drop the `-std=c++20` co-requirement; add
   `-fmodule-file=` and `-fprebuilt-module-path` prefix matching. Clang header-modules predate
   C++20.
6. Hazard flagged: do not force `-std=c++20` on PCM-consuming `.cpp` TUs (current
   modules_cpp20.rs:215-217 unconditionally appends it — wrong for C++17 consumers, breaks
   S2-AC3). Gate force-append on module-interface extension only.

7. AC-conformance fixes (advisor round 2): (a) S1-AC4 requires WARNING level but probe emits
   info! (modules_cpp20.rs:119-144) — ADR-1 now instructs developer to raise it to warn!.
   (b) `-fmodule-file=` has both `name=path` and bare `path` forms — ADR-3 stat-extraction now
   handles both so S3-AC1 missing-file detection is deterministic for both.

## schemaBump
false. MODULE node + module_interface attr exist; module imports reuse EdgeKind::Includes
(ADR-8 scope-limit). Any new ParallelStats field is an in-process counter, not graph schema v5/v6.

## Status
clear — all 3 ADRs accepted.
