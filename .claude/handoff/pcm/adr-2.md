# ADR-2: PCM detection in is_module_tu() — flag-presence, standard-independent

Status: accepted

Context:
`is_module_tu()` (`modules_cpp20.rs:168`) currently returns `true` only for a module-interface
file extension (`.cppm`/`.ixx`/`.mxx`) OR for the conjunction
(`-fmodules` AND `-std=c++20`). This fails every PCM acceptance criterion:
- `-fprebuilt-module-path=<dir>` alone → `false` (fails S1-AC1 / scenarios edge line 30).
- `-fmodule-file=name=path` alone → `false` (fails S1-AC2 / Gherkin line 54).
- `-fmodules` alone (no `-std=c++20`) → `false` (fails Gherkin line 60).

The scenarios flag as `assumed` whether `-fmodules` alone suffices or requires a C++20
language-mode flag co-present (scenarios.md:19). This ADR settles that open assumption.

Clang header-modules and PCM consumption are **not** tied to C++20 — they work under C++11/14/17.
Coupling detection to `-std=c++20` is therefore incorrect.

Decision:
`is_module_tu()` returns `true` when ANY of the following holds (logical OR), **independent of
the language standard**:
1. file extension is `cppm`, `ixx`, or `mxx` (unchanged — module-interface units), OR
2. any arg equals `-fmodules`, OR
3. any arg starts with `-fmodule-file=`, OR
4. any arg starts with `-fprebuilt-module-path`.

Drop the `-std=c++20` co-requirement entirely. Prefix matching (`starts_with`) handles the
`name=path` and `=dir` value forms. A TU carrying both `-fmodule-file=` and
`-fprebuilt-module-path` is detected once (single `true`), not double-counted
(scenarios edge line 31).

Alternatives considered:
- a) Keep `-fmodules && -std=c++20` and add a separate `is_pcm_tu()` predicate — rejected:
  two predicates with overlapping routing invites a dispatch gap; one router predicate is
  simpler and the parser path is the same.
- b) Require `-std=c++20` co-present for the `-fmodules` case only — rejected: header-modules
  are valid pre-C++20; this would silently skip legitimate PCM TUs (a quiet-failure regression,
  the opposite of this task's goal).

Consequences:
- Positive: satisfies S1-AC1..AC3 and Gherkin lines 48-70; resolves the scenarios `assumed`
  open question explicitly (answer: `-fmodules` alone is sufficient, no `-std` co-requirement).
- Negative: a TU that carries `-fmodules` defensively but consumes no modules will route to the
  module parser. Harmless — `parse_module_tu` produces the same nodes for non-module decls, and
  there is no `.pcm` to fail on, so no false failure is raised.
- Follow-up: extend the existing unit tests `is_module_tu_by_args` /
  `is_module_tu_by_extension` (`modules_cpp20.rs:544-561`) with the three new flag cases.

References:
src/visit/modules_cpp20.rs:168-179,544-561; scenarios.md:19,30,31; requirements.md S1;
.claude/handoff/pcm/design.md §4; cognee task:pcm-support role:architect
