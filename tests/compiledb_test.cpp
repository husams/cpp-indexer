// S03 tests — compiledb strip/sanitize/driver (hermetic, label "default")
// and CompileDb::load / linked libclang over the real manifests compile DBs
// (suite "clang", label "clang").
//
// Skip policy (A1 amendment): libclang is now linked, so "no libclang" cannot
// occur — the binary wouldn't link.  SKIP-77 is retained ONLY for the
// fixture-gap case: when CIDX_MANIFESTS_DIR is absent (e.g. the e2e box that
// rsyncs only cidx-cpp/).  The custom main() exits 77 in that case.
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

#include <sys/stat.h>

#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include <unistd.h>

#include "compiledb/compiledb.hpp"
#include "util/errors.hpp"
#include "util/logger.hpp"
#include "util/pathutil.hpp"

using cidx::CompileCommand;
using cidx::CompileDb;

namespace {

bool g_fixture_skipped = false;

// Returns true when CIDX_MANIFESTS_DIR points at an existing directory.
// On a host without the lab checkout (e.g. the e2e box that only rsyncs
// cidx-cpp/) the fixture cases should SKIP rather than fail.
bool require_manifests() {
  struct stat st{};
  if (::stat(CIDX_MANIFESTS_DIR, &st) != 0 || !S_ISDIR(st.st_mode)) {
    g_fixture_skipped = true;
    MESSAGE("SKIP: lab fixtures not found at " << CIDX_MANIFESTS_DIR);
    return false;
  }
  return true;
}

// A1: libclang is linked — load() is a no-op, always succeeds.
// Returns the singleton; never returns nullptr.

// setenv/unsetenv with restore-on-destruction (the clang-labelled ctest
// registration may inject CIDX_LIBCLANG; don't clobber it for later cases).
class ScopedEnv {
public:
  ScopedEnv(const char *name, const char *value) : name_(name) {
    const char *prev = std::getenv(name);
    if (prev != nullptr) {
      prev_ = prev;
    }
    ::setenv(name, value, 1);
  }
  ~ScopedEnv() {
    if (prev_) {
      ::setenv(name_, prev_->c_str(), 1);
    } else {
      ::unsetenv(name_);
    }
  }

private:
  const char *name_;
  std::optional<std::string> prev_;
};

const CompileCommand &find_command(const std::vector<CompileCommand> &cmds,
                                   const std::string &filename) {
  for (const CompileCommand &c : cmds) {
    if (c.filename == filename) {
      return c;
    }
  }
  static CompileCommand missing;
  FAIL("no command for filename ", filename);
  return missing;
}

} // namespace

// ---------------------------------------------------------------------------
// Hermetic cases (default label) — arg vectors fed directly.
// ---------------------------------------------------------------------------

TEST_CASE("strip: driver, -c/-o pair, source, glued -I absolutized") {
  // The manifests shapes.c command, verbatim.
  const std::vector<std::string> argv = {
      "cc", "-I.",      "-std=c11", "-DMAX_SHAPES=64",
      "-c", "shapes.c", "-o",       "shapes.o"};
  const auto out = CompileDb::strip_for_libclang(argv, "shapes.c", "/x/y");
  CHECK(out ==
        std::vector<std::string>{"-I/x/y", "-std=c11", "-DMAX_SHAPES=64"});
}

TEST_CASE("strip: bare drops -- -M family -Werror -pedantic-errors") {
  const std::vector<std::string> argv = {"gcc",
                                         "--",
                                         "-M",
                                         "-MM",
                                         "-MD",
                                         "-MMD",
                                         "-MG",
                                         "-MP",
                                         "-MV",
                                         "-Werror",
                                         "-pedantic-errors",
                                         "-DKEEP",
                                         "a.c"};
  const auto out = CompileDb::strip_for_libclang(argv, "a.c", "/d");
  CHECK(out == std::vector<std::string>{"-DKEEP"});
}

TEST_CASE("strip: pair drops consume the following argument") {
  const std::vector<std::string> argv = {"cc",     "-o",
                                         "out.o",  "-MF",
                                         "deps.d", "-MT",
                                         "target", "-MQ",
                                         "q",      "-dependency-file",
                                         "d.d",    "--serialize-diagnostics",
                                         "s.dia",  "-DKEEP",
                                         "a.c"};
  const auto out = CompileDb::strip_for_libclang(argv, "a.c", "/d");
  CHECK(out == std::vector<std::string>{"-DKEEP"});
}

TEST_CASE("strip: trailing pair-flag with no argument does not crash") {
  const std::vector<std::string> argv = {"cc", "-DKEEP", "-o"};
  const auto out = CompileDb::strip_for_libclang(argv, "a.c", "/d");
  CHECK(out == std::vector<std::string>{"-DKEEP"});
}

TEST_CASE("strip: prefix drops -Werror= -Wp,-M and glued -MF/-MT/-MQ") {
  const std::vector<std::string> argv = {
      "cc",        "-Werror=format", "-Wp,-MD,foo.d", "-Wp,-MMD,bar.d",
      "-MFdeps.d", "-MTtarget",      "-MQquoted",     "-Wall",
      "a.c"};
  const auto out = CompileDb::strip_for_libclang(argv, "a.c", "/d");
  CHECK(out == std::vector<std::string>{"-Wall"});
}

TEST_CASE("strip: source dropped by full path OR basename (G10)") {
  const std::vector<std::string> argv = {"cc", "/abs/dir/foo.c", "foo.c",
                                         "-DKEEP"};
  const auto out =
      CompileDb::strip_for_libclang(argv, "/abs/dir/foo.c", "/abs/dir");
  CHECK(out == std::vector<std::string>{"-DKEEP"});
}

TEST_CASE("strip: -I/-isystem/-iquote absolutized, spaced and glued (G12)") {
  const std::vector<std::string> argv = {
      "cc",           "-I",      "foo", "-Iglued/dir", "-isystem", "sys",
      "-isystemsys2", "-iquote", "q",   "-iquoteq2",   "a.c"};
  const auto out = CompileDb::strip_for_libclang(argv, "a.c", "/base");
  CHECK(out == std::vector<std::string>{"-I", "/base/foo", "-I/base/glued/dir",
                                        "-isystem", "/base/sys",
                                        "-isystem/base/sys2", "-iquote",
                                        "/base/q", "-iquote/base/q2"});
}

TEST_CASE("strip: absolute include paths pass through unchanged") {
  const std::vector<std::string> argv = {"cc", "-I/usr/include", "-isystem",
                                         "/opt/inc", "a.c"};
  const auto out = CompileDb::strip_for_libclang(argv, "a.c", "/base");
  CHECK(out ==
        std::vector<std::string>{"-I/usr/include", "-isystem", "/opt/inc"});
}

TEST_CASE("strip: relative include normpathed against directory") {
  const std::vector<std::string> argv = {"cc", "-I.", "-I../inc", "a.c"};
  const auto out = CompileDb::strip_for_libclang(argv, "a.c", "/x/y");
  CHECK(out == std::vector<std::string>{"-I/x/y", "-I/x/inc"});
}

TEST_CASE("strip: everything else untouched, order preserved") {
  const std::vector<std::string> argv = {
      "cc", "-std=c++17", "-DFOO=bar", "-Wall", "-fPIC", "-pthread", "a.c"};
  const auto out = CompileDb::strip_for_libclang(argv, "a.c", "/d");
  CHECK(out == std::vector<std::string>{"-std=c++17", "-DFOO=bar", "-Wall",
                                        "-fPIC", "-pthread"});
}

TEST_CASE("sanitize: re-applies drop rules only — no argv0/source/path fixes "
          "(G11)") {
  // argv[0] position is NOT special and the source token is NOT dropped.
  const std::vector<std::string> stored = {
      "-I.", "-Werror", "-MF", "deps.d", "-Wp,-MD,x.d", "-MFglued",
      "-c",  "foo.c",   "-o",  "foo.o",  "-DKEEP"};
  const auto out = CompileDb::sanitize(stored);
  CHECK(out == std::vector<std::string>{"-I.", "foo.c", "-DKEEP"});
}

TEST_CASE("sanitize: clean vector passes through") {
  const std::vector<std::string> stored = {"-I/abs", "-std=c11", "-DX=1"};
  CHECK(CompileDb::sanitize(stored) == stored);
}

TEST_CASE("command prefix: env assignments + ccache launcher stripped "
          "(v0.27.1)") {
  // A real ccache invocation: CCACHE_*=... env assignments, then `ccache g++`,
  // then the actual flags. The driver is g++ (not the env prefix) and the
  // stored options carry none of the cache/launcher junk.
  const std::vector<std::string> raw = {
      "CCACHE_DIR=/workspace/.ccache", "CCACHE_COMPRESS=1",
      "CCACHE_AOPS-52378DIR=",         "ccache",
      "g++",                           "-g",
      "-std=gnu++17",                  "-I/foo/include",
      "-lbar",                         "-c",
      "x.cpp",                         "-o",
      "x.o"};
  CHECK(CompileDb::command_start(raw) == 4); // index of g++
  CHECK(CompileDb::driver(raw, "/work") == "g++");
  CHECK(CompileDb::strip_for_libclang(raw, "x.cpp", "/work") ==
        std::vector<std::string>{"-g", "-std=gnu++17", "-I/foo/include"});
  // A plain `cc ...` is unaffected: command_start 0, driver cc.
  const std::vector<std::string> plain = {"cc", "-I.", "-c", "a.c"};
  CHECK(CompileDb::command_start(plain) == 0);
  CHECK(CompileDb::driver(plain, "/work") == "cc");
}

TEST_CASE("sanitize: heals a stored env/launcher/compiler prefix (v0.27.1)") {
  // An older import dropped only argv[0] (the CCACHE_DIR=... assignment),
  // leaving the rest of the prefix in the stored options; sanitize drops
  // through the real compiler.
  const std::vector<std::string> stored = {
      "CCACHE_COMPRESS=1", "ccache", "g++", "-g", "-I/abs", "-DX=1"};
  CHECK(CompileDb::sanitize(stored) ==
        std::vector<std::string>{"-g", "-I/abs", "-DX=1"});
}

TEST_CASE("sanitize: drops linker/library/cache flags, keeps parse flags "
          "(v0.27.0)") {
  // Header-search (-nostdinc) and preprocessor (-pthread) flags are KEPT;
  // everything link/cache-only is dropped. Mirrors the Python test.
  const std::vector<std::string> stored = {
      "-I/inc",   "-std=c++17",   "-DFOO=1",      "-nostdinc",
      "-pthread", "-lfoo",        "-l",           "bar",
      "-L/usr/lib", "-L",         "/extra/lib",   "-Wl,-rpath,/x",
      "-Wa,--noexecstack",        "-shared",      "-static",
      "-rdynamic", "-pie",        "-no-pie",      "-s",
      "-pipe",    "-static-libstdc++",            "-fuse-ld=lld",
      "-fmodules-cache-path=/tmp/cc",             "-Xlinker",
      "-znow",    "-T",           "link.ld"};
  CHECK(CompileDb::sanitize(stored) ==
        std::vector<std::string>{"-I/inc", "-std=c++17", "-DFOO=1", "-nostdinc",
                                 "-pthread"});
}

TEST_CASE("driver: bare name kept bare for PATH resolution") {
  CHECK(CompileDb::driver({"cc", "-c", "a.c"}, "/b") == "cc");
  CHECK(CompileDb::driver({"g++"}, "/b") == "g++");
}

TEST_CASE("driver: relative path with separator absolutized") {
  CHECK(CompileDb::driver({"./gcc", "a.c"}, "/b") == "/b/gcc");
  CHECK(CompileDb::driver({"tools/bin/g++", "a.c"}, "/b") ==
        "/b/tools/bin/g++");
}

TEST_CASE("driver: absolute path unchanged") {
  CHECK(CompileDb::driver({"/opt/1A/toolchain/bin/g++", "a.c"}, "/b") ==
        "/opt/1A/toolchain/bin/g++");
}

TEST_CASE("db_dir_from_arg: trailing compile_commands.json stripped") {
  CHECK(CompileDb::db_dir_from_arg("compile_commands.json") == ".");
  CHECK(CompileDb::db_dir_from_arg("foo/compile_commands.json") == "foo/");
  CHECK(CompileDb::db_dir_from_arg("/a/b/compile_commands.json") == "/a/b/");
  CHECK(CompileDb::db_dir_from_arg("/a/b") == "/a/b");
  CHECK(CompileDb::db_dir_from_arg("some/dir") == "some/dir");
}

// parse_libclang_major / configured_libclang_library_path /
// warn_if_runtime_libclang_ignored tests removed with the libclang drop: those
// libclang-runtime helpers no longer exist (indexing is Clang C++ only).

// ---------------------------------------------------------------------------
// Portable-paths preserve rule (v14): strip_for_libclang with <label>/$VAR.
// ---------------------------------------------------------------------------

TEST_CASE("strip: -I value with '<' preserved verbatim (preserve rule)") {
  // Space form: -I <libfoo-include>/extra → do NOT absolutize
  const std::vector<std::string> argv = {"cc", "-I", "<libfoo-include>/extra",
                                         "a.c"};
  const auto out = CompileDb::strip_for_libclang(argv, "a.c", "/d");
  CHECK(out == std::vector<std::string>{"-I", "<libfoo-include>/extra"});
}

TEST_CASE("strip: -I value with '$' preserved verbatim (preserve rule)") {
  // Space form: -I $VAR/include → do NOT absolutize
  const std::vector<std::string> argv = {"cc", "-I", "$MYLIB/include", "a.c"};
  const auto out = CompileDb::strip_for_libclang(argv, "a.c", "/d");
  CHECK(out == std::vector<std::string>{"-I", "$MYLIB/include"});
}

TEST_CASE("strip: glued -I with '<' preserved verbatim") {
  // Glued form: -I<label>/inc → emit the entire original token
  const std::vector<std::string> argv = {"cc", "-I<libfoo-include>/inc",
                                         "a.c"};
  const auto out = CompileDb::strip_for_libclang(argv, "a.c", "/d");
  CHECK(out == std::vector<std::string>{"-I<libfoo-include>/inc"});
}

TEST_CASE("strip: -isystem with '$' preserved verbatim") {
  const std::vector<std::string> argv = {"cc", "-isystem", "$SYSROOT/include",
                                         "a.c"};
  const auto out = CompileDb::strip_for_libclang(argv, "a.c", "/d");
  CHECK(out == std::vector<std::string>{"-isystem", "$SYSROOT/include"});
}

// ---------------------------------------------------------------------------
// split_base_version (portable-paths §2)
// ---------------------------------------------------------------------------

TEST_CASE("split_base_version: version segment detected") {
  // /opt/libfoo/v1.2.3 → (/opt/libfoo, v1.2.3)
  const auto [base, seg] = CompileDb::split_base_version("/opt/libfoo/v1.2.3");
  CHECK(base == "/opt/libfoo");
  CHECK(seg == "v1.2.3");
}

TEST_CASE("split_base_version: bare numeric version detected") {
  const auto [base, seg] = CompileDb::split_base_version("/opt/mylib/1.0");
  CHECK(base == "/opt/mylib");
  CHECK(seg == "1.0");
}

TEST_CASE("split_base_version: no version segment returns root and empty") {
  // 'include' is not a version segment.
  const auto [base, seg] = CompileDb::split_base_version("/opt/mylib/include");
  CHECK(base == "/opt/mylib/include");
  CHECK(seg.empty());
}

TEST_CASE("split_base_version: empty base case (root is /seg)") {
  // base would be "" or "/": not accepted as versioned.
  const auto [base, seg] = CompileDb::split_base_version("/v1.0");
  CHECK(base == "/v1.0");
  CHECK(seg.empty());
}

TEST_CASE("split_base_version: normpath applied first") {
  // Trailing slash stripped.
  const auto [base, seg] = CompileDb::split_base_version("/opt/mylib/1.0/");
  CHECK(base == "/opt/mylib");
  CHECK(seg == "1.0");
}

TEST_CASE("split_base_version: _-separated version") {
  const auto [base, seg] = CompileDb::split_base_version("/opt/libfoo/1_2_3");
  CHECK(base == "/opt/libfoo");
  CHECK(seg == "1_2_3");
}

TEST_CASE("split_base_version: dot-separated version") {
  const auto [base, seg] = CompileDb::split_base_version("/opt/libfoo/2.14.0");
  CHECK(base == "/opt/libfoo");
  CHECK(seg == "2.14.0");
}

// ---------------------------------------------------------------------------
// libclang-dependent cases (label "clang"; runtime SKIP -> exit 77).
// ---------------------------------------------------------------------------

TEST_SUITE("clang") {

  // The libclang RAII-wrapper and linked-libclang-version tests were removed
  // with the libclang drop (CxString/CxOverriddenCursors and the runtime
  // version query no longer exist).

  TEST_CASE("CompileDb::load over manifests/compile_commands.json") {
    if (!require_manifests()) {
      return;
    }
    const std::string manifests = CIDX_MANIFESTS_DIR;
    const auto cmds = CompileDb::load(manifests + "/compile_commands.json");
    // Unified DB: every manifests TU lives here, so match by name rather than
    // a fixed count (fixtures are added over time — see CLAUDE.md).
    REQUIRE(cmds.size() >= 2);

    const CompileCommand &shapes = find_command(cmds, "shapes.c");
    CHECK(shapes.directory == manifests);
    CHECK(shapes.driver == "cc");
    CHECK(shapes.args == std::vector<std::string>{"-I" + manifests, "-std=c11",
                                                  "-DMAX_SHAPES=64"});

    const CompileCommand &calls = find_command(cmds, "calls.c");
    CHECK(calls.directory == manifests);
    CHECK(calls.driver == "cc");
    CHECK(calls.args == std::vector<std::string>{"-std=c11"});
  }

  TEST_CASE("CompileDb::load accepts the directory form of --db") {
    if (!require_manifests()) {
      return;
    }
    const std::string manifests = CIDX_MANIFESTS_DIR;
    const std::string project = manifests + "/project";
    const auto cmds = CompileDb::load(manifests); // directory form, no json name
    REQUIRE(cmds.size() >= 2);

    // mathlib.c/app.c live under project/ in the unified DB; their per-entry
    // directory + -I. still resolve against project/.
    const CompileCommand &mathlib = find_command(cmds, "mathlib.c");
    CHECK(mathlib.directory == project);
    CHECK(mathlib.driver == "cc");
    CHECK(mathlib.args == std::vector<std::string>{"-I" + project});

    const CompileCommand &app = find_command(cmds, "app.c");
    CHECK(app.args == std::vector<std::string>{"-I" + project});
  }

  TEST_CASE("CompileDb::load throws CidxError on a database-less directory") {
    CHECK_THROWS_AS(CompileDb::load("/nonexistent-cidx-db-dir"),
                    cidx::CidxError);
  }

} // TEST_SUITE("clang")

int main(int argc, char **argv) {
  doctest::Context ctx(argc, argv);
  const int res = ctx.run();
  if (ctx.shouldExit()) {
    return res;
  }
  // SKIP-77 only for fixture-gap (A1: libclang-absence can no longer occur).
  if (res == 0 && g_fixture_skipped) {
    return 77; // CTest SKIP_RETURN_CODE
  }
  return res;
}
