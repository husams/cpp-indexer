# cidx-cpp — C++ port of the cidx semantic symbol indexer (spec/02-design.md).
# A2 (Amendment A2 — spec §13): C++23; build hosts are AppleClang 15+ / gcc 13+.
cmake_minimum_required(VERSION 3.16)
project(cidx LANGUAGES C CXX)

# Default to Release when the user has not specified a build type.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING
      "Build type (Release, Debug, RelWithDebInfo, MinSizeRel)" FORCE)
  message(STATUS "cidx: CMAKE_BUILD_TYPE defaulted to Release")
endif()

# CIDX_STATIC: static-link SQLite3 ONLY, so the binary needs no libsqlite3 at
# runtime. Scope is deliberately narrow after the libclang cutover:
#   * SQLite3 is linked from its static archive (libsqlite3.a) when one is found;
#     it falls back to the shared form (with a WARNING) so a static configure
#     never hard-fails.
#   * The Clang C++ API (clang-cpp + libLLVM) stays DYNAMIC on every platform:
#     cidx_core links it and it uses the system libstdc++, so a second static
#     libstdc++ would put two C++ runtimes in one process and corrupt
#     std::string/exceptions across the LLVM boundary. glibc likewise stays
#     dynamic (a fully static glibc binary breaks NSS/dlopen).
#   * The old static-libclang (C API) contract is GONE: the libclang C API was
#     removed in the LibTooling cutover, so there is no libclang.a to build or
#     link and no CIDX_LIBCLANG_STATIC knob.
# To run a CIDX_STATIC binary the host still needs the shared Clang/LLVM libs
# (e.g. RHEL: `dnf install clang-libs llvm-libs`).
option(CIDX_STATIC "Static-link SQLite3 (Clang/LLVM stay dynamic)" OFF)

# A2.1: C++23 standard; minimum compilers (A2.1 guard).
# Build hosts: macOS AppleClang (dev) and Ubuntu 24.04 gcc 13 (e2e).
# g++ 8.5 cross toolchains are *indexing targets*, not the cidx build host.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# clang-tidy is opt-in for normal builds because a full analysis is
# intentionally more expensive than compilation.  It consumes the checked-in
# .clang-tidy files and the exact C++23 compile commands emitted by CMake.
option(CIDX_ENABLE_CLANG_TIDY
  "Run clang-tidy while compiling project C++ sources" OFF)
option(CIDX_CLANG_TIDY_WARNINGS_AS_ERRORS
  "Treat clang-tidy findings as build errors" OFF)

if(CIDX_ENABLE_CLANG_TIDY)
  find_program(CIDX_CLANG_TIDY_EXECUTABLE
    NAMES clang-tidy clang-tidy-22 clang-tidy-21 clang-tidy-20
          clang-tidy-19 clang-tidy-18 clang-tidy-17 clang-tidy-16
    HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin
    DOC "clang-tidy executable used for CIDX_ENABLE_CLANG_TIDY")
  if(NOT CIDX_CLANG_TIDY_EXECUTABLE)
    message(FATAL_ERROR
      "CIDX_ENABLE_CLANG_TIDY=ON, but clang-tidy was not found")
  endif()

  set(_cidx_clang_tidy "${CIDX_CLANG_TIDY_EXECUTABLE}" "--quiet")
  if(CIDX_CLANG_TIDY_WARNINGS_AS_ERRORS)
    list(APPEND _cidx_clang_tidy "--warnings-as-errors=*")
  endif()

  # Homebrew clang-tidy does not infer the active macOS SDK when it analyzes
  # compile commands produced by AppleClang.
  if(APPLE)
    find_program(CIDX_XCRUN_EXECUTABLE NAMES xcrun)
    if(CIDX_XCRUN_EXECUTABLE)
      execute_process(
        COMMAND "${CIDX_XCRUN_EXECUTABLE}" --show-sdk-path
        OUTPUT_VARIABLE _cidx_macos_sdk
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _cidx_xcrun_rc
        ERROR_QUIET)
      if(_cidx_xcrun_rc EQUAL 0 AND IS_DIRECTORY "${_cidx_macos_sdk}")
        list(APPEND _cidx_clang_tidy
          "--extra-arg-before=-isysroot"
          "--extra-arg-before=${_cidx_macos_sdk}")
      endif()
    endif()
  endif()

  set(CMAKE_CXX_CLANG_TIDY "${_cidx_clang_tidy}")
  message(STATUS "cidx: clang-tidy = ${CIDX_CLANG_TIDY_EXECUTABLE}")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
  message(FATAL_ERROR
    "cidx requires gcc >= 13 for C++23 (found gcc ${CMAKE_CXX_COMPILER_VERSION}). "
    "g++ 8.5 cross toolchains are *indexing targets* only; build cidx with gcc 13+.")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
  message(FATAL_ERROR
    "cidx requires clang >= 16 for C++23 (found clang ${CMAKE_CXX_COMPILER_VERSION}).")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS 15)
  message(FATAL_ERROR
    "cidx requires AppleClang >= 15 for C++23 (found AppleClang ${CMAKE_CXX_COMPILER_VERSION}).")
endif()

message(STATUS "cidx: compiler ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}, C++${CMAKE_CXX_STANDARD}")

# S02: storage layer sits on the system SQLite3 (>= 3.37 enforced at runtime).
find_package(SQLite3 REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Every language consumer is generated from catalogs/core.json.  Keep the
# checked-in artifacts reproducible and make drift fail as part of the normal
# build rather than at release time.
add_custom_target(cidx_catalog_check ALL
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_catalogs.py" --check
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  COMMENT "Checking generated semantic catalog contracts"
  VERBATIM)

# Default: the shared SQLite3 imported target. Under CIDX_STATIC, swap in the
# static archive when one is on disk (the sqlite amalgamation pulls in
# dl/pthread/m; libSystem covers those on macOS so they are Linux-only).
set(_cidx_sqlite_link SQLite::SQLite3)
if(CIDX_STATIC)
  find_library(CIDX_SQLITE3_STATIC NAMES libsqlite3.a
    HINTS /opt/homebrew/opt/sqlite/lib /usr/local/opt/sqlite/lib
          /usr/lib /usr/lib64 /usr/local/lib
    DOC "Static SQLite3 archive for CIDX_STATIC")
  if(CIDX_SQLITE3_STATIC)
    add_library(cidx_sqlite_static STATIC IMPORTED)
    set_target_properties(cidx_sqlite_static PROPERTIES
      IMPORTED_LOCATION "${CIDX_SQLITE3_STATIC}"
      INTERFACE_INCLUDE_DIRECTORIES "${SQLite3_INCLUDE_DIRS}")
    if(NOT APPLE)
      target_link_libraries(cidx_sqlite_static
        INTERFACE ${CMAKE_DL_LIBS} pthread m)
    endif()
    set(_cidx_sqlite_link cidx_sqlite_static)
    message(STATUS "cidx[static]: SQLite3 (static) = ${CIDX_SQLITE3_STATIC}")
  else()
    message(WARNING "cidx[static]: no libsqlite3.a found — using shared SQLite3")
  endif()
endif()

# libclang (clang-c C API) REMOVED in the LibTooling cutover: cidx indexes via
# the Clang C++ API (clang-cpp + libLLVM, found by find_package below). No
# libclang discovery/link remains.

# Core library: util + storage + toolchain + compiledb + cli + vendored MD5/SHA1.
add_library(cidx_core STATIC
  src/application/agent_tools.cpp
  src/application/clean_rebuild.cpp
  src/application/registry.cpp
  src/application/services.cpp
  src/application/conformance_recorder.cpp
  src/application/conformance_schema.cpp
  src/util/logger.cpp
  src/util/env.cpp
  src/util/subprocess.cpp
  src/util/hashing.cpp
  src/util/artifact_io.cpp
  src/util/json_min.cpp
  src/util/pathutil.cpp
  src/util/repo.cpp
  src/util/files.cpp
  src/util/front_end_reuse.cpp
  src/profile/index_profile.cpp
  src/storage/sqlite.cpp
  src/storage/database_verification.cpp
  src/storage/fact_batch_writer.cpp
  src/storage/sqlite_adapters.cpp
  src/storage/storage.cpp
  src/storage/transforms.cpp
  src/storage/artifacts.cpp
  src/storage/tu_dependency_planner.cpp
  src/storage/tu_fact_cache.cpp
  src/storage/storage_definitions.cpp
  src/storage/storage_entity_rollup.cpp
  src/storage/storage_files.cpp
  src/storage/storage_include.cpp
  src/storage/storage_migrate.cpp
  src/storage/storage_query.cpp
  src/storage/storage_repo.cpp
  src/storage/storage_schema.cpp
  src/storage/storage_symbols.cpp
  src/storage/storage_applicability.cpp
  src/storage/storage_types.cpp
  src/storage/workspace_adapter.cpp
  src/toolchain/toolchain.cpp
  src/cli/args.cpp
  src/cli/application_adapter.cpp
  src/cli/format.cpp
  src/cli/json_out.cpp
  src/cli/kind_names.cpp
  src/cli/commands.cpp
  src/cli/commands_delete.cpp
  src/cli/commands_edit.cpp
  src/cli/commands_graph.cpp
  src/cli/commands_ingest.cpp
  src/cli/commands_repo.cpp
  src/cli/commands_query.cpp
  src/cli/commands_show.cpp
  src/cli/analyze.cpp
  src/analysis/facts.cpp
  src/analysis/runner.cpp
  src/graph/query.cpp
  src/graph/emit.cpp
  src/query/plan.cpp
  src/query/result_protocol.cpp
  src/query/cxq.cpp
  src/query/exec.cpp
  src/include_hygiene/graph.cpp
  src/include_hygiene/analysis.cpp
  src/include_hygiene/plan.cpp
  src/util/json_read.cpp
  src/cli/include_cmd.cpp
  src/cli/commands_ui.cpp
  src/ui/graph_view.cpp
  src/ui/assets.cpp
  src/ui/server.cpp
  src/diff/target.cpp
  src/diff/compare.cpp
  src/diff/report.cpp
  src/diff/driver.cpp
  third_party/md5/md5.c
  third_party/sha1/sha1.c
)
add_dependencies(cidx_core cidx_catalog_check)

# --- LLVM/Clang discovery ----------------------------------------------------
# find_package(... CONFIG) only searches CMake's default prefixes, which do NOT
# include a keg-only Homebrew LLVM (/opt/homebrew/opt/llvm) or a distro that
# hides LLVM under a versioned prefix -- so a plain `cmake -S . -B build` fails
# to find LLVMConfig.cmake even when clang is installed. Seed LLVM_DIR/Clang_DIR
# from `llvm-config --cmakedir` (with Homebrew hints) UNLESS the caller already
# set them: an explicit -DLLVM_DIR=/-DClang_DIR= always wins and short-circuits
# this probe.
if(NOT LLVM_DIR OR NOT Clang_DIR)
  find_program(CIDX_LLVM_CONFIG
    NAMES llvm-config
          llvm-config-22 llvm-config-21 llvm-config-20
          llvm-config-19 llvm-config-18
    HINTS /opt/homebrew/opt/llvm/bin /opt/homebrew/opt/llvm@22/bin
          /usr/local/opt/llvm/bin /opt/homebrew/bin /usr/local/bin
    DOC "llvm-config used to locate LLVMConfig.cmake / ClangConfig.cmake")
  if(CIDX_LLVM_CONFIG)
    execute_process(
      COMMAND "${CIDX_LLVM_CONFIG}" --cmakedir
      OUTPUT_VARIABLE _cidx_llvm_cmakedir
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _cidx_llvm_config_rc
      ERROR_QUIET)
    if(_cidx_llvm_config_rc EQUAL 0 AND IS_DIRECTORY "${_cidx_llvm_cmakedir}")
      if(NOT LLVM_DIR)
        set(LLVM_DIR "${_cidx_llvm_cmakedir}" CACHE PATH
            "LLVMConfig.cmake dir (from llvm-config --cmakedir)" FORCE)
      endif()
      # ClangConfig.cmake sits beside LLVMConfig.cmake: <cmakedir>/../clang.
      if(NOT Clang_DIR)
        get_filename_component(_cidx_cmake_root "${_cidx_llvm_cmakedir}" DIRECTORY)
        if(IS_DIRECTORY "${_cidx_cmake_root}/clang")
          set(Clang_DIR "${_cidx_cmake_root}/clang" CACHE PATH
              "ClangConfig.cmake dir (beside LLVMConfig.cmake)" FORCE)
        endif()
      endif()
      message(STATUS
        "cidx: LLVM/Clang discovered via ${CIDX_LLVM_CONFIG} (LLVM_DIR=${LLVM_DIR})")
    endif()
  endif()
endif()

# --- LibTooling indexing engine (cidx_ast) ------------------------------------
# The LT engine uses the Clang C++ API. Compiled as its own object library so
# its LLVM/Clang headers are added with -isystem (a plain -I would break
# gcc's #include_next <stdlib.h> in <cstdlib>) and it can carry -fno-rtti
# independently. lt_engine.hpp is clang-free, so cidx_core links the objects
# but never sees the Clang C++ headers.
find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)
add_library(cidx_ast OBJECT
  src/ast/front_end_reuse.cpp
  src/ast/kind_map.cpp src/ast/usr.cpp src/ast/location.cpp
  src/ast/names.cpp src/ast/decl_flags.cpp
  src/ast/symbol_extractor.cpp src/ast/symbol_visitor.cpp
  src/ast/mint_builder.cpp
  src/ast/template_argument_encoder.cpp
  src/ast/declaration_edge_visitor.cpp src/ast/type_use.cpp
  src/ast/type_graph.cpp
  src/ast/value_provenance.cpp src/ast/instance_minter.cpp
  src/ast/edge_emission_context.cpp src/ast/call_edge_emitter.cpp
  src/ast/statement_edge_visitor.cpp src/ast/function_definition_visitor.cpp
  src/ast/call_template_args.cpp src/ast/display_name_rewrite.cpp
  src/ast/receiver_provenance.cpp src/ast/instantiation_edges.cpp
  src/ast/namespace_use_visitor.cpp src/ast/routed_root_events.cpp
  src/ast/fact_identity.cpp src/ast/fact_batch.cpp src/ast/fact_extraction.cpp
  src/ast/owned_header_plan.cpp
  src/ast/fact_batch_artifact.cpp
  src/ast/indexing_plan.cpp src/ast/pass_registry.cpp
  src/ast/index_engine.cpp
  src/ast/include_facts.cpp
  src/ast/clang_version.cpp)
target_include_directories(cidx_ast PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_include_directories(cidx_ast SYSTEM PRIVATE
  ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
target_compile_definitions(cidx_ast PRIVATE ${LLVM_DEFINITIONS}
  "CIDX_CLANG_RESOURCE_DIR=\"${LLVM_LIBRARY_DIR}/clang/${LLVM_VERSION_MAJOR}\"")
target_compile_features(cidx_ast PRIVATE cxx_std_23)
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(cidx_ast PRIVATE -fno-rtti)
endif()
target_sources(cidx_core PRIVATE $<TARGET_OBJECTS:cidx_ast>)
target_sources(cidx_core PRIVATE src/application/fact_batch_replay.cpp)
# TU fact-cache orchestration: consumes the extraction engine's publication and
# the storage sidecar, so it links after cidx_ast joins cidx_core.
target_sources(cidx_core PRIVATE
  src/application/tu_replay_context.cpp
  src/application/tu_fact_cache_service.cpp)

# --- include-hygiene validator/executor (cidx_hygiene) -----------------------
# validator.cpp drives a SyntaxOnlyAction over an in-memory VFS overlay and
# executor.cpp uses clang-format, so both need the Clang C++ API. Same object-
# library treatment as cidx_ast: -isystem LLVM headers and its own -fno-rtti.
# The rest of src/include_hygiene/ (graph, analysis, plan) is pure storage and
# lives in cidx_core.
add_library(cidx_hygiene OBJECT
  src/include_hygiene/validator.cpp
  src/include_hygiene/executor.cpp
  src/include_hygiene/planner.cpp)
target_include_directories(cidx_hygiene PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_include_directories(cidx_hygiene SYSTEM PRIVATE
  ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
target_compile_definitions(cidx_hygiene PRIVATE ${LLVM_DEFINITIONS}
  "CIDX_CLANG_RESOURCE_DIR=\"${LLVM_LIBRARY_DIR}/clang/${LLVM_VERSION_MAJOR}\"")
target_compile_features(cidx_hygiene PRIVATE cxx_std_23)
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(cidx_hygiene PRIVATE -fno-rtti)
endif()
target_sources(cidx_core PRIVATE $<TARGET_OBJECTS:cidx_hygiene>)

# --- declarative AST extraction DSL (cidx_extract, HSE-64) -------------------
# validator_matchers.cpp and engine.cpp construct/execute Clang dynamic AST
# matchers (clang::ast_matchers::dynamic::Parser, clang::ast_matchers::
# MatchFinder), so this needs the Clang C++ API and the same -isystem/-fno-rtti
# treatment as cidx_ast/cidx_hygiene. plan_ir.hpp/plan_json.cpp/plan_identity.cpp/
# matcher_catalog.cpp/extension_facts.cpp/validator_structure.cpp are Clang-free
# but compiled here too for build simplicity (mirrors ast/fact_batch.cpp living
# inside cidx_ast despite being Clang-free itself).
add_library(cidx_extract OBJECT
  src/extract/plan_json.cpp
  src/extract/plan_identity.cpp
  src/extract/matcher_catalog.cpp
  src/extract/matcher_root_binding.cpp
  src/extract/validator_structure.cpp
  src/extract/validator_matchers.cpp
  src/extract/extension_facts.cpp
  src/extract/engine.cpp
  src/extract/artifact.cpp)
target_include_directories(cidx_extract PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_include_directories(cidx_extract SYSTEM PRIVATE
  ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
target_compile_definitions(cidx_extract PRIVATE ${LLVM_DEFINITIONS}
  "CIDX_CLANG_RESOURCE_DIR=\"${LLVM_LIBRARY_DIR}/clang/${LLVM_VERSION_MAJOR}\"")
target_compile_features(cidx_extract PRIVATE cxx_std_23)
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(cidx_extract PRIVATE -fno-rtti)
endif()
target_sources(cidx_core PRIVATE $<TARGET_OBJECTS:cidx_extract>)

# --- astgraph AST dumper (cidx_astgraph_ast) ---------------------------------
# astgraph.cpp uses the Clang C++ AST (RecursiveASTVisitor + ClangTool), so like
# cidx_ast it compiles in its own object library with -isystem LLVM headers and
# -fno-rtti. astgraph.hpp is clang-free, so cidx_core links the objects without
# ever seeing the Clang C++ headers.
add_library(cidx_astgraph_ast OBJECT src/astgraph/astgraph.cpp)
target_include_directories(cidx_astgraph_ast PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_include_directories(cidx_astgraph_ast SYSTEM PRIVATE
  ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
target_compile_definitions(cidx_astgraph_ast PRIVATE ${LLVM_DEFINITIONS}
  "CIDX_CLANG_RESOURCE_DIR=\"${LLVM_LIBRARY_DIR}/clang/${LLVM_VERSION_MAJOR}\"")
target_compile_features(cidx_astgraph_ast PRIVATE cxx_std_23)
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(cidx_astgraph_ast PRIVATE -fno-rtti)
endif()
target_sources(cidx_core PRIVATE $<TARGET_OBJECTS:cidx_astgraph_ast>)

# --- diff analyzer (cidx_diff_ast) --------------------------------------------
# analyze.cpp is the only clang-including diff TU (docs/diff.md module
# boundary), so like cidx_ast it compiles in its own object library with
# -isystem LLVM headers and -fno-rtti. analyze.hpp is clang-free, so cidx_core
# links the objects without ever seeing the Clang C++ headers.
add_library(cidx_diff_ast OBJECT src/diff/analyze.cpp)
target_include_directories(cidx_diff_ast PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_include_directories(cidx_diff_ast SYSTEM PRIVATE
  ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
target_compile_definitions(cidx_diff_ast PRIVATE ${LLVM_DEFINITIONS}
  "CIDX_CLANG_RESOURCE_DIR=\"${LLVM_LIBRARY_DIR}/clang/${LLVM_VERSION_MAJOR}\"")
target_compile_features(cidx_diff_ast PRIVATE cxx_std_23)
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(cidx_diff_ast PRIVATE -fno-rtti)
endif()
target_sources(cidx_core PRIVATE $<TARGET_OBJECTS:cidx_diff_ast>)

# --- compiledb (cidx_compiledb) ----------------------------------------------
# compiledb.cpp loads compile_commands.json via clang::tooling::JSONCompilationDatabase
# (Clang C++ API), so like cidx_ast it compiles in its own -fno-rtti object library.
# compiledb.hpp is clang-free, so cidx_core links the objects without seeing the
# Clang C++ headers.
add_library(cidx_compiledb OBJECT src/compiledb/compiledb.cpp)
target_include_directories(cidx_compiledb PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_include_directories(cidx_compiledb SYSTEM PRIVATE
  ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
target_compile_definitions(cidx_compiledb PRIVATE ${LLVM_DEFINITIONS})
target_compile_features(cidx_compiledb PRIVATE cxx_std_23)
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(cidx_compiledb PRIVATE -fno-rtti)
endif()
target_sources(cidx_core PRIVATE $<TARGET_OBJECTS:cidx_compiledb>)
target_sources(cidx_core PRIVATE src/workspace/context.cpp)
target_sources(cidx_core PRIVATE src/workspace/tu_fact_cache_identity.cpp)

# cidx indexes via the Clang C++ API only (monolithic shared libs:
# clang-cpp + libLLVM). No libclang.
target_link_libraries(cidx_core PUBLIC clang-cpp LLVM)
# S02: storage layer links SQLite3 (shared, or the static archive under
# CIDX_STATIC).
target_link_libraries(cidx_core PUBLIC ${_cidx_sqlite_link})
target_include_directories(cidx_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(cidx_core PRIVATE
  "CIDX_UI_ASSET_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/web\""
  "CIDX_SPEC_TLA_CONFORMANCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/spec/tla/conformance\"")
target_compile_options(cidx_core PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra>
)
# Builtin-header resource dir of the Clang we compile against, for
# Toolchain::resource_include() (toolchain.cpp). PUBLIC so test exes inherit it.
target_compile_definitions(cidx_core PUBLIC
  "CIDX_CLANG_RESOURCE_DIR=\"${LLVM_LIBRARY_DIR}/clang/${LLVM_VERSION_MAJOR}\"")

# `cidx analyze` embeds the declared Souffle rule files from
# python/indexer/rules; Python consumes the same fact/result contract.
set(_cidx_dl_rules_dir "${CMAKE_CURRENT_SOURCE_DIR}/python/indexer/rules")
set(_cidx_dl_header
  "${CMAKE_CURRENT_BINARY_DIR}/generated/application/souffle_rules.hpp")
add_custom_command(
  OUTPUT "${_cidx_dl_header}"
  COMMAND "${CMAKE_COMMAND}"
          "-DRULES_DIR=${_cidx_dl_rules_dir}"
          "-DOUT=${_cidx_dl_header}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_dl.cmake"
  DEPENDS "${_cidx_dl_rules_dir}/prelude.dl"
          "${_cidx_dl_rules_dir}/callgraph.dl"
          "${_cidx_dl_rules_dir}/cycles.dl"
          "${_cidx_dl_rules_dir}/unused.dl"
          "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_dl.cmake"
  VERBATIM)
add_custom_target(cidx_dl_rules DEPENDS "${_cidx_dl_header}")
add_dependencies(cidx_core cidx_dl_rules)
target_include_directories(cidx_core PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}/generated")

# Optional native Souffle support for maintained astgraph analyses.  Each rule
# is generated at build time and embedded as an object, while user-authored
# Datalog remains the standalone `souffle` CLI workflow.
option(CIDX_ASTGRAPH_SOUFFLE "Build native Souffle astgraph analyses" ON)
if(CIDX_ASTGRAPH_SOUFFLE)
  find_program(CIDX_SOUFFLE_EXECUTABLE NAMES souffle)
  # Homebrew ships a nested include root; prefer it to the parent include dir
  # so generated <souffle/...> includes select one consistent header tree.
  find_path(CIDX_SOUFFLE_INCLUDE_DIR NAMES souffle/CompiledSouffle.h
    HINTS /opt/homebrew/opt/souffle/include/souffle)
  if(CIDX_SOUFFLE_EXECUTABLE AND CIDX_SOUFFLE_INCLUDE_DIR)
    set(_cidx_astgraph_rule_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/astgraph")
    set(_cidx_astgraph_callgraph_cpp
      "${_cidx_astgraph_rule_dir}/ast_callgraph.cpp")
    add_custom_command(
      OUTPUT "${_cidx_astgraph_callgraph_cpp}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${_cidx_astgraph_rule_dir}"
      COMMAND "${CIDX_SOUFFLE_EXECUTABLE}" -g "${_cidx_astgraph_callgraph_cpp}"
              "${CMAKE_CURRENT_SOURCE_DIR}/src/astgraph/rules/ast_callgraph.dl"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/astgraph/rules/ast_callgraph.dl"
      VERBATIM)
    # Souffle 2.5's generated runtime is C++17-compatible but is not C++23
    # clean on current libc++; keep this narrow bridge out of cidx_core.
    add_library(cidx_astgraph_souffle_rules OBJECT
      src/astgraph/souffle_runner.cpp
      "${_cidx_astgraph_callgraph_cpp}")
    set_target_properties(cidx_astgraph_souffle_rules PROPERTIES
      CXX_STANDARD 17
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS OFF
      # Souffle emits ast_callgraph.cpp into the build tree. It is generated
      # third-party code, so it is outside the project's clang-tidy policy.
      CXX_CLANG_TIDY "")
    target_compile_definitions(cidx_astgraph_souffle_rules PRIVATE
      CIDX_HAVE_ASTGRAPH_SOUFFLE=1
      __EMBEDDED_SOUFFLE__)
    target_include_directories(cidx_astgraph_souffle_rules PRIVATE
      "${CIDX_SOUFFLE_INCLUDE_DIR}"
      "${CMAKE_CURRENT_SOURCE_DIR}/src")
    set(CIDX_ASTGRAPH_SOUFFLE_ENABLED ON)
    message(STATUS "cidx: native astgraph Souffle = ${CIDX_SOUFFLE_EXECUTABLE}")
  else()
    message(WARNING "cidx: Souffle not found; native astgraph analyses disabled")
  endif()
endif()
if(NOT CIDX_ASTGRAPH_SOUFFLE_ENABLED)
  target_sources(cidx_core PRIVATE src/astgraph/souffle_stub.cpp)
endif()

# A2.1: the stdc++fs workaround for GNU < 9 is removed — minimum is now gcc 13
# where <filesystem> is in libstdc++ proper with no extra link needed.

add_executable(cidx src/main.cpp)
target_compile_options(cidx PRIVATE -Wall -Wextra)
target_link_libraries(cidx PRIVATE cidx_core)

add_executable(cidx-agent src/application/agent_process_main.cpp)
target_compile_options(cidx-agent PRIVATE -Wall -Wextra)
target_link_libraries(cidx-agent PRIVATE cidx_core)
set_target_properties(cidx-agent PROPERTIES
  BUILD_RPATH "${LLVM_LIBRARY_DIR}"
  INSTALL_RPATH "${LLVM_LIBRARY_DIR}"
  INSTALL_RPATH_USE_LINK_PATH ON
)

# NOTE: -static-libstdc++/-static-libgcc is intentionally NOT applied. cidx_core
# now links the Clang C++ API (clang-cpp + the shared libLLVM), which uses the
# SYSTEM libstdc++; statically embedding a second libstdc++ would put two C++
# runtimes in one process and corrupt std::string/exceptions crossing the LLVM
# boundary. A gcc-toolset-built binary runs on base RHEL 9 via system
# libstdc++.so.6 + the nonshared static bits, matching libLLVM.so.

# RPATH so the dynamic Clang C++ libs (clang-cpp + libLLVM) resolve at runtime.
set_target_properties(cidx PROPERTIES
  BUILD_RPATH "${LLVM_LIBRARY_DIR}"
  INSTALL_RPATH "${LLVM_LIBRARY_DIR}"
  INSTALL_RPATH_USE_LINK_PATH ON
)

# S-075 qualification probe. It is intentionally separate from production
# indexing: ADR-014 selects the no-ship path until a reusable mechanism has
# corpus-backed evidence and a complete failure-atomic implementation.
add_executable(front_end_reuse_probe tests/front_end_reuse_probe.cpp)
target_compile_options(front_end_reuse_probe PRIVATE -Wall -Wextra)
target_link_libraries(front_end_reuse_probe PRIVATE cidx_core)
target_include_directories(front_end_reuse_probe SYSTEM PRIVATE
  ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
set_target_properties(front_end_reuse_probe PROPERTIES
  BUILD_RPATH "${LLVM_LIBRARY_DIR}"
  INSTALL_RPATH "${LLVM_LIBRARY_DIR}"
  INSTALL_RPATH_USE_LINK_PATH ON
)

# cidx-astgraph: dump one TU's AST into <TU>.db for Soufflé/Datalog reasoning
# (src/astgraph/). Shares cidx configuration: compile args come from the same
# index.db, the parse goes through the same Toolchain/Parser as `cidx index`.
add_executable(cidx-astgraph src/astgraph/main.cpp)
target_compile_options(cidx-astgraph PRIVATE -Wall -Wextra)
target_link_libraries(cidx-astgraph PRIVATE cidx_core)
if(CIDX_ASTGRAPH_SOUFFLE_ENABLED)
  target_sources(cidx-astgraph PRIVATE
    $<TARGET_OBJECTS:cidx_astgraph_souffle_rules>)
endif()
# -static-libstdc++ omitted: see the note on the cidx target (dynamic libLLVM).
set_target_properties(cidx-astgraph PROPERTIES
  BUILD_RPATH "${LLVM_LIBRARY_DIR}"
  INSTALL_RPATH "${LLVM_LIBRARY_DIR}"
  INSTALL_RPATH_USE_LINK_PATH ON
)

# cidx-diff: syntax-aware and semantic source diff over the cidx index
# (src/diff/, docs/diff.md). Shares cidx configuration: compile args come from
# the same index.db (opened read-only), the parse goes through the same
# Toolchain as `cidx index`.
add_executable(cidx-diff src/diff/main.cpp)
target_compile_options(cidx-diff PRIVATE -Wall -Wextra)
target_link_libraries(cidx-diff PRIVATE cidx_core)
# -static-libstdc++ omitted: see the note on the cidx target (dynamic libLLVM).
set_target_properties(cidx-diff PROPERTIES
  BUILD_RPATH "${LLVM_LIBRARY_DIR}"
  INSTALL_RPATH "${LLVM_LIBRARY_DIR}"
  INSTALL_RPATH_USE_LINK_PATH ON
)

# Example programs (examples/). OFF by default: they are documentation-grade
# samples, not part of the product build.
option(CIDX_BUILD_EXAMPLES "Build example programs under examples/" OFF)
if(CIDX_BUILD_EXAMPLES)
  add_executable(cidx-queryplan-example examples/queryplan/queryplan_example.cpp)
  target_compile_options(cidx-queryplan-example PRIVATE -Wall -Wextra)
  target_link_libraries(cidx-queryplan-example PRIVATE cidx_core)
  # Same dynamic Clang/LLVM link model as the cidx target (see its note).
  set_target_properties(cidx-queryplan-example PROPERTIES
    BUILD_RPATH "${LLVM_LIBRARY_DIR}"
    INSTALL_RPATH "${LLVM_LIBRARY_DIR}"
    INSTALL_RPATH_USE_LINK_PATH ON
  )
endif()

enable_testing()
add_subdirectory(tests)

# Always-available explicit lint target. This re-checks every project-owned
# source in compile_commands.json, independent of incremental build state.
set(_cidx_tidy_environment)
if(CIDX_CLANG_TIDY_WARNINGS_AS_ERRORS)
  list(APPEND _cidx_tidy_environment "CIDX_TIDY_WARNINGS_AS_ERRORS=1")
endif()
add_custom_target(cidx-clang-tidy
  COMMAND "${CMAKE_COMMAND}" -E env ${_cidx_tidy_environment}
          "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_clang_tidy.sh"
          "${CMAKE_BINARY_DIR}"
  DEPENDS cidx_dl_rules
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  COMMENT "Running clang-tidy over C++23 project sources"
  VERBATIM)
