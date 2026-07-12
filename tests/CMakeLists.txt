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
  storage_migration_test
  graph_storage_test
  graph_query_test
  graph_parity_boundary_test
  fuzzy_match_test
  repo_test
  include_aliasing_test
)

foreach(test_name IN LISTS CIDX_DEFAULT_TESTS)
  add_executable(${test_name} ${test_name}.cpp)
  target_compile_options(${test_name} PRIVATE -Wall -Wextra)
  target_link_libraries(${test_name} PRIVATE cidx_core)
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES LABELS "default")
endforeach()

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

# --- S03: compiledb_test — one exe, two ctest registrations -----------------
# Hermetic strip/sanitize/driver/major-regex cases run under the "default"
# label; the CompileDb::load cases live in doctest suite "clang" and are
# registered separately under the "clang" label.
# A1 (Amendment A1): libclang is linked at build time — SKIP-77 is kept here
# only for the fixture-gap case (CIDX_MANIFESTS_DIR absent), not for "no dylib".
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
# run under "default"; the real-LibClang::major() gnuc-cap smoke lives in
# doctest suite "clang".
# A1: SKIP-77 kept for fixture-gap only (no "no dylib" path anymore).
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
# LibTooling engine and covered via cli_test + the index parity gate.

# --- astgraph_test — per-TU AST->SQLite graph dumper (src/astgraph/) ----------
# Every case performs a real parse on temp-dir sources: single "clang"
# registration, SKIP-77 when no libclang loads.
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
# (CXCompilationDatabase), live in doctest suite "clang".
# A1: SKIP-77 kept for fixture-gap only.
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

# ast_query_test / astcache_test removed in the LibTooling cutover: the
# `cidx ast` command and the AST/PCH cache they exercised were deleted.

# --- S08: parity gate — RETIRED in the LibTooling cutover (Phase 4).
# The byte-identical Python(libclang)-vs-C++ dual-implementation contract is
# dropped: C++ now indexes via the LibTooling engine, whose AST-traversal order
# differs from libclang's, so the surrogate-key (edge.id / edge_site.edge_id)
# emission order — and thus a byte-strict `sqlite3 .dump` diff — can no longer
# match Python's libclang output. Semantic parity (identical symbol rows and
# identical edge/edge_site SETS) is preserved and was verified directly during
# the cutover. Python is being narrowed to storage/query only (separate phase),
# after which a Python indexer no longer exists to diff against.
