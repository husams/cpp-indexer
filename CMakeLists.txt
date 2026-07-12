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

# CIDX_STATIC: prefer static archives for the third-party dependencies so the
# binary carries fewer runtime requirements. Platform reality differs:
#   * Linux: SQLite3 from its static archive so the binary needs no libsqlite3
#     at runtime. libstdc++ stays DYNAMIC (system libstdc++.so.6): cidx_core
#     links the Clang C++ API (dynamic libLLVM) which uses the system libstdc++,
#     so a second static libstdc++ would corrupt the ABI across that boundary.
#     glibc
#     stays dynamic (a fully static glibc binary is discouraged and breaks
#     NSS/dlopen). libclang is ALSO linked statically when a C-API wrapper
#     archive is supplied via -DCIDX_LIBCLANG_STATIC=<libclang.a> (build it once
#     with scripts/build-static-libclang.sh against a NON-LTO distro LLVM, e.g.
#     apt llvm-18 — validated 2026-06-13 on Ubuntu 24.04 -> the binary has no
#     libclang.so dependency). Without it, libclang falls back to dynamic: no
#     distribution ships a linkable static libclang C API (release tarballs are
#     LTO bitcode; -dev packages ship only libclang.so), hence the wrapper.
#   * macOS: only SQLite3 has a static archive; libclang ships shared-only and
#     Apple provides no static libSystem, so libclang stays dynamic (RPATH).
# Each dependency falls back to its shared form (with a STATUS/WARNING) when no
# static archive is found, so a static configure never hard-fails.
option(CIDX_STATIC "Statically link dependencies where the platform allows" OFF)

# A2.1: C++23 standard; minimum compilers (A2.1 guard).
# Build hosts: macOS AppleClang (dev) and Ubuntu 24.04 gcc 13 (e2e).
# g++ 8.5 cross toolchains are *indexing targets*, not the cidx build host.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

# S02: storage layer sits on the system SQLite3 (>= 3.35 enforced at runtime).
find_package(SQLite3 REQUIRED)

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

# ---------------------------------------------------------------------------
# A1 (Amendment A1 — spec §12): link libclang at build time.
# Search order (A1.1):
#   1. CIDX_LIBCLANG cache var (configure-time FILEPATH), if set.
#   2. llvm-config --libdir output directory.
#   3. /opt/llvm*/lib64, /opt/llvm*/lib, /usr/lib/llvm-*/lib,
#      /usr/local/llvm*/lib globs.
#   4. macOS pip-wheel native/ directory (python3 probe, OPTIONAL).
#   5. System default (no PATHS — CMake's normal library search).
# FATAL_ERROR if not resolved.
# ---------------------------------------------------------------------------

set(CIDX_LIBCLANG "" CACHE FILEPATH "Path to libclang shared library (.so/.dylib)")

set(_cidx_libclang_hints "")

# 1. User-provided path (full file → use directly; directory → add as hint).
if(CIDX_LIBCLANG)
  if(EXISTS "${CIDX_LIBCLANG}" AND NOT IS_DIRECTORY "${CIDX_LIBCLANG}")
    set(CIDX_LIBCLANG_LIB "${CIDX_LIBCLANG}")
  else()
    list(APPEND _cidx_libclang_hints "${CIDX_LIBCLANG}")
  endif()
endif()

if(NOT CIDX_LIBCLANG_LIB)
  # 2. llvm-config --libdir (quiet on failure). Prefer a Homebrew keg-only LLVM
  #    (macOS): it is NOT symlinked onto PATH, so hint its bin/ explicitly. This
  #    is how macOS builds pick up the installed LLVM (e.g. 22) headers+library
  #    instead of the pip `libclang` wheel (which caps at 18 and ships no headers).
  find_program(_llvm_config
    NAMES llvm-config
    HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin)
  if(_llvm_config)
    execute_process(
      COMMAND "${_llvm_config}" --libdir
      OUTPUT_VARIABLE _llvm_libdir
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
      RESULT_VARIABLE _llvm_config_result
    )
    if(_llvm_config_result EQUAL 0 AND _llvm_libdir)
      list(APPEND _cidx_libclang_hints "${_llvm_libdir}")
    endif()
  endif()

  # 3. Glob-based /opt/llvm* and /usr/lib/llvm-* candidates.
  foreach(_glob_pat
      "/opt/llvm*/lib64" "/opt/llvm*/lib"
      "/usr/lib/llvm-*/lib"
      "/usr/local/llvm*/lib")
    file(GLOB _glob_dirs "${_glob_pat}")
    if(_glob_dirs)
      list(SORT _glob_dirs)
      list(APPEND _cidx_libclang_hints ${_glob_dirs})
    endif()
  endforeach()

  # 4. macOS pip-wheel native/ directory (python3 probe, OPTIONAL).
  find_program(_python3 python3)
  if(_python3)
    execute_process(
      COMMAND "${_python3}" -c
        "import clang.cindex, os; print(os.path.dirname(clang.cindex.__file__)+'/native')"
      OUTPUT_VARIABLE _wheel_native_dir
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
      RESULT_VARIABLE _wheel_probe_result
    )
    if(_wheel_probe_result EQUAL 0 AND _wheel_native_dir AND IS_DIRECTORY "${_wheel_native_dir}")
      list(APPEND _cidx_libclang_hints "${_wheel_native_dir}")
    endif()
  endif()

  # 5. find_library with accumulated hints (+ CMake system default).
  find_library(CIDX_LIBCLANG_LIB
    NAMES clang libclang
    HINTS ${_cidx_libclang_hints}
    DOC "Path to libclang shared library"
  )
endif()

if(NOT CIDX_LIBCLANG_LIB)
  message(FATAL_ERROR
    "libclang not found. Pass -DCIDX_LIBCLANG=/path/to/libclang.{so,dylib} "
    "or install an LLVM development package.")
endif()

get_filename_component(_cidx_libclang_dir "${CIDX_LIBCLANG_LIB}" DIRECTORY)
message(STATUS "libclang: ${CIDX_LIBCLANG_LIB}")
message(STATUS "libclang dir (RPATH): ${_cidx_libclang_dir}")

# A1.4: locate the libclang C-API headers (clang-c/Index.h). These used to be
# vendored under third_party/clang-c (pinned to LLVM 18); they now come from the
# SAME LLVM install as the linked library, so header and runtime versions can no
# longer drift. Resolution mirrors the library search above.
#   1. CIDX_LIBCLANG_INCLUDE cache var, if set.
#   2. llvm-config --includedir.
#   3. Sibling include/ of the resolved libclang directory.
#   4. Homebrew / /opt/llvm* / /usr/lib/llvm-* / /usr/include globs.
set(CIDX_LIBCLANG_INCLUDE "" CACHE PATH "Directory containing clang-c/Index.h")
set(_cidx_clang_inc_hints "")
if(CIDX_LIBCLANG_INCLUDE)
  list(APPEND _cidx_clang_inc_hints "${CIDX_LIBCLANG_INCLUDE}")
endif()
if(_llvm_config)
  execute_process(
    COMMAND "${_llvm_config}" --includedir
    OUTPUT_VARIABLE _llvm_incdir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _llvm_inc_result)
  if(_llvm_inc_result EQUAL 0 AND _llvm_incdir)
    list(APPEND _cidx_clang_inc_hints "${_llvm_incdir}")
  endif()
endif()
list(APPEND _cidx_clang_inc_hints
  "${_cidx_libclang_dir}/../include"
  "${_cidx_libclang_dir}/../../include")
foreach(_inc_glob
    "/opt/homebrew/opt/llvm/include" "/usr/local/opt/llvm/include"
    "/opt/llvm*/include" "/usr/lib/llvm-*/include" "/usr/include")
  file(GLOB _inc_dirs "${_inc_glob}")
  if(_inc_dirs)
    list(SORT _inc_dirs)
    list(APPEND _cidx_clang_inc_hints ${_inc_dirs})
  endif()
endforeach()
find_path(CIDX_CLANG_C_INCLUDE_DIR
  NAMES clang-c/Index.h
  HINTS ${_cidx_clang_inc_hints}
  DOC "Directory containing the libclang C-API headers (clang-c/Index.h)")
if(NOT CIDX_CLANG_C_INCLUDE_DIR)
  message(FATAL_ERROR
    "clang-c/Index.h not found. Install an LLVM development package "
    "(clang-devel / llvm-toolset on RHEL, `brew install llvm` on macOS) or pass "
    "-DCIDX_LIBCLANG_INCLUDE=<dir containing clang-c/>.")
endif()
message(STATUS "libclang headers: ${CIDX_CLANG_C_INCLUDE_DIR}")

# How libclang is linked into cidx_core. Default: the resolved shared library
# (RPATH-located at runtime). Under CIDX_STATIC on a non-Apple host, try to
# build a static link line instead.
set(_cidx_libclang_link "${CIDX_LIBCLANG_LIB}")
set(_cidx_libclang_static OFF)

if(CIDX_STATIC AND APPLE)
  message(STATUS "cidx[static]: macOS — libclang stays dynamic "
                 "(no static archive ships; libSystem has no static form)")
elseif(CIDX_STATIC)
  # Linux: link a static libclang. The libclang C API itself ships only as a
  # shared lib everywhere, so a wrapper libclang.a (the clang/tools/libclang
  # C-API objects) must be built first with scripts/build-static-libclang.sh and
  # pointed at via -DCIDX_LIBCLANG_STATIC. We then add the distro's clang/LLVM
  # *component* static archives (regular ELF, libstdc++ — ABI-matched to a g++
  # build of cidx; an LTO/bitcode LLVM will NOT link) and the llvm-config LLVM +
  # system libs, all in one --start-group so the circular refs resolve.
  #   Validated 2026-06-13 on Ubuntu 24.04 apt llvm-18 (18.1.3): the resulting
  #   cidx has NO libclang.so dependency (only libc/libm/libz/libzstd/libtinfo).
  # CIDX_LIBCLANG_STATIC: path to the C-API wrapper archive. Usually passed via
  # -D (printed by build-static-libclang.sh); else try to find one on disk.
  if(NOT CIDX_LIBCLANG_STATIC)
    find_library(CIDX_LIBCLANG_STATIC NAMES libclang.a
      HINTS "${_cidx_libclang_dir}" ${_cidx_libclang_hints}
      DOC "Static libclang.a (C-API wrapper) for CIDX_STATIC")
  endif()
  # llvm-config MUST come from the SAME prefix as the libclang we are linking
  # (its --libdir/--libs select the component archives). Pin it to the resolved
  # libclang's sibling bin/ rather than trusting PATH (which may point at an
  # unrelated/LTO LLVM, e.g. /opt/llvm-*). -DCIDX_LLVM_CONFIG overrides.
  if(CIDX_LLVM_CONFIG)
    set(_cidx_llvm_config "${CIDX_LLVM_CONFIG}")
  elseif(EXISTS "${_cidx_libclang_dir}/../bin/llvm-config")
    set(_cidx_llvm_config "${_cidx_libclang_dir}/../bin/llvm-config")
  else()
    find_program(_cidx_llvm_config NAMES llvm-config llvm-config-18)
  endif()
  if(CIDX_LIBCLANG_STATIC AND EXISTS "${CIDX_LIBCLANG_STATIC}" AND _cidx_llvm_config)
    execute_process(COMMAND "${_cidx_llvm_config}" --libdir
      OUTPUT_VARIABLE _ll_libdir OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND "${_cidx_llvm_config}" --ldflags
      OUTPUT_VARIABLE _ll_ldflags OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND "${_cidx_llvm_config}" --link-static --libs
      OUTPUT_VARIABLE _ll_libs OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND "${_cidx_llvm_config}" --link-static --system-libs
      OUTPUT_VARIABLE _ll_syslibs OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    separate_arguments(_ll_ldflags)
    separate_arguments(_ll_libs)
    separate_arguments(_ll_syslibs)
    # Drop components llvm-config lists but the distro ships no static .a for
    # (e.g. Polly on apt LLVM) — they would fail with 'cannot find -lPolly'.
    list(FILTER _ll_libs EXCLUDE REGEX "Polly")
    # Clang component archives (libclangAST.a, ...) live beside the LLVM ones.
    file(GLOB _cidx_clang_static_libs "${_ll_libdir}/libclang*.a")
    list(SORT _cidx_clang_static_libs)
    set(_cidx_libclang_link
      ${_ll_ldflags}
      "-Wl,--start-group"
      "${CIDX_LIBCLANG_STATIC}"          # the C-API wrapper archive
      ${_cidx_clang_static_libs}          # clang component archives
      ${_ll_libs}                         # LLVM component archives
      "-Wl,--end-group"
      ${_ll_syslibs})
    set(_cidx_libclang_static ON)
    message(STATUS "cidx[static]: libclang (static) = ${CIDX_LIBCLANG_STATIC}")
    message(STATUS "cidx[static]: LLVM libdir = ${_ll_libdir}")
  else()
    message(WARNING "cidx[static]: no static libclang -> libclang stays dynamic. "
      "Build the C-API wrapper with scripts/build-static-libclang.sh and pass "
      "-DCIDX_LIBCLANG_STATIC=<libclang.a> (needs a non-LTO LLVM + llvm-config).")
  endif()
endif()

# Core library: util + storage + clangx + compiledb + cli + vendored MD5/SHA1.
add_library(cidx_core STATIC
  src/util/logger.cpp
  src/util/env.cpp
  src/util/subprocess.cpp
  src/util/hashing.cpp
  src/util/json_min.cpp
  src/util/pathutil.cpp
  src/util/repo.cpp
  src/util/files.cpp
  src/storage/sqlite.cpp
  src/storage/storage.cpp
  src/clangx/clang_runtime.cpp
  src/clangx/toolchain.cpp
  src/clangx/parse.cpp
  src/clangx/pch.cpp
  src/clangx/ast.cpp
  src/clangx/ast_cursor.cpp
  src/clangx/ast_symbols.cpp
  src/clangx/ast_templates.cpp
  src/clangx/ast_body.cpp
  src/clangx/ast_edges.cpp
  src/clangx/ast_query.cpp
  src/astcache/astcache.cpp
  src/compiledb/compiledb.cpp
  src/cli/args.cpp
  src/cli/format.cpp
  src/cli/json_out.cpp
  src/cli/kind_names.cpp
  src/cli/commands.cpp
  src/cli/analyze.cpp
  src/graph/query.cpp
  src/graph/emit.cpp
  src/astgraph/astgraph.cpp
  third_party/md5/md5.c
  third_party/sha1/sha1.c
)

# --- LibTooling indexing engine (cidx_lt) ------------------------------------
# The LT engine uses the Clang C++ API. Compiled as its own object library so
# its LLVM/Clang headers are added with -isystem (a plain -I would break
# gcc's #include_next <stdlib.h> in <cstdlib>) and it can carry -fno-rtti
# independently. lt_engine.hpp is clang-free, so cidx_core links the objects
# but never sees the Clang C++ headers.
find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)
add_library(cidx_lt OBJECT
  src/clangx_lt/kind_map.cpp src/clangx_lt/usr.cpp src/clangx_lt/location.cpp
  src/clangx_lt/names.cpp src/clangx_lt/decl_flags.cpp
  src/clangx_lt/symbol_extractor.cpp src/clangx_lt/symbol_visitor.cpp
  src/clangx_lt/storage_symbol_sink.cpp src/clangx_lt/mint_builder.cpp
  src/clangx_lt/template_arg_resolver.cpp src/clangx_lt/storage_edge_sink.cpp
  src/clangx_lt/edge_visitor.cpp src/clangx_lt/type_use.cpp
  src/clangx_lt/value_source.cpp src/clangx_lt/instance_minter.cpp
  src/clangx_lt/body_walker.cpp src/clangx_lt/body_pass_visitor.cpp
  src/clangx_lt/ns_uses_visitor.cpp src/clangx_lt/lt_engine.cpp)
target_include_directories(cidx_lt PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_include_directories(cidx_lt SYSTEM PRIVATE
  ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
target_compile_definitions(cidx_lt PRIVATE ${LLVM_DEFINITIONS}
  "CIDX_LT_RESOURCE_DIR=\"${LLVM_LIBRARY_DIR}/clang/${LLVM_VERSION_MAJOR}\"")
target_compile_features(cidx_lt PRIVATE cxx_std_23)
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(cidx_lt PRIVATE -fno-rtti)
endif()
target_sources(cidx_core PRIVATE $<TARGET_OBJECTS:cidx_lt>)

# A1.1: link libclang (shared by default; a static archive group under
# CIDX_STATIC on Linux — see the resolution block above).
target_link_libraries(cidx_core PUBLIC ${_cidx_libclang_link})
# LT engine: the Clang C++ API (monolithic shared libs: clang-cpp + libLLVM).
target_link_libraries(cidx_core PUBLIC clang-cpp LLVM)
# S02: storage layer links SQLite3 (shared, or the static archive under
# CIDX_STATIC).
target_link_libraries(cidx_core PUBLIC ${_cidx_sqlite_link})
target_include_directories(cidx_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
  ${CIDX_CLANG_C_INCLUDE_DIR}
)
target_compile_options(cidx_core PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra>
)
# A1.3: bake the resolved libclang path into the binary so library_path()
# returns it without a dladdr() call. PUBLIC so all linked test exes inherit it.
target_compile_definitions(cidx_core PUBLIC
  "CIDX_LIBCLANG_PATH=\"${CIDX_LIBCLANG_LIB}\"")

# `cidx analyze` embeds the canonical Souffle rule files from
# python/indexer/rules so both implementations ship byte-identical Datalog.
set(_cidx_dl_rules_dir "${CMAKE_CURRENT_SOURCE_DIR}/python/indexer/rules")
set(_cidx_dl_header
  "${CMAKE_CURRENT_BINARY_DIR}/generated/cli/souffle_rules.hpp")
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
      CXX_EXTENSIONS OFF)
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

# NOTE: -static-libstdc++/-static-libgcc is intentionally NOT applied. cidx_core
# now links the Clang C++ API (clang-cpp + the shared libLLVM), which uses the
# SYSTEM libstdc++; statically embedding a second libstdc++ would put two C++
# runtimes in one process and corrupt std::string/exceptions crossing the LLVM
# boundary. A gcc-toolset-built binary runs on base RHEL 9 via system
# libstdc++.so.6 + the nonshared static bits, matching libLLVM.so.

# RPATH is only needed while libclang is dynamic. With a static libclang there
# is no shared dependency to locate, so skip it (a leftover RPATH is harmless
# but misleading).
if(NOT _cidx_libclang_static)
  set_target_properties(cidx PROPERTIES
    BUILD_RPATH "${_cidx_libclang_dir}"
    INSTALL_RPATH "${_cidx_libclang_dir}"
    INSTALL_RPATH_USE_LINK_PATH ON
  )
endif()

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
if(NOT _cidx_libclang_static)
  set_target_properties(cidx-astgraph PROPERTIES
    BUILD_RPATH "${_cidx_libclang_dir}"
    INSTALL_RPATH "${_cidx_libclang_dir}"
    INSTALL_RPATH_USE_LINK_PATH ON
  )
endif()

enable_testing()
add_subdirectory(tests)

# ---------------------------------------------------------------------------
# Experimental LibTooling AST probe (Phase-0 de-risk for the clangx-LT redesign,
# see wiki pages/planning/cidx-libtooling-ast-layer). OFF by default; links the
# full Clang C++ API (clang-cpp), unlike the rest of cidx which uses only the
# libclang C API. Compiled -fno-rtti to match a Homebrew/no-RTTI LLVM.
# ---------------------------------------------------------------------------
option(CIDX_CLANGX_LIBTOOLING "Build the experimental LibTooling AST probe" OFF)
if(CIDX_CLANGX_LIBTOOLING)
  find_package(LLVM REQUIRED CONFIG)
  find_package(Clang REQUIRED CONFIG)
  message(STATUS "cidx[lt]: LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")
  add_executable(cidx_lt_probe
    src/clangx_lt/main.cpp
    src/clangx_lt/kind_map.cpp
    src/clangx_lt/usr.cpp
    src/clangx_lt/location.cpp
    src/clangx_lt/names.cpp
    src/clangx_lt/decl_flags.cpp
    src/clangx_lt/symbol_extractor.cpp
    src/clangx_lt/tsv_symbol_emitter.cpp
    src/clangx_lt/symbol_visitor.cpp
    src/clangx_lt/symbol_consumer.cpp
    src/clangx_lt/symbol_action.cpp
  )
  # Builtin-header resource dir of the LLVM we compile against (gotcha 8b).
  target_compile_definitions(cidx_lt_probe PRIVATE
    "CIDX_LT_RESOURCE_DIR=\"${LLVM_LIBRARY_DIR}/clang/${LLVM_VERSION_MAJOR}\"")
  target_include_directories(cidx_lt_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_include_directories(cidx_lt_probe SYSTEM PRIVATE
    ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
  target_compile_definitions(cidx_lt_probe PRIVATE ${LLVM_DEFINITIONS})
  target_compile_features(cidx_lt_probe PRIVATE cxx_std_17)
  if(NOT LLVM_ENABLE_RTTI)
    target_compile_options(cidx_lt_probe PRIVATE -fno-rtti)
  endif()
  target_link_libraries(cidx_lt_probe PRIVATE clang-cpp LLVM)

  # Phase-2 gate tool: full interleaved LT indexer (symbols + edges per file).
  add_executable(cidx_lt_index
    src/clangx_lt/index_main.cpp
    src/clangx_lt/index_consumer.cpp
    src/clangx_lt/kind_map.cpp
    src/clangx_lt/usr.cpp
    src/clangx_lt/location.cpp
    src/clangx_lt/names.cpp
    src/clangx_lt/decl_flags.cpp
    src/clangx_lt/symbol_extractor.cpp
    src/clangx_lt/symbol_visitor.cpp
    src/clangx_lt/storage_symbol_sink.cpp
    src/clangx_lt/mint_builder.cpp
    src/clangx_lt/template_arg_resolver.cpp
    src/clangx_lt/storage_edge_sink.cpp
    src/clangx_lt/edge_visitor.cpp
    src/clangx_lt/type_use.cpp
    src/clangx_lt/value_source.cpp
    src/clangx_lt/instance_minter.cpp
    src/clangx_lt/body_walker.cpp
    src/clangx_lt/body_pass_visitor.cpp
    src/clangx_lt/ns_uses_visitor.cpp
  )
  target_compile_definitions(cidx_lt_index PRIVATE
    "CIDX_LT_RESOURCE_DIR=\"${LLVM_LIBRARY_DIR}/clang/${LLVM_VERSION_MAJOR}\"")
  target_include_directories(cidx_lt_index PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_include_directories(cidx_lt_index SYSTEM PRIVATE
    ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
  target_compile_definitions(cidx_lt_index PRIVATE ${LLVM_DEFINITIONS})
  target_compile_features(cidx_lt_index PRIVATE cxx_std_23)
  if(NOT LLVM_ENABLE_RTTI)
    target_compile_options(cidx_lt_index PRIVATE -fno-rtti)
  endif()
  target_link_libraries(cidx_lt_index PRIVATE cidx_core clang-cpp LLVM)
endif()
