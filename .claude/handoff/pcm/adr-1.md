# ADR-1: Dispatch point and routing of module/PCM TUs

Status: accepted

Context:
The C++20/PCM module parser (`parse_module_tu`, `modules_cpp20.rs:203`) is fully written but
not wired into the pipeline. The parallel Phase-1 worker closure unconditionally calls
`visit_tu_with_index` at `src/pipeline/parallel.rs:176`. We must route module/PCM TUs to the
module parser without regressing standard TUs (S2-AC1, S2-AC2) and without changing the
existing `ParallelStats` mapping (`parallel.rs:181-211`), which already maps
`Err(Error::Clang)` → `tu_error` and `Ok(had_errors)` → ok/partial.

Decision:
1. Branch at the dispatch site (`parallel.rs:176`, inside the existing `catch_unwind` +
   `with_thread_index` block) on `is_module_tu(&entry.file, &filtered_args)`:
   - `false` → call `visit_tu_with_index` as today (no regression).
   - `true` → enter the module path: probe gate (ADR-3) → counted skip or `parse_module_tu`.
2. Reuse `probe_cpp20_support()` as the process-wide capability gate. It is OnceLock-cached
   (`modules_cpp20.rs:67`), so calling it per TU is cheap and idempotent. The probe is a
   *proxy* (it tests interface-unit parsing, not `.pcm` consumption); fine-grained `.pcm`
   failures are handled by ADR-3, not the probe.
   **S1-AC4 fix:** the probe's "UNAVAILABLE" lines are currently `info!`
   (`modules_cpp20.rs:119-124,139-144`), but S1-AC4 / Gherkin line 77 require **WARNING** level.
   The developer MUST raise the process-wide "C++20 modules: UNAVAILABLE" line from `info!` to
   `warn!` (the per-TU `warn_and_skip` is already `warn!` and is correct).
3. Preserve the TU's original `-std` for PCM-*consuming* `.cpp` TUs. `parse_module_tu`
   force-appends `-std=c++20` (`modules_cpp20.rs:215-217`); that is correct for
   `.cppm`/`.ixx`/`.mxx` module-interface units but wrong for a C++17 `.cpp` consuming a `.pcm`
   (it changes language semantics and risks spurious "module out of date" Fatals, breaking
   S2-AC3). Gate the force-append on a module-interface extension; for flag-only PCM consumers
   pass the TU's args through unchanged.

Alternatives considered:
- a) Branch inside `visit_tu_inner` rather than at the dispatch site — rejected: buries
  routing in the standard visitor, mixes two parse strategies in one function, and obscures
  the `tu_error` accounting that S2/S3 depend on.
- b) Always force `-std=c++20` for any module/PCM TU (keep current behavior) — rejected:
  breaks S2-AC3 for C++17/14 PCM consumers; see Decision §3.
- c) Make the probe per-TU rather than process-wide (parse the actual `.pcm`) — rejected as
  the *primary* gate: too expensive, and the real safety net is the ADR-3 stat+Fatal check
  which is per-TU already. The proxy probe only short-circuits an obviously unsupported host.

Consequences:
- Positive: minimal, localized change; standard-TU path untouched; existing stats machinery
  reused; honest best-effort posture documented.
- Negative: the probe being a proxy means a libclang build that parses interface units but
  mishandles `.pcm` consumption will pass the gate and rely entirely on ADR-3 to fail loudly.
  Acceptable — ADR-3 is the real guard.
- Follow-up: developer must add the extension check that gates the `-std=c++20` force-append.

References:
src/pipeline/parallel.rs:163-179; src/visit/modules_cpp20.rs:66,168,203,215-217;
.claude/handoff/pcm/design.md §5,§9; cognee task:pcm-support role:architect
