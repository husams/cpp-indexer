// S-074: the DATABASE end of bounded parallel extraction.
//
// tests/parallel_extraction_test.cpp proves the generic runner publishes in
// dispatch order under a randomised completion permutation. That is the
// scheduler's contract, not the index's: it says nothing about what the
// controlled writer actually wrote. These cases close that gap by indexing a
// real corpus twice -- serially, and in parallel with worker completion forced
// into the exact REVERSE of the dispatch order -- and comparing normalized,
// ordered projections of the Layer-0 tables.
//
// The corpus is chosen so the properties at risk are all exercised:
//   * one header shared by every translation unit, so owned-header
//     amortisation has to survive concurrent discovery;
//   * calls from each unit into that shared header, so cross-translation-unit
//     symbol identity has to resolve for units whose header work was claimed
//     by another unit -- the facts that vanish if identity is resolved from a
//     pre-run snapshot instead of at publication;
//   * the same USR declared in more than one unit, so duplicate-USR handling
//     cannot become completion-order dependent.
//
// Real parses, so this carries the "clang" label.
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

#include <sys/stat.h>

#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "cli/args.hpp"
#include "cli/commands.hpp"
#include "index/parallel_index.hpp"
#include "storage/sqlite.hpp"
#include "storage/storage.hpp"
#include "util/logger.hpp"

using cidx::SqliteDb;
using cidx::SqliteStmt;
using cidx::Storage;
namespace cli = cidx::cli;

namespace {

constexpr std::size_t kTranslationUnits = 4;

std::string make_temp_dir() {
  char tmpl[] = "/tmp/cidx_parallel_db_XXXXXX";
  char *dir = ::mkdtemp(tmpl);
  REQUIRE(dir != nullptr);
  return dir;
}

void write_file(const std::string &path, const std::string &content) {
  std::ofstream out(path);
  REQUIRE(out.good());
  out << content;
}

std::string replace_all(std::string text, const std::string &from,
                        const std::string &to) {
  if (from.empty()) {
    return text;
  }
  std::size_t pos = 0;
  while ((pos = text.find(from, pos)) != std::string::npos) {
    text.replace(pos, from.size(), to);
    pos += to.size();
  }
  return text;
}

// Writes the corpus and its compilation database, and returns the source root.
std::string write_corpus(const std::string &root) {
  ::mkdir(root.c_str(), 0755);
  write_file(root + "/shared.hpp",
             "#pragma once\n"
             "namespace shared {\n"
             "inline int value() { return 7; }\n"
             "struct Carrier { int held; int get() const { return held; } };\n"
             "} // namespace shared\n");
  std::string commands = "[\n";
  for (std::size_t unit = 0; unit < kTranslationUnits; ++unit) {
    const std::string name = "unit_" + std::to_string(unit) + ".cpp";
    write_file(root + "/" + name,
               "#include \"shared.hpp\"\n"
               "int repeated_declaration();\n"
               "int local_" +
                   std::to_string(unit) +
                   "() { return shared::value(); }\n"
                   "int carried_" +
                   std::to_string(unit) +
                   "(shared::Carrier c) { return c.get(); }\n"
                   "namespace shared { int fan_" +
                   std::to_string(unit) + "() { return value(); } }\n");
    if (unit != 0) {
      commands += ",\n";
    }
    commands += "  {\"directory\": \"" + root + "\", \"file\": \"" + root +
                "/" + name + "\", \"command\": \"clang++ -std=c++23 -I" + root +
                " -c " + root + "/" + name + " -o " + root + "/" + name +
                ".o\"}";
  }
  commands += "\n]\n";
  write_file(root + "/compile_commands.json", commands);
  return root;
}

int run_cidx(const std::vector<std::string> &argv, const std::string &cache,
             cidx::Logger &log) {
  cli::ParsedArgs parsed = cli::parse_args(argv);
  REQUIRE(!parsed.help_text);
  std::ostringstream out;
  std::ostringstream err;
  cli::Context ctx;
  ctx.cache_dir = cache;
  ctx.index_path = cache + "/index.db";
  ctx.logger = &log;
  ctx.out = &out;
  ctx.err = &err;
  const int code = cli::run_command(parsed, ctx);
  if (code != 0) {
    MESSAGE("cidx failed: " << err.str());
  }
  return code;
}

std::string dump(SqliteDb &db, const char *name, const char *sql, int columns) {
  std::string out = std::string("== ") + name + " ==\n";
  SqliteStmt statement = db.prepare(sql);
  while (statement.step()) {
    for (int column = 0; column < columns; ++column) {
      if (column != 0) {
        out += '\t';
      }
      out += statement.col_text(column);
    }
    out += '\n';
  }
  return out;
}

// The semantic core of the Layer-0 projection: surrogate keys resolved to
// stable semantic keys, every row set ordered, so the result is independent of
// insertion order and of where the corpus lives.
std::string project(const std::string &index_path, const std::string &root) {
  Storage store(index_path);
  SqliteDb &db = store.raw_db();
  std::string out;
  out += dump(db, "symbol",
              "SELECT s.usr, s.spelling, COALESCE(s.qual_name,''), "
              "COALESCE(sk.name, CAST(s.kind AS TEXT)), "
              "COALESCE(ff.name,''), COALESCE(s.line,''), "
              "COALESCE(s.col,''), s.is_definition, s.resolved, "
              "COALESCE(s.linkage,''), COALESCE(s.parent_usr,'') "
              "FROM symbol s LEFT JOIN symbol_kind sk ON sk.id = s.kind "
              "LEFT JOIN file ff ON ff.id = s.file_id "
              "ORDER BY s.usr, ff.name, s.line, s.col",
              11);
  out += dump(db, "decl_site",
              "SELECT s.usr, COALESCE(f.name,''), COALESCE(d.line,''), "
              "COALESCE(d.col,''), d.is_definition "
              "FROM decl_site d JOIN symbol s ON s.id = d.symbol_id "
              "LEFT JOIN file f ON f.id = d.file_id "
              "ORDER BY s.usr, f.name, d.line, d.col",
              5);
  out += dump(db, "edge",
              "SELECT ss.usr, ds.usr, COALESCE(ek.name, CAST(e.kind AS TEXT)), "
              "e.count FROM edge e JOIN symbol ss ON ss.id = e.src_id "
              "JOIN symbol ds ON ds.id = e.dst_id "
              "LEFT JOIN edge_kind ek ON ek.id = e.kind "
              "ORDER BY ss.usr, ds.usr, e.kind",
              4);
  out += dump(db, "edge_site",
              "SELECT ss.usr, ds.usr, COALESCE(f.name,''), "
              "COALESCE(es.line,''), COALESCE(es.col,'') "
              "FROM edge_site_read es JOIN edge e ON e.id = es.edge_id "
              "JOIN symbol ss ON ss.id = e.src_id "
              "JOIN symbol ds ON ds.id = e.dst_id "
              "LEFT JOIN file f ON f.id = es.file_id "
              "ORDER BY ss.usr, ds.usr, f.name, es.line, es.col",
              5);
  out += dump(db, "definition",
              "SELECT s.usr, COALESCE(f.name,''), d.line, d.col "
              "FROM definition d JOIN symbol s ON s.id = d.symbol_id "
              "LEFT JOIN file f ON f.id = d.file_id "
              "ORDER BY s.usr, f.name, d.line, d.col",
              4);
  return replace_all(out, root, "{ROOT}");
}

// Forces worker completion into the exact reverse of the dispatch order: rank
// r does not report completion until every higher rank already has. Every rank
// is in flight at once (the plan allocates one worker per translation unit), so
// this cannot deadlock -- and the owned-header claim gate, which runs DURING
// extraction in ascending rank order, has already been satisfied by the time
// any rank reaches the barrier.
class ReverseCompletionBarrier {
public:
  explicit ReverseCompletionBarrier(std::size_t ranks)
      : next_(ranks == 0 ? 0 : ranks - 1) {}

  void operator()(std::size_t rank) {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait(lock, [this, rank] { return next_ == rank; });
    order_.push_back(rank);
    if (next_ > 0) {
      --next_;
    }
    ready_.notify_all();
  }

  [[nodiscard]] auto order() -> std::vector<std::size_t> {
    const std::scoped_lock lock(mutex_);
    return order_;
  }

private:
  std::mutex mutex_;
  std::condition_variable ready_;
  // The only rank allowed to report completion, walked down from the last to
  // the first.
  std::size_t next_;
  std::vector<std::size_t> order_;
};

} // namespace

TEST_SUITE("clang") {

  TEST_CASE("parallel publication writes the serial database under reversed "
            "completion") {
    const std::string root = make_temp_dir();
    write_corpus(root + "/src");
    cidx::Logger log;

    const std::string serial_cache = root + "/serial";
    ::mkdir(serial_cache.c_str(), 0755);
    REQUIRE(run_cidx({"import", "--db", root + "/src/compile_commands.json",
                      "--name", "parallel-db"},
                     serial_cache, log) == 0);
    REQUIRE(run_cidx({"index", "--jobs", "1"}, serial_cache, log) == 0);

    const std::string parallel_cache = root + "/parallel";
    ::mkdir(parallel_cache.c_str(), 0755);
    REQUIRE(run_cidx({"import", "--db", root + "/src/compile_commands.json",
                      "--name", "parallel-db"},
                     parallel_cache, log) == 0);

    const std::string parallel_index = parallel_cache + "/index.db";
    std::vector<std::size_t> observed;
    std::size_t published = 0;
    {
      Storage db(parallel_index);
      std::vector<cidx::index::ParallelIndexTarget> targets;
      for (const auto &[file, path] : db.list_files()) {
        if (path.ends_with(".cpp")) {
          targets.push_back({.file = file, .path = path});
        }
      }
      REQUIRE(targets.size() == kTranslationUnits);

      ReverseCompletionBarrier barrier(kTranslationUnits);
      const cidx::index::ParallelIndexReport report =
          cidx::index::run_parallel_index(
              db, parallel_index, targets, true, false,
              {.jobs = static_cast<int>(kTranslationUnits)},
              [] { return false; },
              [&observed,
               &published](const cidx::index::ParallelIndexTarget & /*target*/,
                           const cidx::ast::IndexOneOutcome &outcome, bool ok) {
                observed.push_back(observed.size());
                published += ok ? 1 : 0;
                INFO("outcome error: " << outcome.error);
                CHECK(ok);
                return true;
              },
              std::ref(barrier));
      CHECK(report.plan.workers == kTranslationUnits);
      CHECK(report.published == kTranslationUnits);
      CHECK(report.failed == 0);
      // The barrier is only meaningful if it actually inverted the order.
      const std::vector<std::size_t> completion = barrier.order();
      REQUIRE(completion.size() == kTranslationUnits);
      for (std::size_t index = 0; index < kTranslationUnits; ++index) {
        CHECK(completion[index] == kTranslationUnits - 1 - index);
      }
    }
    CHECK(published == kTranslationUnits);

    CHECK(project(serial_cache + "/index.db", root) ==
          project(parallel_index, root));
  }

  TEST_CASE("delete/re-emit and repeated declarations stay deterministic under "
            "parallel publication") {
    // The fixtures the story names that publication ORDER could disturb: a
    // symbol deleted from one unit and re-emitted, the same symbol declared by
    // every unit (shared::value), and a declaration repeated within a unit.
    // Both arms run the identical edit sequence; the databases must agree.
    const auto index_sequence = [](const std::string &root,
                                   const std::string &jobs) {
      const std::string source = root + "/src";
      write_corpus(source);
      const std::string cache = root + "/cache";
      ::mkdir(cache.c_str(), 0755);
      cidx::Logger log;
      REQUIRE(run_cidx({"import", "--db", source + "/compile_commands.json",
                        "--name", "parallel-db"},
                       cache, log) == 0);
      REQUIRE(run_cidx({"index", "--jobs", jobs}, cache, log) == 0);
      // Delete a definition from one unit and re-emit a different one in its
      // place, and make every other unit stale too, so the whole set is
      // republished together and the run plans more than one worker. (The
      // grammar parser has no `rebuild` subcommand -- that spelling belongs to
      // the typed adapter -- so staleness is produced by editing.)
      for (std::size_t unit = 0; unit < kTranslationUnits; ++unit) {
        const std::string index = std::to_string(unit);
        std::string body = "#include \"shared.hpp\"\n"
                           "int repeated_declaration();\n"
                           "int touched_" +
                           index + "() { return " + index + "; }\n";
        body += unit == 1
                    ? "int reemitted_1() { return shared::value(); }\n"
                    : "int local_" + index +
                          "() { return shared::value(); }\n"
                          "int carried_" +
                          index + "(shared::Carrier c) { return c.get(); }\n";
        body += "namespace shared { int fan_" + index +
                "() { return value(); } }\n";
        write_file(source + "/unit_" + index + ".cpp", body);
      }
      REQUIRE(run_cidx({"index", "--jobs", jobs}, cache, log) == 0);
      return project(cache + "/index.db", root);
    };
    const std::string serial_root = make_temp_dir();
    const std::string parallel_root = make_temp_dir();
    const std::string serial = index_sequence(serial_root, "1");
    const std::string parallel =
        index_sequence(parallel_root, std::to_string(kTranslationUnits));
    CHECK(serial == parallel);
    // The deletion must actually have taken effect, or the comparison above is
    // comparing two unchanged databases.
    CHECK(serial.find("local_1") == std::string::npos);
    CHECK(serial.find("reemitted_1") != std::string::npos);
  }

  TEST_CASE("cross-translation-unit references survive parallel publication") {
    // The regression this guards: resolving cross-TU symbol identity from a
    // pre-run snapshot silently loses every reference into a header another
    // translation unit owns. Those references are `uses` edges into the
    // header's namespace, so their absence is invisible in counts of symbols.
    const std::string root = make_temp_dir();
    write_corpus(root + "/src");
    cidx::Logger log;
    const std::string cache = root + "/cache";
    ::mkdir(cache.c_str(), 0755);
    REQUIRE(run_cidx({"import", "--db", root + "/src/compile_commands.json",
                      "--name", "parallel-db"},
                     cache, log) == 0);
    REQUIRE(run_cidx({"index", "--jobs", std::to_string(kTranslationUnits)},
                     cache, log) == 0);

    Storage store(cache + "/index.db");
    SqliteDb &db = store.raw_db();
    auto edges = db.prepare(
        "SELECT COUNT(*) FROM edge e JOIN symbol src ON src.id = e.src_id "
        "JOIN symbol dst ON dst.id = e.dst_id "
        "WHERE dst.usr LIKE '%@N@shared%'");
    REQUIRE(edges.step());
    CHECK(edges.col_int64(0) > 0);
    // Every translation unit calls shared::value(); each call is an edge whose
    // destination is the header-owned function.
    auto calls = db.prepare("SELECT COUNT(DISTINCT e.src_id) FROM edge e "
                            "JOIN symbol dst ON dst.id = e.dst_id "
                            "WHERE dst.spelling = 'value'");
    REQUIRE(calls.step());
    CHECK(calls.col_int64(0) >= static_cast<std::int64_t>(kTranslationUnits));
  }
}

int main(int argc, char **argv) {
  doctest::Context context;
  context.applyCommandLine(argc, argv);
  return context.run();
}
