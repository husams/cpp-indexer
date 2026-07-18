// include_hygiene_test — the executable specification for `cidx include`
// (planning/cidx-include-hygiene, "Required tests").
//
// These cases are the safety bar, not a smoke test. The dangerous failure for
// this feature is not a crash; it is confidently deleting an #include that was
// doing something the symbol graph cannot see. So most of what follows asserts
// that a removal does NOT happen: X-macros, macro-only providers, transitive
// providers, conditional branches, unowned targets.
//
// Real parses, so the whole file is doctest suite "clang" (ctest label
// "clang"), excluded from the hermetic default gate.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <set>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "cli/args.hpp"
#include "cli/commands.hpp"
#include "include_hygiene/analysis.hpp"
#include "include_hygiene/graph.hpp"
#include "include_hygiene/plan.hpp"
#include "storage/storage.hpp"
#include "util/logger.hpp"

namespace cli = cidx::cli;
namespace hyg = cidx::hygiene;

namespace {

std::string make_temp_dir() {
  char tmpl[] = "/tmp/cidx_include_XXXXXX";
  char *d = ::mkdtemp(tmpl);
  REQUIRE(d != nullptr);
  return d;
}

void write_file(const std::string &path, const std::string &content) {
  std::ofstream f(path);
  REQUIRE(f.good());
  f << content;
}

std::string read_file(const std::string &path) {
  std::ifstream f(path);
  if (!f.good()) {
    return {};
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

int run_cidx(const std::vector<std::string> &argv, const std::string &cache,
             std::string *stdout_text = nullptr) {
  cli::ParsedArgs pa = cli::parse_args(argv);
  REQUIRE(!pa.help_text);
  std::ostringstream out, err;
  cidx::Logger log;
  log.set_file(cache + "/cidx.log");
  cli::Context ctx;
  ctx.cache_dir = cache;
  ctx.index_path = cache + "/index.db";
  ctx.logger = &log;
  ctx.out = &out;
  ctx.err = &err;
  const int rc = cli::run_command(pa, ctx);
  if (stdout_text != nullptr) {
    *stdout_text = out.str();
  }
  return rc;
}

// A project with one TU per source file, indexed and ready to analyze.
struct Project {
  std::string cache;
  std::string proj;

  Project() : cache(make_temp_dir()), proj(cache + "/proj") {
    ::mkdir(proj.c_str(), 0755);
  }

  void add(const std::string &name, const std::string &content) {
    write_file(proj + "/" + name, content);
  }

  // Index `sources` as translation units. Must be called after every add().
  void index(const std::vector<std::string> &sources,
             const std::string &std_flag = "-std=c++17") {
    std::string entries;
    for (std::size_t i = 0; i < sources.size(); ++i) {
      entries += std::string(i ? ",\n  " : "  ") + "{\"directory\": \"" + proj +
                 "\", \"command\": \"c++ " + std_flag + " -I. -c " + sources[i] +
                 "\", \"file\": \"" + proj + "/" + sources[i] + "\"}";
    }
    write_file(proj + "/compile_commands.json", "[\n" + entries + "\n]\n");
    REQUIRE(run_cidx({"import", "--db", proj, "--name", "fx"}, cache) == 0);
    REQUIRE(run_cidx({"index"}, cache) == 0);
  }

  hyg::AnalysisResult analyze(cidx::Storage &db) {
    hyg::AnalysisOptions opts;
    return hyg::analyze(db, opts);
  }

  std::string path(const std::string &name) const { return proj + "/" + name; }
};

// The finding for one directive, by source file and line.
//
// Returns by VALUE, and rvalue results are rejected at compile time: an earlier
// draft returned a pointer into the AnalysisResult, and `find_at(p.analyze(db),
// ...)` then dangled the moment the temporary died -- which showed up as
// plausible-looking wrong classifications rather than a crash. Don't let the
// signature allow that again.
std::optional<hyg::IncludeCandidate> find_at(const hyg::AnalysisResult &&,
                                             const std::string &, int64_t) =
    delete;

std::optional<hyg::IncludeCandidate> find_at(const hyg::AnalysisResult &r,
                                             const std::string &abs_path,
                                             int64_t line) {
  for (const hyg::IncludeCandidate &c : r.candidates) {
    if (c.src_path == abs_path && c.line == line) {
      return c;
    }
  }
  return std::nullopt;
}

} // namespace

TEST_SUITE("clang") {

TEST_CASE("unused: zero references makes an include unused_by_reference") {
  Project p;
  p.add("used.hpp", "#pragma once\nstruct Used { int f() const; };\n");
  p.add("unused.hpp", "#pragma once\nstruct Unused { int g() const; };\n");
  p.add("main.cpp", "#include \"used.hpp\"\n"      // line 1: used
                    "#include \"unused.hpp\"\n"    // line 2: unused
                    "int main() { Used u; return u.f(); }\n");
  p.index({"main.cpp"});

  cidx::Storage db(p.cache + "/index.db");
  const hyg::AnalysisResult r = p.analyze(db);

  const std::optional<hyg::IncludeCandidate> used = find_at(r, p.path("main.cpp"), 1);
  REQUIRE(used.has_value());
  CHECK(used->cls == hyg::Classification::Used);
  CHECK(used->intersection_count > 0);
  CHECK_FALSE(used->evidence.empty()); // a `used` verdict must show its work

  const std::optional<hyg::IncludeCandidate> unused = find_at(r, p.path("main.cpp"), 2);
  REQUIRE(unused.has_value());
  CHECK(unused->cls == hyg::Classification::UnusedByReference);
  CHECK(unused->intersection_count == 0);
  CHECK(unused->caveats.empty()); // nothing stops this one being executable
}

TEST_CASE("unused: a reference through a TRANSITIVE header does not use the "
          "direct include") {
  // main.cpp includes middle.hpp, which includes leaf.hpp. main uses Leaf only.
  // Symbols(middle.hpp) is what middle declares ITSELF -- Leaf belongs to
  // leaf.hpp -- so middle.hpp is unused even though the program needs it to
  // compile. That is the whole point of separating the reference claim from the
  // apply-safety gate: the finding is TRUE, and removing it would still break
  // the build, so validation must refuse to accept it.
  Project p;
  p.add("leaf.hpp", "#pragma once\nstruct Leaf { int v() const; };\n");
  p.add("middle.hpp", "#pragma once\n#include \"leaf.hpp\"\n"
                      "struct Middle { int m() const; };\n");
  p.add("main.cpp", "#include \"middle.hpp\"\n"
                    "int main() { Leaf l; return l.v(); }\n");
  p.index({"main.cpp"});

  cidx::Storage db(p.cache + "/index.db");
  const hyg::AnalysisResult r = p.analyze(db);

  const std::optional<hyg::IncludeCandidate> c = find_at(r, p.path("main.cpp"), 1);
  REQUIRE(c.has_value());
  CHECK(c->cls == hyg::Classification::UnusedByReference);
  CHECK(c->intersection_count == 0);
  // Leaf must NOT appear among middle.hpp's own symbols.
  CHECK(std::find(c->header_symbols.begin(), c->header_symbols.end(), "Leaf") ==
        c->header_symbols.end());

  // The safety gate has to catch it: removing it does not compile.
  const hyg::CleanupPlan plan = hyg::build_plan(db, r, p.cache + "/index.db");
  bool found = false;
  for (const hyg::PlanItem &it : plan.items) {
    if (it.line != 1) {
      continue;
    }
    found = true;
    CHECK(it.state != hyg::PlanState::Accepted); // never executable
  }
  CHECK(found);
}

TEST_CASE("unused: a .cpp including its OWN header is manual_review, not used") {
  // The most common include in C++, and the one the reference rule alone gets
  // wrong: `int f();` in u.hpp and `int f() { ... }` in u.cpp are ONE symbol,
  // in both Owners(u.cpp) and Symbols(u.hpp), and it never references itself,
  // so the reference intersection is genuinely empty. That does NOT make the
  // include `used` -- the verdict is reference-only. The declaration/definition
  // overlap is a safety CAVEAT: removing the directive still COMPILES (a
  // definition does not need its declaration), so the compile gate cannot catch
  // it either, and the finding must be manual_review, never executable.
  Project p;
  p.add("u.hpp", "#pragma once\nint helper(int x);\n");
  p.add("u.cpp", "#include \"u.hpp\"\nint helper(int x) { return x + 1; }\n");
  p.index({"u.cpp"});

  cidx::Storage db(p.cache + "/index.db");
  const hyg::AnalysisResult r = p.analyze(db);
  const std::optional<hyg::IncludeCandidate> c = find_at(r, p.path("u.cpp"), 1);
  REQUIRE(c.has_value());
  CHECK(c->cls == hyg::Classification::ManualReview);
  CHECK(c->intersection_count == 0); // the reference intersection IS empty
  const bool own_header_caveat =
      std::any_of(c->caveats.begin(), c->caveats.end(), [](const std::string &w) {
        return w.find("own header") != std::string::npos;
      });
  CHECK(own_header_caveat); // and it says WHY it is not auto-removable

  // Belt and braces: it must never reach a plan as executable.
  for (const hyg::PlanItem &it :
       hyg::build_plan(db, r, p.cache + "/index.db").items) {
    const bool self_header_accepted =
        it.line == 1 && it.state == hyg::PlanState::Accepted;
    CHECK_FALSE(self_header_accepted);
  }
}

TEST_CASE("unused: a signature-only reference (const Foo&) uses the include") {
  // No body, no call -- Foo appears only in a parameter type. The signature
  // tier's type closure is what makes this a reference.
  Project p;
  p.add("foo.hpp", "#pragma once\nstruct Foo { int x; };\n");
  p.add("main.cpp", "#include \"foo.hpp\"\n"
                    "void take(const Foo &f);\n"
                    "int main() { return 0; }\n");
  p.index({"main.cpp"});

  cidx::Storage db(p.cache + "/index.db");
  const hyg::AnalysisResult r = p.analyze(db);
  const std::optional<hyg::IncludeCandidate> c = find_at(r, p.path("main.cpp"), 1);
  REQUIRE(c.has_value());
  CHECK(c->cls == hyg::Classification::Used);
}

TEST_CASE("unused: an inheritance-only reference uses the include") {
  Project p;
  p.add("base.hpp", "#pragma once\nstruct Base { virtual ~Base(); };\n");
  p.add("main.cpp", "#include \"base.hpp\"\n"
                    "struct Derived : Base {};\n"
                    "int main() { return 0; }\n");
  p.index({"main.cpp"});

  cidx::Storage db(p.cache + "/index.db");
  const hyg::AnalysisResult r = p.analyze(db);
  const std::optional<hyg::IncludeCandidate> c = find_at(r, p.path("main.cpp"), 1);
  REQUIRE(c.has_value());
  CHECK(c->cls == hyg::Classification::Used);
}

TEST_CASE("macro-only dependency is never auto-removed") {
  // The header supplies ONLY a macro. Zero symbol references -- exactly what an
  // unused include looks like -- yet it is genuinely required. Without the
  // recorder's macro tracking this is the case that would silently break
  // builds, so it must be manual_review, never executable.
  Project p;
  p.add("macros.hpp", "#pragma once\n#define ANSWER 42\n");
  p.add("main.cpp", "#include \"macros.hpp\"\n"
                    "int main() { return ANSWER; }\n");
  p.index({"main.cpp"});

  cidx::Storage db(p.cache + "/index.db");
  const hyg::AnalysisResult r = p.analyze(db);
  const std::optional<hyg::IncludeCandidate> c = find_at(r, p.path("main.cpp"), 1);
  REQUIRE(c.has_value());
  CHECK(c->cls == hyg::Classification::ManualReview);
  CHECK(c->intersection_count == 0); // the reference claim IS zero...
  // ...but the macro use is recorded, which is what blocks the removal.
  REQUIRE(c->macro_uses.size() == 1);
  CHECK(c->macro_uses.front() == "ANSWER");

  for (const hyg::PlanItem &it :
       hyg::build_plan(db, r, p.cache + "/index.db").items) {
    CHECK(it.state != hyg::PlanState::Accepted);
  }
}

TEST_CASE("duplicate: a repeated GUARDED include is a duplicate; the first is "
          "kept") {
  Project p;
  p.add("g.hpp", "#pragma once\nstruct G { int f() const; };\n");
  p.add("main.cpp", "#include \"g.hpp\"\n"   // line 1: first, kept
                    "#include \"g.hpp\"\n"   // line 2: duplicate
                    "int main() { G g; return g.f(); }\n");
  p.index({"main.cpp"});

  cidx::Storage db(p.cache + "/index.db");
  const hyg::AnalysisResult r = p.analyze(db);

  const std::optional<hyg::IncludeCandidate> first = find_at(r, p.path("main.cpp"), 1);
  REQUIRE(first.has_value());
  CHECK(first->cls != hyg::Classification::Duplicate); // never the first one

  const std::optional<hyg::IncludeCandidate> dup = find_at(r, p.path("main.cpp"), 2);
  REQUIRE(dup.has_value());
  CHECK(dup->cls == hyg::Classification::Duplicate);
  CHECK(dup->guarded);
}

TEST_CASE("duplicate: a repeated UNGUARDED (X-macro) include is NEVER "
          "auto-removed") {
  // The second include is the entire point of the X-macro pattern: the header
  // is re-read to expand a differently-defined macro. Removing it changes the
  // program, and it may well still compile -- so guardedness, not compilation,
  // has to be the gate.
  Project p;
  p.add("items.def", "X(alpha)\nX(beta)\n"); // no guard, re-read on purpose
  p.add("main.cpp", "#define X(n) int n##_first();\n"
                    "#include \"items.def\"\n" // line 2
                    "#undef X\n"
                    "#define X(n) int n##_second();\n"
                    "#include \"items.def\"\n" // line 5: NOT a duplicate
                    "#undef X\n"
                    "int main() { return 0; }\n");
  p.index({"main.cpp"});

  cidx::Storage db(p.cache + "/index.db");
  const hyg::AnalysisResult r = p.analyze(db);

  const std::optional<hyg::IncludeCandidate> second = find_at(r, p.path("main.cpp"), 5);
  REQUIRE(second.has_value());
  CHECK_FALSE(second->guarded);
  CHECK(second->cls == hyg::Classification::ManualReview);
  CHECK(second->cls != hyg::Classification::Duplicate);

  // The FIRST occurrence matters just as much. Its declarations are referenced
  // by nothing and the file compiles without it, so both the reference rule and
  // the compile gate would happily delete it -- silently dropping the
  // alpha_first/beta_first declarations. Guardedness is the only sound gate.
  const std::optional<hyg::IncludeCandidate> first = find_at(r, p.path("main.cpp"), 2);
  REQUIRE(first.has_value());
  CHECK(first->intersection_count == 0); // looks exactly like an unused include
  CHECK(first->cls == hyg::Classification::ManualReview);

  for (const hyg::PlanItem &it :
       hyg::build_plan(db, r, p.cache + "/index.db").items) {
    CHECK(it.state != hyg::PlanState::Accepted);
  }
}

TEST_CASE("conditional: an include inside #if is only ever proven for the "
          "configuration that lexed it") {
  Project p;
  p.add("cond.hpp", "#pragma once\nstruct Cond { int f() const; };\n");
  p.add("main.cpp", "#ifdef FEATURE\n"
                    "#include \"cond.hpp\"\n" // line 2
                    "#endif\n"
                    "int main() { return 0; }\n");
  p.index({"main.cpp"}, "-std=c++17 -DFEATURE");

  cidx::Storage db(p.cache + "/index.db");
  const hyg::AnalysisResult r = p.analyze(db);
  const std::optional<hyg::IncludeCandidate> c = find_at(r, p.path("main.cpp"), 2);
  REQUIRE(c.has_value());
  // Zero references, but inside a conditional region: the verdict holds for
  // THIS configuration only, so it must not be executable.
  CHECK_FALSE(c->cond_fingerprint.empty());
  CHECK(c->cls == hyg::Classification::ManualReview);
  const bool warned =
      std::any_of(c->caveats.begin(), c->caveats.end(), [](const std::string &w) {
        return w.find("conditional") != std::string::npos;
      });
  CHECK(warned);
}

TEST_CASE("system headers are reported but never auto-removed") {
  Project p;
  p.add("main.cpp", "#include <string>\n"
                    "int main() { return 0; }\n");
  p.index({"main.cpp"});

  cidx::Storage db(p.cache + "/index.db");
  const hyg::AnalysisResult r = p.analyze(db);
  const std::optional<hyg::IncludeCandidate> c = find_at(r, p.path("main.cpp"), 1);
  REQUIRE(c.has_value());
  CHECK(c->is_angled);
  CHECK(c->cls == hyg::Classification::ManualReview);
}

TEST_CASE("the removal range covers the whole line and its trailing comment") {
  Project p;
  p.add("u.hpp", "#pragma once\nstruct U {};\n");
  p.add("main.cpp", "#include \"u.hpp\"   // why it is here\n"
                    "int main() { return 0; }\n");
  p.index({"main.cpp"});

  cidx::Storage db(p.cache + "/index.db");
  const hyg::AnalysisResult r = p.analyze(db);
  const std::optional<hyg::IncludeCandidate> c = find_at(r, p.path("main.cpp"), 1);
  REQUIRE(c.has_value());
  // The comment annotates the directive, so it travels with it -- and the
  // range ends past the newline, leaving no blank line behind.
  CHECK(c->begin_offset == 0);
  CHECK(c->directive_text == "#include \"u.hpp\"   // why it is here\n");
}

TEST_CASE("apply: removes only the planned bytes and leaves a compiling tree") {
  Project p;
  p.add("used.hpp", "#pragma once\nstruct Used { int f() const; };\n");
  p.add("unused.hpp", "#pragma once\nstruct Unused {};\n");
  p.add("main.cpp", "#include \"used.hpp\"\n"
                    "#include \"unused.hpp\"\n"
                    "int main() { Used u; return u.f(); }\n");
  p.index({"main.cpp"});

  const std::string plan_path = p.cache + "/plan.json";
  REQUIRE(run_cidx({"include", "plan", "--output", plan_path}, p.cache) == 0);
  REQUIRE(run_cidx({"include", "apply", plan_path}, p.cache) == 0);

  const std::string after = read_file(p.path("main.cpp"));
  CHECK(after.find("used.hpp") != std::string::npos);      // kept
  CHECK(after.find("unused.hpp") == std::string::npos);    // gone
  CHECK(after.find("int main()") != std::string::npos);    // code untouched
}

TEST_CASE("apply: --dry-run writes nothing") {
  Project p;
  p.add("unused.hpp", "#pragma once\nstruct Unused {};\n");
  p.add("main.cpp", "#include \"unused.hpp\"\nint main() { return 0; }\n");
  p.index({"main.cpp"});

  const std::string before = read_file(p.path("main.cpp"));
  const std::string plan_path = p.cache + "/plan.json";
  REQUIRE(run_cidx({"include", "plan", "--output", plan_path}, p.cache) == 0);
  REQUIRE(run_cidx({"include", "apply", plan_path, "--dry-run"}, p.cache) == 0);
  CHECK(read_file(p.path("main.cpp")) == before);
}

TEST_CASE("apply: refuses a stale plan and leaves the file alone") {
  Project p;
  p.add("unused.hpp", "#pragma once\nstruct Unused {};\n");
  p.add("main.cpp", "#include \"unused.hpp\"\nint main() { return 0; }\n");
  p.index({"main.cpp"});

  const std::string plan_path = p.cache + "/plan.json";
  REQUIRE(run_cidx({"include", "plan", "--output", plan_path}, p.cache) == 0);

  // The world moves after the plan was reviewed.
  write_file(p.path("main.cpp"),
             "#include \"unused.hpp\"\nint main() { return 1; }\n");
  const std::string before = read_file(p.path("main.cpp"));

  CHECK(run_cidx({"include", "apply", plan_path}, p.cache) == 1);
  CHECK(read_file(p.path("main.cpp")) == before); // untouched
}

TEST_CASE("apply: --only applies just the named item") {
  Project p;
  p.add("a.hpp", "#pragma once\nstruct A {};\n");
  p.add("b.hpp", "#pragma once\nstruct B {};\n");
  p.add("main.cpp", "#include \"a.hpp\"\n#include \"b.hpp\"\n"
                    "int main() { return 0; }\n");
  p.index({"main.cpp"});

  cidx::Storage db(p.cache + "/index.db");
  const hyg::CleanupPlan plan =
      hyg::build_plan(db, p.analyze(db), p.cache + "/index.db");
  std::string a_id;
  for (const hyg::PlanItem &it : plan.items) {
    if (it.line == 1 && it.state == hyg::PlanState::Accepted) {
      a_id = it.id;
    }
  }
  REQUIRE_FALSE(a_id.empty());

  const std::string plan_path = p.cache + "/plan.json";
  REQUIRE(run_cidx({"include", "plan", "--output", plan_path}, p.cache) == 0);
  REQUIRE(run_cidx({"include", "apply", plan_path, "--only", a_id}, p.cache) ==
          0);

  const std::string after = read_file(p.path("main.cpp"));
  CHECK(after.find("a.hpp") == std::string::npos); // the selected one went
  CHECK(after.find("b.hpp") != std::string::npos); // the other stayed
}

TEST_CASE("apply: an unknown --only id is refused, not ignored") {
  Project p;
  p.add("unused.hpp", "#pragma once\nstruct Unused {};\n");
  p.add("main.cpp", "#include \"unused.hpp\"\nint main() { return 0; }\n");
  p.index({"main.cpp"});

  const std::string plan_path = p.cache + "/plan.json";
  REQUIRE(run_cidx({"include", "plan", "--output", plan_path}, p.cache) == 0);
  const std::string before = read_file(p.path("main.cpp"));
  CHECK(run_cidx({"include", "apply", plan_path, "--only", "deadbeef"},
                 p.cache) == 1);
  CHECK(read_file(p.path("main.cpp")) == before);
}

TEST_CASE("plan paths are repo-relative under a real common root") {
  // repo_root must be the directory the sources actually share, not "/". An
  // earlier byte-wise prefix walk collapsed it to "/", which still *worked* --
  // join("/", "Users/...") round-trips -- while quietly making every path in
  // the artifact absolute-minus-slash, so the plan was neither reviewable in a
  // diff nor portable between clones. Both are the point of storing it
  // relative.
  Project p;
  p.add("a.hpp", "#pragma once\nstruct A {};\n");
  p.add("main.cpp", "#include \"a.hpp\"\nint main() { return 0; }\n");
  p.index({"main.cpp"});

  cidx::Storage db(p.cache + "/index.db");
  const hyg::CleanupPlan plan =
      hyg::build_plan(db, p.analyze(db), p.cache + "/index.db");

  CHECK(plan.repo_root == p.proj);
  REQUIRE_FALSE(plan.items.empty());
  for (const hyg::PlanItem &it : plan.items) {
    CHECK(it.file.find('/') == std::string::npos); // "main.cpp", not a path
    CHECK(it.file.front() != '/');
  }
  for (const auto &[file, _] : plan.file_hashes) {
    CHECK(file.front() != '/');
  }
}

TEST_CASE("plan JSON is deterministic and round-trips") {
  Project p;
  p.add("a.hpp", "#pragma once\nstruct A {};\n");
  p.add("main.cpp", "#include \"a.hpp\"\nint main() { return 0; }\n");
  p.index({"main.cpp"});

  cidx::Storage db(p.cache + "/index.db");
  const hyg::AnalysisResult r = p.analyze(db);
  hyg::CleanupPlan p1 = hyg::build_plan(db, r, p.cache + "/index.db");
  hyg::CleanupPlan p2 = hyg::build_plan(db, r, p.cache + "/index.db");
  // resolved_at is a wall-clock stamp; everything else must be identical.
  p2.resolved_at = p1.resolved_at;
  CHECK(hyg::serialize(p1) == hyg::serialize(p2));

  const hyg::CleanupPlan back = hyg::deserialize(hyg::serialize(p1));
  CHECK(hyg::serialize(back) == hyg::serialize(p1));
}

TEST_CASE("graph: direct, reverse, and transitive queries") {
  Project p;
  p.add("leaf.hpp", "#pragma once\nstruct Leaf {};\n");
  p.add("middle.hpp", "#pragma once\n#include \"leaf.hpp\"\nstruct Middle {};\n");
  p.add("main.cpp", "#include \"middle.hpp\"\n"
                    "int main() { Leaf l; Middle m; (void)l; (void)m; return 0; }\n");
  p.index({"main.cpp"});

  cidx::Storage db(p.cache + "/index.db");
  const hyg::IncludeGraph g = hyg::IncludeGraph::load(db, false);

  const std::vector<hyg::GraphEdge> direct = g.direct_from(p.path("main.cpp"));
  REQUIRE(direct.size() == 1);
  CHECK(direct.front().dst_path == p.path("middle.hpp"));

  // main.cpp reaches leaf.hpp only through middle.hpp.
  const std::vector<std::string> reach = g.transitive_from(p.path("main.cpp"), 0);
  CHECK(std::find(reach.begin(), reach.end(), p.path("leaf.hpp")) != reach.end());

  // ...and leaf.hpp's impact set reaches back to main.cpp.
  const std::vector<std::string> impact = g.transitive_to(p.path("leaf.hpp"), 0);
  CHECK(std::find(impact.begin(), impact.end(), p.path("main.cpp")) !=
        impact.end());

  const std::vector<std::string> why =
      g.shortest_path(p.path("main.cpp"), p.path("leaf.hpp"));
  REQUIRE(why.size() == 3); // main -> middle -> leaf
  CHECK(why.front() == p.path("main.cpp"));
  CHECK(why.back() == p.path("leaf.hpp"));

  CHECK(g.cycles().empty());
}

TEST_CASE("a header included by two TUs is validated through both") {
  Project p;
  p.add("shared.hpp", "#pragma once\nstruct Shared { int f() const; };\n");
  p.add("unused.hpp", "#pragma once\nstruct Unused {};\n");
  // Both TUs include unused.hpp; a removal in a.cpp only affects a.cpp, but the
  // affected-TU set for shared.hpp must name both.
  p.add("a.cpp", "#include \"shared.hpp\"\n#include \"unused.hpp\"\n"
                 "int fa() { Shared s; return s.f(); }\n");
  p.add("b.cpp", "#include \"shared.hpp\"\n"
                 "int fb() { Shared s; return s.f(); }\n");
  p.index({"a.cpp", "b.cpp"});

  cidx::Storage db(p.cache + "/index.db");
  const hyg::IncludeGraph g = hyg::IncludeGraph::load(db, false);
  const std::vector<std::string> dependants = g.transitive_to(p.path("shared.hpp"), 0);
  CHECK(std::find(dependants.begin(), dependants.end(), p.path("a.cpp")) !=
        dependants.end());
  CHECK(std::find(dependants.begin(), dependants.end(), p.path("b.cpp")) !=
        dependants.end());
}

TEST_CASE("aggregate: one physical directive under two configs is one candidate") {
  // shared.hpp -> unused.hpp is a single physical directive, but shared.hpp is
  // parsed by two TUs under two DISTINCT configurations (different -D flags,
  // hence different digests). With per-config facts restored, that must surface
  // as ONE candidate whose config set is the union -- not one item per config.
  Project p;
  p.add("unused.hpp", "#pragma once\nstruct Unused {};\n");
  p.add("shared.hpp", "#pragma once\n#include \"unused.hpp\"\n"
                      "struct Shared { int f() const; };\n");
  p.add("a.cpp", "#include \"shared.hpp\"\nint fa() { Shared s; return s.f(); }\n");
  p.add("b.cpp", "#include \"shared.hpp\"\nint fb() { Shared s; return s.f(); }\n");
  // Hand-write compile_commands with a different -D per TU so the two
  // configurations do not collapse to one digest.
  write_file(p.proj + "/compile_commands.json",
             "[\n  {\"directory\": \"" + p.proj +
                 "\", \"command\": \"c++ -std=c++17 -DA=1 -I. -c a.cpp\", "
                 "\"file\": \"" + p.proj + "/a.cpp\"},\n"
                 "  {\"directory\": \"" + p.proj +
                 "\", \"command\": \"c++ -std=c++17 -DB=1 -I. -c b.cpp\", "
                 "\"file\": \"" + p.proj + "/b.cpp\"}\n]\n");
  REQUIRE(run_cidx({"import", "--db", p.proj, "--name", "fx"}, p.cache) == 0);
  REQUIRE(run_cidx({"index"}, p.cache) == 0);

  cidx::Storage db(p.cache + "/index.db");
  const hyg::AnalysisResult r = p.analyze(db);
  int count = 0;
  std::size_t configs = 0;
  for (const hyg::IncludeCandidate &c : r.candidates) {
    if (c.src_path == p.path("shared.hpp") && c.line == 2) {
      ++count;
      configs = c.configs.size();
    }
  }
  CHECK(count == 1);      // exactly one candidate for the one directive
  CHECK(configs == 2);    // both TU configurations, unioned onto it
}

TEST_CASE("a TU that includes nothing still reports the tier as populated") {
  // A config row is written for every TU processed, so a project that happens to
  // include nothing has configs but zero edges -- and must NOT be mistaken for
  // a DB that predates the tier (the vacuous "nothing to report" refusal).
  Project p;
  p.add("main.cpp", "int main() { return 0; }\n");
  p.index({"main.cpp"});

  cidx::Storage db(p.cache + "/index.db");
  CHECK(db.include_graph_populated());
  const hyg::AnalysisResult r = hyg::analyze(db, hyg::AnalysisOptions{});
  CHECK_FALSE(r.include_graph_empty);
}

TEST_CASE("include facts: retiring a TU's configuration spares other TUs") {
  // delete_include_configs_for_tu removes only the named TU's configuration(s)
  // and their edges (a changed compile command's obsolete digest), while a
  // shared header's facts recorded under another TU survive.
  const std::string tmp = make_temp_dir();
  cidx::Storage db(tmp + "/i.db");
  const int64_t comp = db.add_component("c", tmp);
  const int64_t dir = db.add_directory(comp, tmp);
  const int64_t tu_a = db.add_file(dir, "a.cpp");
  const int64_t tu_b = db.add_file(dir, "b.cpp");
  const int64_t hdr = db.add_file(dir, "shared.hpp");

  cidx::IncludeConfig ca;
  ca.tu_file_id = tu_a;
  ca.digest = "AAAA";
  const int64_t cid_a = db.add_include_config(ca);
  cidx::IncludeConfig cb;
  cb.tu_file_id = tu_b;
  cb.digest = "BBBB";
  const int64_t cid_b = db.add_include_config(cb);

  cidx::IncludeEdge ea;
  ea.src_file_id = hdr;
  ea.dst_path = tmp + "/leaf.hpp";
  ea.config_id = cid_a;
  db.add_include_edge(ea);
  cidx::IncludeEdge eb;
  eb.src_file_id = hdr;
  eb.dst_path = tmp + "/leaf.hpp";
  eb.config_id = cid_b;
  db.add_include_edge(eb);
  REQUIRE(db.include_edges_from(hdr, true).size() == 2);

  db.delete_include_configs_for_tu(tu_a);
  const std::vector<cidx::IncludeEdge> after = db.include_edges_from(hdr, true);
  REQUIRE(after.size() == 1);         // A's edge retired via config cascade
  CHECK(after.front().config_id == cid_b); // B's edge survives
}

TEST_CASE("apply: refuses a hard-linked source") {
  // rename(2) would give the edit a new inode and leave the file's other name
  // pointing at the old content -- a silent divergence no per-inode copy fixes.
  Project p;
  p.add("unused.hpp", "#pragma once\nstruct Unused {};\n");
  p.add("main.cpp", "#include \"unused.hpp\"\nint main() { return 0; }\n");
  REQUIRE(::link((p.proj + "/main.cpp").c_str(),
                 (p.proj + "/twin.cpp").c_str()) == 0);
  p.index({"main.cpp"});

  const std::string plan_path = p.cache + "/plan.json";
  REQUIRE(run_cidx({"include", "plan", "--output", plan_path}, p.cache) == 0);
  const std::string before = read_file(p.path("main.cpp"));
  CHECK(run_cidx({"include", "apply", plan_path}, p.cache) == 1);
  CHECK(read_file(p.path("main.cpp")) == before);
}

TEST_CASE("an index with no include facts refuses rather than reporting zero") {
  // A v30 database upgraded to v31 has an EMPTY include graph until a reindex.
  // "No unused includes" would be a vacuous truth that reads exactly like a
  // clean bill of health -- the most dangerous possible answer.
  const std::string tmp = make_temp_dir();
  cidx::Storage db(tmp + "/empty.db");
  db.add_component("c", "/data/c");

  const hyg::AnalysisResult r = hyg::analyze(db, hyg::AnalysisOptions{});
  CHECK(r.include_graph_empty);
  CHECK(r.candidates.empty());
}

TEST_CASE("conditional: an #ifdef on a macro from a header is never unused") {
  // The header supplies no referenced SYMBOL, so the reference rule alone would
  // call it unused. But #ifdef FEATURE selects a branch on the macro's
  // definedness -- removing the header flips the branch and still compiles --
  // so the dependency must be recorded and the finding forced to manual_review.
  Project p;
  p.add("feature.hpp", "#pragma once\n#define FEATURE 1\n");
  p.add("main.cpp", "#include \"feature.hpp\"\n"
                    "#ifdef FEATURE\nint on() { return 1; }\n#endif\n"
                    "int main() { return 0; }\n");
  p.index({"main.cpp"});

  cidx::Storage db(p.cache + "/index.db");
  const hyg::AnalysisResult r = p.analyze(db);
  const std::optional<hyg::IncludeCandidate> c =
      find_at(r, p.path("main.cpp"), 1);
  REQUIRE(c.has_value());
  CHECK(c->cls == hyg::Classification::ManualReview);
  const bool has_macro =
      std::find(c->macro_uses.begin(), c->macro_uses.end(), "FEATURE") !=
      c->macro_uses.end();
  CHECK(has_macro);
}

TEST_CASE("include facts: a header included by two TUs keeps facts under both "
          "configurations") {
  // shared.hpp is itself a source of includes (it pulls leaf.hpp) and is parsed
  // once per TU, under a different build configuration each time. The old
  // wholesale delete-by-src-file wiped the first TU's row when the second was
  // recorded, collapsing the graph to last-TU-wins. The refresh is per
  // configuration now, so both survive.
  Project p;
  p.add("leaf.hpp", "#pragma once\nstruct Leaf {};\n");
  p.add("shared.hpp", "#pragma once\n#include \"leaf.hpp\"\nstruct Shared {};\n");
  p.add("a.cpp", "#include \"shared.hpp\"\nint fa() { Shared s; (void)s; return 0; }\n");
  p.add("b.cpp", "#include \"shared.hpp\"\nint fb() { Shared s; (void)s; return 0; }\n");
  p.index({"a.cpp", "b.cpp"});

  cidx::Storage db(p.cache + "/index.db");
  const std::optional<cidx::File> shared = db.get_file(p.path("shared.hpp"));
  REQUIRE(shared.has_value());
  int leaf_edges = 0;
  std::set<int64_t> configs;
  for (const cidx::IncludeEdge &e : db.include_edges_from(shared->id, true)) {
    if (e.dst_path == p.path("leaf.hpp")) {
      ++leaf_edges;
      configs.insert(e.config_id);
    }
  }
  CHECK(leaf_edges == 2);   // one per TU configuration
  CHECK(configs.size() == 2);
}

TEST_CASE("scope: a path the include tier never indexed is reported uncovered") {
  // Reporting zero findings for an un-indexed path reads exactly like a clean
  // bill of health. A covered path must NOT be flagged; an unknown one must.
  Project p;
  p.add("unused.hpp", "#pragma once\nstruct Unused {};\n");
  p.add("main.cpp", "#include \"unused.hpp\"\nint main() { return 0; }\n");
  p.index({"main.cpp"});

  cidx::Storage db(p.cache + "/index.db");
  hyg::AnalysisOptions opts;
  opts.scope_paths = {p.path("main.cpp"), p.path("ghost.cpp")};
  const hyg::AnalysisResult r = hyg::analyze(db, opts);
  CHECK(std::find(r.uncovered_scope.begin(), r.uncovered_scope.end(),
                  p.path("ghost.cpp")) != r.uncovered_scope.end());
  CHECK(std::find(r.uncovered_scope.begin(), r.uncovered_scope.end(),
                  p.path("main.cpp")) == r.uncovered_scope.end());
}

TEST_CASE("apply: a plan that promotes a USED include is refused") {
  // The plan is editable. Re-pointing an accepted item at a `used` directive --
  // with matching bytes and hash so the byte and staleness checks pass -- must
  // still be caught, because apply re-derives the classification from the DB
  // rather than trusting the plan's state.
  Project p;
  p.add("used.hpp", "#pragma once\nstruct Used { int f() const; };\n");
  p.add("unused.hpp", "#pragma once\nstruct Unused {};\n");
  p.add("main.cpp", "#include \"used.hpp\"\n#include \"unused.hpp\"\n"
                    "int main() { Used u; return u.f(); }\n");
  p.index({"main.cpp"});

  cidx::Storage db(p.cache + "/index.db");
  hyg::CleanupPlan plan =
      hyg::build_plan(db, p.analyze(db), p.cache + "/index.db");
  const hyg::AnalysisResult r = p.analyze(db);
  const std::optional<hyg::IncludeCandidate> used =
      find_at(r, p.path("main.cpp"), 1);
  REQUIRE(used.has_value());
  REQUIRE(used->cls == hyg::Classification::Used);

  bool tampered = false;
  for (hyg::PlanItem &it : plan.items) {
    if (it.state == hyg::PlanState::Accepted) {
      it.id = used->id;
      it.line = used->line;
      it.col = used->col;
      it.begin_offset = used->begin_offset;
      it.end_offset = used->end_offset;
      it.directive_text = used->directive_text;
      tampered = true;
    }
  }
  REQUIRE(tampered);

  const std::string plan_path = p.cache + "/tampered.json";
  write_file(plan_path, hyg::serialize(plan));
  const std::string before = read_file(p.path("main.cpp"));
  CHECK(run_cidx({"include", "apply", plan_path}, p.cache) == 1);
  CHECK(read_file(p.path("main.cpp")) == before); // the used include stays
}

TEST_CASE("apply: refuses an item whose file hash the plan omits") {
  Project p;
  p.add("unused.hpp", "#pragma once\nstruct Unused {};\n");
  p.add("main.cpp", "#include \"unused.hpp\"\nint main() { return 0; }\n");
  p.index({"main.cpp"});

  cidx::Storage db(p.cache + "/index.db");
  hyg::CleanupPlan plan =
      hyg::build_plan(db, p.analyze(db), p.cache + "/index.db");
  plan.file_hashes.clear(); // tamper: strip the evidence the staleness gate needs

  const std::string plan_path = p.cache + "/nohash.json";
  write_file(plan_path, hyg::serialize(plan));
  const std::string before = read_file(p.path("main.cpp"));
  CHECK(run_cidx({"include", "apply", plan_path}, p.cache) == 1);
  CHECK(read_file(p.path("main.cpp")) == before);
}

TEST_CASE("apply: refuses an item whose path escapes the repository root") {
  Project p;
  p.add("unused.hpp", "#pragma once\nstruct Unused {};\n");
  p.add("main.cpp", "#include \"unused.hpp\"\nint main() { return 0; }\n");
  p.index({"main.cpp"});

  cidx::Storage db(p.cache + "/index.db");
  hyg::CleanupPlan plan =
      hyg::build_plan(db, p.analyze(db), p.cache + "/index.db");
  for (hyg::PlanItem &it : plan.items) {
    if (it.state == hyg::PlanState::Accepted) {
      it.file = "../escape.cpp"; // tamper: point outside repo_root
    }
  }
  const std::string plan_path = p.cache + "/escape.json";
  write_file(plan_path, hyg::serialize(plan));
  const std::string before = read_file(p.path("main.cpp"));
  CHECK(run_cidx({"include", "apply", plan_path}, p.cache) == 1);
  CHECK(read_file(p.path("main.cpp")) == before);
}

TEST_CASE("apply: refuses to edit a symlinked source") {
  // rename(2) would replace the symlink with a regular file, silently detaching
  // it from its target (which could even lie outside the repo). Refuse instead.
  Project p;
  p.add("unused.hpp", "#pragma once\nstruct Unused {};\n");
  p.add("real.cpp", "#include \"unused.hpp\"\nint main() { return 0; }\n");
  REQUIRE(::symlink((p.proj + "/real.cpp").c_str(),
                    (p.proj + "/main.cpp").c_str()) == 0);
  p.index({"main.cpp"});

  const std::string plan_path = p.cache + "/plan.json";
  REQUIRE(run_cidx({"include", "plan", "--output", plan_path}, p.cache) == 0);
  const std::string before = read_file(p.path("real.cpp"));
  CHECK(run_cidx({"include", "apply", plan_path}, p.cache) == 1);

  struct stat lst {};
  REQUIRE(::lstat(p.path("main.cpp").c_str(), &lst) == 0);
  CHECK(S_ISLNK(lst.st_mode));                     // still a symlink
  CHECK(read_file(p.path("real.cpp")) == before);  // target untouched
}

TEST_CASE("apply: preserves the source file's permissions") {
  Project p;
  p.add("unused.hpp", "#pragma once\nstruct Unused {};\n");
  p.add("main.cpp", "#include \"unused.hpp\"\nint main() { return 0; }\n");
  p.index({"main.cpp"});
  // An executable, group-writable source keeps its mode through the atomic swap.
  REQUIRE(::chmod(p.path("main.cpp").c_str(), 0754) == 0);

  const std::string plan_path = p.cache + "/plan.json";
  REQUIRE(run_cidx({"include", "plan", "--output", plan_path}, p.cache) == 0);
  REQUIRE(run_cidx({"include", "apply", plan_path}, p.cache) == 0);

  struct stat st {};
  REQUIRE(::stat(p.path("main.cpp").c_str(), &st) == 0);
  CHECK((st.st_mode & 07777) == 0754);
  CHECK(read_file(p.path("main.cpp")).find("unused.hpp") == std::string::npos);
}

} // TEST_SUITE("clang")
