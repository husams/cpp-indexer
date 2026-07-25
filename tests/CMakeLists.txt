# One doctest executable per module (design §8, D21). All S01 suites are
# hermetic: label "default" — no libclang, no network, no Python at runtime.
# env_logger_test also hosts the subprocess coverage (design §3: no separate
# exe).
set(CIDX_DEFAULT_TESTS
  pathutil_test
  json_min_test
  hashing_test
  env_logger_test
  storage_smoke_test
  storage_identity_scope_test
  storage_migration_test
  storage_profile_test
  storage_ports_test
  storage_sink_test
  storage_artifact_test
  graph_storage_test
  graph_query_test
  graph_parity_boundary_test
  query_plan_test
  catalog_test
  result_protocol_test
  analysis_runner_test
  fuzzy_match_test
  repo_test
  include_aliasing_test
  workspace_context_test
  ast_pass_test
  ui_graph_view_test
  application_services_test
  extraction_plan_test
  extraction_engine_test
)

foreach(test_name IN LISTS CIDX_DEFAULT_TESTS)
  add_executable(${test_name} ${test_name}.cpp)
  target_compile_options(${test_name} PRIVATE -Wall -Wextra)
  target_link_libraries(${test_name} PRIVATE cidx_core)
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES LABELS "default")
endforeach()

# The AST pass contract test drives a real LibTooling action through the
# statement visitor, so it needs the same Clang C++ headers as cidx_ast.
target_include_directories(ast_pass_test SYSTEM PRIVATE
  ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})

# extraction_engine_test (HSE-64) drives real clang::tooling::runToolOnCode
# parses through the ExtractionPlan engine -- same lightweight in-memory-TU
# treatment as ast_pass_test (no compile database/toolchain resolution, so it
# stays under "default" rather than the heavier "clang" label reserved for
# CLI/toolchain-resolved parses).
target_include_directories(extraction_engine_test SYSTEM PRIVATE
  ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})

add_executable(application_process_test application_process_test.cpp)
target_compile_options(application_process_test PRIVATE -Wall -Wextra)
target_link_libraries(application_process_test PRIVATE cidx_core)
add_test(NAME application_process_test
         COMMAND application_process_test $<TARGET_FILE:cidx>)
set_tests_properties(application_process_test PROPERTIES LABELS "default")

target_compile_definitions(result_protocol_test PRIVATE
  CIDX_RESULT_PROTOCOL_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/../spec/contracts/golden/result-envelope.json"
  CIDX_EVENT_PROTOCOL_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/../spec/contracts/golden/event.json"
  CIDX_ERROR_STATUS_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/../spec/contracts/golden/error-status.json"
  CIDX_ERROR_TRUNCATED_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/../spec/contracts/golden/error-truncated-envelope.json"
  CIDX_PROTOCOL_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/../spec/contracts/golden/protocol-vectors.json")

# query_plan_test pins canonical CXQ plan JSON against the cross-language
# golden shared with python/tests/test_queryplan.py (regenerate with
# CIDX_UPDATE_GOLDEN=1 -- keep both suites in sync).
target_compile_definitions(query_plan_test PRIVATE
  CIDX_CXQ_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/golden/cxq_plans.txt")

# S02: the migration suite opens committed Python-generated fixture DBs
# (copied to a temp dir first — the fixtures themselves stay read-only).
target_compile_definitions(storage_migration_test PRIVATE
  CIDX_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

# CIDX_MANIFESTS_DIR can be overridden at cmake configure time (e.g. the e2e box
# that rsyncs only cidx-cpp/).  Default: the canonical `libclang-lab/manifests`
# tree, matching the Python suite (test_astcache.py anchors `_MANIFESTS` to
# `_REPO_ROOT/libclang-lab/manifests`).  Anchoring to the literal `libclang-lab`
# segment (not worktree-relative `../../manifests`) keeps the frozen cache_key
# interchange contract identical whether built from main or a worktree.
if(NOT DEFINED CIDX_MANIFESTS_DIR)
  # Standalone layout: optional integration fixtures live at repo/manifests.
  # Callers may still point at the original lab fixtures explicitly.
  get_filename_component(_cidx_repo_root
    "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
  set(CIDX_MANIFESTS_DIR "${_cidx_repo_root}/manifests")
endif()

# A compile_commands.json cannot be committed portably: its `directory` fields
# must be ABSOLUTE, so a checked-in copy is frozen to whatever checkout generated
# it and is unusable from any other worktree / CI / clone. So the FIXTURE is a
# portable template (manifests/compile_commands.json.in, @CIDX_MANIFESTS@ token)
# and the real compile_commands.json is generated per-checkout right here, next
# to the source files it references. Every consumer (the C++ clang tests AND the
# Python suite's test_repository) reads the natural manifests/compile_commands.json
# path; the generated file is gitignored (never committed).
set(_cidx_cdb_template "${CIDX_MANIFESTS_DIR}/compile_commands.json.in")
if(EXISTS "${_cidx_cdb_template}")
  set(CIDX_MANIFESTS "${CIDX_MANIFESTS_DIR}")  # substituted for @CIDX_MANIFESTS@
  configure_file("${_cidx_cdb_template}"
    "${CIDX_MANIFESTS_DIR}/compile_commands.json" @ONLY)
endif()

# --- S03: compiledb_test — one exe, two ctest registrations -----------------
# Hermetic strip/sanitize/driver/major-regex cases run under the "default"
# label; the CompileDb::load cases (clang::tooling::JSONCompilationDatabase, the
# Clang C++ API) live in doctest suite "clang" and are registered separately
# under the "clang" label. The Clang C++ API is linked into the binary, so
# SKIP-77 is kept here only for the fixture-gap case (CIDX_MANIFESTS_DIR absent).
add_executable(compiledb_test compiledb_test.cpp)
target_compile_options(compiledb_test PRIVATE -Wall -Wextra)
target_link_libraries(compiledb_test PRIVATE cidx_core)
target_compile_definitions(compiledb_test PRIVATE
  CIDX_MANIFESTS_DIR="${CIDX_MANIFESTS_DIR}")

add_test(NAME compiledb_test COMMAND compiledb_test --test-suite-exclude=clang)
set_tests_properties(compiledb_test PROPERTIES LABELS "default")

add_test(NAME compiledb_clang_test COMMAND compiledb_test --test-suite=clang)
set_tests_properties(compiledb_clang_test PROPERTIES
  LABELS "clang"
  SKIP_RETURN_CODE 77)

# --- S04: toolchain_test — one exe, two ctest registrations ------------------
# Hermetic cases (fake-driver fixture + env injection + Toolchain test seams)
# run under "default"; the real clang major-version gnuc-cap smoke lives in
# doctest suite "clang".
# SKIP-77 is kept for the fixture-gap case only.
add_executable(toolchain_test toolchain_test.cpp)
target_compile_options(toolchain_test PRIVATE -Wall -Wextra)
target_link_libraries(toolchain_test PRIVATE cidx_core)
target_compile_definitions(toolchain_test PRIVATE
  CIDX_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

add_test(NAME toolchain_test COMMAND toolchain_test --test-suite-exclude=clang)
set_tests_properties(toolchain_test PROPERTIES LABELS "default")

add_test(NAME toolchain_clang_test COMMAND toolchain_test --test-suite=clang)
set_tests_properties(toolchain_clang_test PROPERTIES
  LABELS "clang"
  SKIP_RETURN_CODE 77)

# ast_test removed in the LibTooling cutover: it exercised the retired libclang
# cursor-walk engine (AstIndexer). Real-parse indexing is now driven by the
# LibTooling engine and covered via cli_test + the index golden gate
# (index_golden_test, below).

# --- astgraph_test — per-TU AST->SQLite graph dumper (src/astgraph/) ----------
# Every case performs a real parse (via the Clang C++ API) on temp-dir sources:
# single "clang" registration, SKIP-77 when the parse environment is unavailable.
add_executable(astgraph_test astgraph_test.cpp)
target_compile_options(astgraph_test PRIVATE -Wall -Wextra)
target_link_libraries(astgraph_test PRIVATE cidx_core)
if(CIDX_ASTGRAPH_SOUFFLE_ENABLED)
  target_sources(astgraph_test PRIVATE
    $<TARGET_OBJECTS:cidx_astgraph_souffle_rules>)
endif()

add_test(NAME astgraph_clang_test COMMAND astgraph_test --test-suite=clang)
set_tests_properties(astgraph_clang_test PROPERTIES
  LABELS "clang"
  SKIP_RETURN_CODE 77)

# --- S07: cli_test — one exe, two ctest registrations -------------------------
# Args-grammar, format, add-source, and query-golden cases are hermetic
# ("default" label); the cmd_import cases need CompileDb::load
# (clang::tooling::JSONCompilationDatabase) and live in doctest suite "clang".
# SKIP-77 kept for fixture-gap only.
add_executable(cli_test cli_test.cpp)
target_compile_options(cli_test PRIVATE -Wall -Wextra)
target_link_libraries(cli_test PRIVATE cidx_core)
target_compile_definitions(cli_test PRIVATE
  CIDX_MANIFESTS_DIR="${CIDX_MANIFESTS_DIR}")

add_test(NAME cli_test COMMAND cli_test --test-suite-exclude=clang)
set_tests_properties(cli_test PROPERTIES LABELS "default")

add_test(NAME cli_clang_test COMMAND cli_test --test-suite=clang)
set_tests_properties(cli_clang_test PROPERTIES
  LABELS "clang"
  SKIP_RETURN_CODE 77)

# --- diff_test — cidx-diff plumbing (src/diff/) -------------------------------
# Hermetic cases (read-only Storage mode, config resolution/delta, CLI11
# grammar via diff::run) live in suite "diff" under the "default" label; the
# real-parse fixture matrix (docs/diff.md) lives in doctest suite "clang",
# registered separately. SKIP-77 reserved for a missing parse environment.
add_executable(diff_test diff_test.cpp)
target_compile_options(diff_test PRIVATE -Wall -Wextra)
target_link_libraries(diff_test PRIVATE cidx_core)

add_test(NAME diff_test COMMAND diff_test --test-suite-exclude=clang)
set_tests_properties(diff_test PROPERTIES LABELS "default")

add_test(NAME diff_clang_test COMMAND diff_test --test-suite=clang)
set_tests_properties(diff_clang_test PROPERTIES
  LABELS "clang"
  SKIP_RETURN_CODE 77)

# ast_query_test / astcache_test removed in the LibTooling cutover: the
# `cidx ast` command and the AST/PCH cache they exercised were deleted.

# --- include_hygiene_test — the `cidx include` safety bar --------------------
# The executable spec for planning/cidx-include-hygiene. Mostly asserts that
# removals do NOT happen (X-macros, macro-only providers, transitive providers,
# conditional branches): confidently deleting a working #include is this
# feature's dangerous failure, not a crash. Real parses -> label "clang".
add_executable(include_hygiene_test include_hygiene_test.cpp)
target_compile_options(include_hygiene_test PRIVATE -Wall -Wextra)
target_link_libraries(include_hygiene_test PRIVATE cidx_core)

add_test(NAME include_hygiene_test
         COMMAND include_hygiene_test --test-suite=clang)
set_tests_properties(include_hygiene_test PROPERTIES
  LABELS "clang"
  SKIP_RETURN_CODE 77)

# --- ast_visitor_test — focused visitor fixtures (refactoring.md Phase 0) -----
# Pins provenance/template-arg semantics for the src/ast refactor and carries
# the correction fixtures (doctest should_fail) that Phases 2-3 make true.
# Every case is a real parse via the CLI -> label "clang".
add_executable(ast_visitor_test ast_visitor_test.cpp)
target_compile_options(ast_visitor_test PRIVATE -Wall -Wextra)
target_link_libraries(ast_visitor_test PRIVATE cidx_core)

add_test(NAME ast_visitor_test COMMAND ast_visitor_test --test-suite=clang)
set_tests_properties(ast_visitor_test PROPERTIES
  LABELS "clang"
  SKIP_RETURN_CODE 77)

# --- S08: index golden gate — the repository-wide indexing oracle -------------
# Replaces the RETIRED byte-identical Python(libclang)-vs-C++ parity gate. The
# old gate diffed a full `sqlite3 .dump`, which the LibTooling engine can no
# longer match: its AST-traversal order differs from libclang's, so the
# surrogate-key (edge.id / edge_site.edge_id) emission order — and thus a
# byte-strict dump — diverges even when the SEMANTIC content is identical.
#
# index_golden_test indexes a fixed fixture and compares NORMALIZED, ORDERED
# projections of the seven Layer-0 tables (file, symbol, decl_site, edge,
# edge_site, call_arg, template_arg) against tests/golden/index_layer0.txt.
# Resolving surrogate keys to USR/edge-kind/basename and ORDERing by them makes
# the oracle invariant to traversal order while still catching any semantic
# regression. Real parse -> label "clang" (SKIP-77 reserved; regenerate the
# golden with CIDX_UPDATE_GOLDEN=1 after an intentional change or Clang bump).
add_executable(index_golden_test index_golden_test.cpp)
target_compile_options(index_golden_test PRIVATE -Wall -Wextra)
target_link_libraries(index_golden_test PRIVATE cidx_core)
target_compile_definitions(index_golden_test PRIVATE
  CIDX_GOLDEN_FILE="${CMAKE_CURRENT_SOURCE_DIR}/golden/index_layer0.txt")

add_test(NAME index_golden_test COMMAND index_golden_test --test-suite=clang)
set_tests_properties(index_golden_test PROPERTIES
  LABELS "clang"
  SKIP_RETURN_CODE 77)
