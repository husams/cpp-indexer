# ADR-8: C++20 modules — best-effort on libclang 18 with runtime capability probe

Status: accepted
Date: 2026-05-17
Resolves: PRD Q3, AC-M5-7..9

## Context

PRD Q3 asks whether to implement C++20 modules support now (on libclang 18) or defer until libclang 19+. libclang 18 partially supports C++20 module interfaces (`.cppm`, `.pcm`); behaviour varies by distro build and CMake configuration. Requirements AC-M5-7..9:

- AC-M5-7: when libclang 18 supports it, `.cppm` files in the TU queue are processed and emit nodes/edges for exported declarations.
- AC-M5-8: when not supported, the indexer logs a warning, **skips that TU, and continues** (no abort).
- AC-M5-9: `cxg-index --version` notes the runtime limitation when modules are unavailable.

## Decision

Implement C++20 module support on libclang 18 as a **best-effort** path with a runtime capability probe and graceful fallback. Concretely:

1. **Runtime capability probe**, run once at indexer startup:
   - Construct a `clang::Index`, parse a minimal `.cppm` test fixture embedded in the binary (`include_bytes!("modules_probe.cppm")`), invoke `clang::TranslationUnit::parse` with `-std=c++20 -fmodules`.
   - If parse succeeds AND the resulting TU exposes `EntityKind::ModuleImportDecl` cursors → `cpp20_modules_capable = true`.
   - Else → `cpp20_modules_capable = false`; emit one INFO log line at startup naming the missing capability.

2. **TU dispatch** (Phase 1):
   - For TUs with extension `.cppm`, `.ixx`, `.mxx` or `compile_commands.json` `args` containing `-fmodules` / `-std=c++20`:
     - If `cpp20_modules_capable` → process via `visit::modules_cpp20::parse_module_tu()`. Emit nodes for `MODULE` (as a module interface unit, distinct attribute `module_interface: true`), `NAMESPACE`, `CLASS`, `FUNCTION` etc. for exported decls. Emit a `MODULE_EXPORTS` edge (added to schema if not present; if added, bump SCHEMA_VERSION per ADR-9).
     - If not capable → log WARN `cxg_module_skipped` with file path and reason, increment `cxg_modules_skipped_total`, mark TU as done in the manifest with `partial: true`. AC-M5-8.

3. **Version output** (AC-M5-9):
   - `cxg-index --version` prints, after the version line: `libclang: <version>` and, when `cpp20_modules_capable == false`, an additional line `C++20 modules: UNAVAILABLE (libclang build lacks module support; .cppm/.ixx TUs will be skipped)`.

4. **Scope limit for v1**:
   - Module **interface** units are processed (exported decls); module **implementation** units are processed as regular TUs.
   - Module-private fragments are not separately modelled.
   - `import` directives produce `INCLUDES`-equivalent edges to the imported `MODULE` node (distinguished by `module_interface: true` on the target). No new edge kind to keep schema churn bounded.

## Alternatives considered

- **Defer entirely to libclang 19+**: rejected. The Streamlit agent already runs on developer machines today with libclang 18; deferring means M5's Chromium fixture (which has some module-using subtrees) silently loses coverage. Best-effort + graceful skip gives partial value now and zero ops cost.
- **Hard-require libclang 19**: rejected. Forces every user to upgrade their toolchain for v1 GA; conflicts with PRD's libclang-18 baseline.
- **Always attempt to parse `.cppm`, abort on failure**: rejected by AC-M5-8 (must continue, not abort).
- **Auto-detect per-TU capability**: rejected as too expensive. Single startup probe is sufficient: libclang's modules support is build-time fixed, not per-TU.

## Consequences

Positive:
- Modern C++ codebases get partial coverage in v1.
- Older / minimal libclang builds still work; the indexer degrades gracefully.
- Single startup probe; no per-TU overhead.

Negative:
- "Best-effort" support means some module-using AC tests must be skipped or marked conditional in CI when the runner's libclang lacks support.
- Schema may gain `module_interface: true` attribute on `MODULE` node, which is a forward-compatible change (additive).

Follow-ups:
- After v1, evaluate libclang 19's module support; if it stabilises, promote modules from best-effort to fully supported and remove the capability probe.
- Add `cpp20_modules_capable` to `GET /v1/status` so ops can audit.

Revisit if: a user repo's module usage is broken by the best-effort handling, or libclang 19 ships in the team's distro of choice.

## References

- requirements.md AC-M5-7, AC-M5-8, AC-M5-9
- PRD v1.1 §7 compatibility C++20, Q3
- engineering plan v1.1 §M5, §Risk register (C++20 modules)
- Cognee tags: `task:cpp-indexer role:architect`
