// `cidx analyze` — Souffle Datalog analyses over the semantic index.
//
// Behavioral mirror of python/indexer/souffle.py + cli.cmd_analyze: exports
// bounded TSV fact files from index.db, prepends the shared prelude
// declarations, runs the standalone `souffle` interpreter, and prints every
// .output relation as JSON with deterministically sorted rows. The rule
// bytes are embedded at build time from python/indexer/rules (see
// cmake/embed_dl.cmake), so both implementations run identical Datalog.
// Keep the fact SQL, output shapes, exit codes, and error text in lockstep
// with the Python module.
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cli/args.hpp"
#include "cli/commands.hpp"
#include "cli/json_out.hpp"
#include "cli/souffle_rules.hpp"
#include "storage/sqlite.hpp"
#include "storage/storage.hpp"
#include "util/errors.hpp"
#include "util/pathutil.hpp"
#include "util/subprocess.hpp"

namespace cidx::cli {

namespace {

namespace fs = std::filesystem;

constexpr double kSouffleTimeout = 600.0;

struct BuiltinRule {
  const char *name;
  const char *description;
  std::string_view body;
};

// Keep names/descriptions in sync with python/indexer/souffle.py
// BUILTIN_RULES; --list output is parity-checked byte for byte.
const BuiltinRule kBuiltinRules[] = {
    {.name = "callgraph",
     .description =
         "direct and transitive call graph (outputs: call, call_transitive)",
     .body = dlrules::k_callgraph},
    {.name = "cycles",
     .description =
         "call cycles over calls/dispatch_calls edges (outputs: cycle_member, "
         "cycle_edge)",
     .body = dlrules::k_cycles},
    {.name = "unused",
     .description =
         "defined functions with no incoming call or override edge (outputs: "
         "unused)",
     .body = dlrules::k_unused},
};

struct FactQuery {
  const char *name;
  int cols;
  const char *sql;
};

// Column order must match rules/prelude.dl; identical SQL lives in
// python/indexer/souffle.py FACT_QUERIES.
const FactQuery kFactQueries[] = {
    {.name = "symbol",
     .cols = 8,
     .sql = "SELECT id, usr, spelling, COALESCE(qual_name, ''), kind, "
            "is_definition,"
            " COALESCE(file_id, 0), COALESCE(line, 0) FROM symbol ORDER BY id"},
    {.name = "symbol_kind",
     .cols = 2,
     .sql = "SELECT id, name FROM symbol_kind ORDER BY id"},
    {.name = "edge",
     .cols = 5,
     .sql = "SELECT id, src_id, dst_id, kind, count FROM edge ORDER BY id"},
    {.name = "edge_kind",
     .cols = 2,
     .sql = "SELECT id, name FROM edge_kind ORDER BY id"},
    {.name = "edge_site",
     .cols = 4,
     .sql = "SELECT edge_id, file_id, COALESCE(line, 0), COALESCE(col, 0)"
            " FROM edge_site ORDER BY edge_id, file_id, line, col"},
    {.name = "entity_node",
     .cols = 2,
     .sql = "SELECT id, kind FROM entity_node ORDER BY id"},
    {.name = "entity_kind",
     .cols = 2,
     .sql = "SELECT id, name FROM entity_kind ORDER BY id"},
    {.name = "entity_edge",
     .cols = 6,
     .sql = "SELECT src_id, dst_id, kind, COALESCE(via_member_id, 0), access,"
            " is_virtual FROM entity_edge"
            " ORDER BY src_id, dst_id, kind, COALESCE(via_member_id, 0)"},
    {.name = "entity_edge_kind",
     .cols = 2,
     .sql = "SELECT id, name FROM entity_edge_kind ORDER BY id"},
    {.name = "file",
     .cols = 2,
     .sql = "SELECT f.id, c.path ||"
            " CASE WHEN d.path = '' THEN '' ELSE '/' || d.path END || '/' || "
            "f.name"
            " FROM file f"
            " JOIN directory d ON f.directory_id = d.id"
            " JOIN component c ON d.component_id = c.id"
            " ORDER BY f.id"},
};

bool is_regular_file(const std::string &path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// TSV fact files cannot carry tabs or newlines; flatten to spaces (mirrors
// souffle.py _clean).
std::string clean(std::string text) {
  for (char &c : text) {
    if (c == '\t' || c == '\n' || c == '\r') {
      c = ' ';
    }
  }
  return text;
}

std::string strip(const std::string &text) {
  const char *ws = " \t\n\r\f\v";
  const std::size_t begin = text.find_first_not_of(ws);
  if (begin == std::string::npos) {
    return "";
  }
  const std::size_t end = text.find_last_not_of(ws);
  return text.substr(begin, end - begin + 1);
}

std::optional<std::string> find_souffle() {
  const char *env = std::getenv("CIDX_SOUFFLE");
  if (env != nullptr && *env != '\0') {
    return std::string(env);
  }
  const char *path = std::getenv("PATH");
  if (path == nullptr) {
    return std::nullopt;
  }
  std::istringstream iss(path);
  std::string dir;
  while (std::getline(iss, dir, ':')) {
    if (dir.empty()) {
      continue;
    }
    std::string cand = pathutil::join(dir, "souffle");
    if (is_regular_file(cand) && ::access(cand.c_str(), X_OK) == 0) {
      return cand;
    }
  }
  return std::nullopt;
}

void require_schema(SqliteDb &db, const std::string &db_path) {
  std::string got = "none";
  try {
    SqliteStmt st =
        db.prepare("SELECT value FROM meta WHERE key = 'schema_version'");
    if (st.step()) {
      got = st.col_text(0);
    }
  } catch (const StorageError &e) {
    throw CidxError("cannot read index at " + db_path + ": " + e.what());
  }
  if (got != std::to_string(kSchemaVersion)) {
    throw CidxError("unsupported index schema version: " + got +
                    " (expected " + std::to_string(kSchemaVersion) +
                    "); run 'cidx db migrate'");
  }
}

struct ExportStats {
  int files = 0;
  int64_t rows = 0;
};

ExportStats export_facts(const std::string &db_path,
                         const std::string &out_dir) {
  SqliteDb db(db_path);
  require_schema(db, db_path);
  std::error_code ec;
  fs::create_directories(out_dir, ec);
  if (ec) {
    throw CidxError("cannot create " + out_dir + ": " + ec.message());
  }
  ExportStats stats;
  for (const FactQuery &fq : kFactQueries) {
    const std::string path = pathutil::join(out_dir, std::string(fq.name) +
                                                         ".facts");
    std::ofstream out(path, std::ios::binary);
    if (!out) {
      throw CidxError("cannot write " + path);
    }
    SqliteStmt st = db.prepare(fq.sql);
    while (st.step()) {
      for (int i = 0; i < fq.cols; ++i) {
        if (i > 0) {
          out << '\t';
        }
        out << clean(st.col_text(i));
      }
      out << '\n';
      ++stats.rows;
    }
    ++stats.files;
  }
  const std::string prelude_path = pathutil::join(out_dir, "cidx_facts.dl");
  std::ofstream out(prelude_path, std::ios::binary);
  if (!out) {
    throw CidxError("cannot write " + prelude_path);
  }
  out << dlrules::k_prelude;
  return stats;
}

// Names of the program's .output relations, sorted and deduplicated
// (mirrors souffle.py output_relations).
std::vector<std::string> output_relations(const std::string &program) {
  std::vector<std::string> names;
  std::istringstream iss(program);
  std::string line;
  while (std::getline(iss, line)) {
    std::size_t pos = line.find_first_not_of(" \t\r\f\v");
    if (pos == std::string::npos) {
      continue;
    }
    const std::string_view kDirective = ".output";
    if (line.compare(pos, kDirective.size(), kDirective) != 0) {
      continue;
    }
    pos += kDirective.size();
    const std::size_t name_begin = line.find_first_not_of(" \t\r\f\v", pos);
    if (name_begin == std::string::npos || name_begin == pos) {
      continue; // require whitespace between .output and the name
    }
    std::size_t name_end = name_begin;
    auto ident = [](char c, bool first) {
      return (std::isalpha(static_cast<unsigned char>(c)) != 0) || c == '_' ||
             (!first && std::isdigit(static_cast<unsigned char>(c)) != 0);
    };
    while (name_end < line.size() &&
           ident(line[name_end], name_end == name_begin)) {
      ++name_end;
    }
    if (name_end > name_begin) {
      names.push_back(line.substr(name_begin, name_end - name_begin));
    }
  }
  std::ranges::sort(names);
  names.erase(std::ranges::unique(names).begin(), names.end());
  return names;
}

using Rows = std::vector<std::vector<std::string>>;

Rows read_relation(const std::string &path) {
  if (!is_regular_file(path)) {
    return {};
  }
  std::ifstream in(path, std::ios::binary);
  std::ostringstream buf;
  buf << in.rdbuf();
  std::string text = buf.str();
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t nl = text.find('\n', start);
    if (nl == std::string::npos) {
      lines.push_back(text.substr(start));
      break;
    }
    lines.push_back(text.substr(start, nl - start));
    start = nl + 1;
  }
  if (!lines.empty() && lines.back().empty()) {
    lines.pop_back();
  }
  Rows rows;
  rows.reserve(lines.size());
  for (const std::string &line : lines) {
    std::vector<std::string> cells;
    std::size_t begin = 0;
    while (true) {
      const std::size_t tab = line.find('\t', begin);
      if (tab == std::string::npos) {
        cells.push_back(line.substr(begin));
        break;
      }
      cells.push_back(line.substr(begin, tab - begin));
      begin = tab + 1;
    }
    rows.push_back(std::move(cells));
  }
  std::ranges::sort(rows);
  return rows;
}

// Scoped temp dir under the system temp root; removed recursively.
struct TempDir {
  std::string path;
  TempDir() {
    std::string tmpl =
        (fs::temp_directory_path() / "cidx-analyze-XXXXXX").string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (::mkdtemp(buf.data()) == nullptr) {
      throw CidxError("cannot create temporary directory");
    }
    path.assign(buf.data());
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  TempDir(const TempDir &) = delete;
  TempDir &operator=(const TempDir &) = delete;
};

std::map<std::string, Rows> run_program(const std::string &db_path,
                                        const std::string &program,
                                        int jobs) {
  const std::optional<std::string> exe = find_souffle();
  if (!exe || !is_regular_file(*exe) || ::access(exe->c_str(), X_OK) != 0) {
    throw CidxError(
        "souffle executable not found; install souffle or set CIDX_SOUFFLE");
  }
  const std::string full = std::string(dlrules::k_prelude) + "\n" + program;

  TempDir tmp;
  const std::string facts_dir = pathutil::join(tmp.path, "facts");
  const std::string out_dir = pathutil::join(tmp.path, "out");
  std::error_code ec;
  fs::create_directories(out_dir, ec);
  if (ec) {
    throw CidxError("cannot create " + out_dir + ": " + ec.message());
  }
  export_facts(db_path, facts_dir);
  const std::string program_path = pathutil::join(tmp.path, "program.dl");
  {
    std::ofstream out(program_path, std::ios::binary);
    if (!out) {
      throw CidxError("cannot write " + program_path);
    }
    out << full;
  }

  const std::vector<std::string> argv = {*exe,    "-F", facts_dir,
                                         "-D",    out_dir,
                                         "-j",    std::to_string(jobs),
                                         program_path};
  const RunResult res = run(argv, kSouffleTimeout);
  if (res.timed_out) {
    throw CidxError("souffle timed out after " +
                    std::to_string(static_cast<int>(kSouffleTimeout)) + "s");
  }
  if (res.exit_code != 0) {
    std::string detail = strip(res.err);
    if (detail.empty()) {
      detail = strip(res.out);
    }
    throw CidxError("souffle failed (exit " + std::to_string(res.exit_code) +
                    "): " + detail);
  }

  std::map<std::string, Rows> relations;
  for (const std::string &name : output_relations(full)) {
    relations[name] = read_relation(pathutil::join(out_dir, name + ".csv"));
  }
  return relations;
}

std::string read_text_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw CidxError("cannot read " + path);
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

} // namespace

int cmd_analyze(const ParsedArgs &args, Context &ctx) {
  const int modes = (args.analyze_list ? 1 : 0) +
                    (args.analyze_export ? 1 : 0) +
                    (args.analyze_rule ? 1 : 0) +
                    (args.analyze_rules_file ? 1 : 0);
  if (modes != 1) {
    *ctx.err << "error: exactly one of --list, --export-facts, --rule, or "
                "--rules-file is required\n";
    return 2;
  }
  if (args.analyze_jobs < 1) {
    *ctx.err << "error: --jobs must be at least 1\n";
    return 2;
  }

  if (args.analyze_list) {
    json_out::Array rules;
    for (const BuiltinRule &rule : kBuiltinRules) {
      json_out::Object obj;
      obj.emplace_back("name", json_out::Value::of(std::string(rule.name)));
      obj.emplace_back("description",
                       json_out::Value::of(std::string(rule.description)));
      rules.push_back(json_out::Value::obj(std::move(obj)));
    }
    json_out::Object top;
    top.emplace_back("rules", json_out::Value::arr(std::move(rules)));
    *ctx.out << json_out::dumps_indent2(json_out::Value::obj(std::move(top)))
             << "\n";
    return 0;
  }

  if (!is_regular_file(ctx.index_path)) {
    *ctx.err << "error: index not found at " << ctx.index_path
             << " (run 'cidx import' first, or pass --db)\n";
    return 1;
  }

  try {
    if (args.analyze_export) {
      const std::string out_dir = pathutil::abspath(*args.analyze_export);
      const ExportStats stats = export_facts(ctx.index_path, out_dir);
      *ctx.out << out_dir << ": " << stats.files << " fact files, "
               << stats.rows << " rows\n";
      return 0;
    }

    std::string label;
    std::string program;
    if (args.analyze_rule) {
      const BuiltinRule *found = nullptr;
      for (const BuiltinRule &rule : kBuiltinRules) {
        if (*args.analyze_rule == rule.name) {
          found = &rule;
          break;
        }
      }
      if (found == nullptr) {
        *ctx.err << "error: unknown rule: " << *args.analyze_rule
                 << " (see cidx analyze --list)\n";
        return 1;
      }
      label = found->name;
      program = std::string(found->body);
    } else {
      const std::string path = pathutil::abspath(*args.analyze_rules_file);
      if (!is_regular_file(path)) {
        *ctx.err << "error: rules file not found: " << path << "\n";
        return 1;
      }
      label = path;
      program = read_text_file(path);
    }

    const std::map<std::string, Rows> relations =
        run_program(ctx.index_path, program, args.analyze_jobs);

    json_out::Object rel_obj;
    for (const auto &[name, rows] : relations) {
      json_out::Array rows_arr;
      rows_arr.reserve(rows.size());
      for (const std::vector<std::string> &row : rows) {
        json_out::Array cells;
        cells.reserve(row.size());
        for (const std::string &cell : row) {
          cells.push_back(json_out::Value::of(cell));
        }
        rows_arr.push_back(json_out::Value::arr(std::move(cells)));
      }
      rel_obj.emplace_back(name, json_out::Value::arr(std::move(rows_arr)));
    }
    json_out::Object top;
    top.emplace_back("rule", json_out::Value::of(label));
    top.emplace_back("db",
                     json_out::Value::of(pathutil::abspath(ctx.index_path)));
    top.emplace_back("relations", json_out::Value::obj(std::move(rel_obj)));
    *ctx.out << json_out::dumps_indent2(json_out::Value::obj(std::move(top)))
             << "\n";
    return 0;
  } catch (const CidxError &e) {
    *ctx.err << "error: " << e.what() << "\n";
    return 1;
  }
}

} // namespace cidx::cli
