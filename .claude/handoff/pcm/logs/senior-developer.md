# senior-developer log — pcm-support (plan mode)

Stage 4. Inputs read: requirements.md, scenarios.md, design.md, adr-1/2/3 (all Status: accepted).
Verified against source: modules_cpp20.rs (probe, is_module_tu, parse_module_tu, warn_and_skip),
parallel.rs dispatch closure (163-179) + stats mapping (181-211), mod.rs phase-1 entry,
shallow.rs VisitOptions/visit_tu_with_index, tests/integration/ inventory.

Deliverable: plan.md — 5 stories (S1 detect+warn, S2 route, S3 loud-fail, S4 integ test, S5 docs).
Every story has exit-criteria commands with the AGENTS.md libclang env baked in
(LIBCLANG_PATH + DYLD_LIBRARY_PATH = /Library/Developer/CommandLineTools/usr/lib). No
MISSING_EXIT_CRITERIA — S5 (docs) uses grep-string checks, not prose.

Key decisions settled in plan:
- Single dispatch site confirmed: mod.rs:200 is the cache-hash loop (no parse); only live parse
  path is run_phase1_parallel -> parallel.rs:176. S2 routing at that one site is complete; no
  second silent-bypass path. (Closed advisor's flag #3.)
- parallel-safe honest count: 1. S1->S2->S3 chained + share modules_cpp20.rs -> false. S5 (docs,
  disjoint file) parallel-safe with S4; both run after S1-S3. Recommend sequential shared-tree
  for S1-S3 per recorded dev-team lesson.
- Failure-signaling OR resolved to one path: all PCM failures -> Err(Error::Clang) -> tu_error ->
  failed_tu_count (drives both summary and exit). Default FailOnTuError=Ratio(1.0) => mixed
  fixture exits 0; tests assert failed_tu_count>0, not exit code.

Hazards forwarded to developer: gate -std=c++20 force-append on interface ext (S2); empirically
confirm corrupt .pcm => Severity::Fatal on libclang18+ (S3); schema.txt must stay untouched
(schemaBump:false).

status: clear; stories: 5 (parallel-safe:1).
