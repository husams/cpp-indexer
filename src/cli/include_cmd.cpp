// `cidx include` — the include-hygiene command group (planning/
// cidx-include-hygiene). Four verbs with an explicit mutating boundary:
//
//   graph / check / plan   read-only
//   apply                  the ONLY command that edits source
//
// Every text and JSON output here is deterministically ordered: repository-
// relative path, then source offset, then configuration digest.
#include "cli/commands.hpp"

#include "cli/args.hpp"
#include "cli/json_out.hpp"
#include "include_hygiene/analysis.hpp"
#include "include_hygiene/executor.hpp"
#include "include_hygiene/graph.hpp"
#include "include_hygiene/plan.hpp"
#include "storage/storage.hpp"
#include "util/errors.hpp"
#include "util/files.hpp"
#include "util/pathutil.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

namespace cidx::cli {

namespace {

using json_out::Value;

// Read a --files-from list: one path per line, absolute or relative to the
// current directory. Blank lines and #-comments are skipped so a generated
// list can carry provenance. "-" reads stdin, which makes the obvious
// `git diff --name-only | cidx include check --files-from -` work.
bool read_files_from(const std::string &spec, std::vector<std::string> &out,
                     std::ostream &err) {
  std::istream *in = &std::cin;
  std::ifstream file;
  if (spec != "-") {
    file.open(spec);
    if (!file) {
      err << "cannot read --files-from " << spec << "\n";
      return false;
    }
    in = &file;
  }
  std::string line;
  while (std::getline(*in, line)) {
    // Tolerate CRLF: a list file often comes from another tool.
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const std::size_t b = line.find_first_not_of(" \t");
    if (b == std::string::npos || line[b] == '#') {
      continue;
    }
    const std::size_t e = line.find_last_not_of(" \t");
    out.push_back(
        pathutil::abspath(pathutil::expanduser(line.substr(b, e - b + 1))));
  }
  return true;
}

// The union of PATH... and --files-from, deduplicated and sorted.
bool resolve_scope(const ParsedArgs &args, std::vector<std::string> &out,
                   std::ostream &err) {
  out = args.inc_paths;
  if (args.files_from && !read_files_from(*args.files_from, out, err)) {
    return false;
  }
  std::ranges::sort(out);
  out.erase(std::ranges::unique(out).begin(), out.end());
  return true;
}

// An index with no include facts cannot answer any of these questions: it
// predates v31 or has not been reindexed. Reporting "nothing found" would be a
// vacuous truth that reads exactly like a clean bill of health, so refuse.
bool refuse_empty_graph(cidx::Storage &db, std::ostream &err) {
  if (db.include_graph_populated()) {
    return false;
  }
  err << "no include facts in this index: it predates schema v31 or has not "
         "been reindexed since.\nRun `cidx index` to populate the include "
         "graph.\n";
  return true;
}

std::string rel(const std::string &path) { return path; }

// Warn about requested scope paths the include tier never observed: reporting
// zero findings for them would look exactly like a clean bill of health.
void warn_uncovered(const hygiene::AnalysisResult &res, std::ostream &err) {
  for (const std::string &p : res.uncovered_scope) {
    err << "warning: no include-tier coverage for " << p
        << " (not indexed for includes; findings for it are not reported)\n";
  }
}

// The default (whole-repo) command has no scope_paths to check, so a partially
// reindexed tree -- some TUs carry include facts, the rest are stale/pending --
// would silently look clean. Enumerate the translation units the tier never
// covered and report them (bounded), so "no findings" cannot masquerade as
// "fully analyzed". Only for the unscoped case; scoped paths use warn_uncovered.
void warn_unscoped_coverage(cidx::Storage &db, std::ostream &err) {
  // A TU is uncovered if the tier never observed it -- whether it is still
  // PENDING (never indexed) or was indexed before the tier existed. The pending
  // case is the whole point: one processed TU makes the global populated check
  // pass, so every not-yet-indexed TU would otherwise be silently omitted and a
  // partial tree would read as fully analyzed. Pending and indexed-but-uncovered
  // are reported separately so a fresh import is distinguishable from stale data.
  std::vector<std::string> pending;
  std::vector<std::string> stale;
  for (const auto &[row, abs] : db.list_files()) {
    // Headers are covered via their including TU, never analyzed standalone.
    if (files::is_header(abs) || db.include_tier_covers_file(row.id)) {
      continue;
    }
    (row.indexed ? stale : pending).push_back(abs);
  }
  const auto report = [&](std::vector<std::string> &tus, const char *what) {
    if (tus.empty()) {
      return;
    }
    std::ranges::sort(tus);
    err << "warning: " << tus.size() << " " << what
        << " (reindex to analyze them); findings cover only the rest:\n";
    const std::size_t cap = 10;
    for (std::size_t i = 0; i < tus.size() && i < cap; ++i) {
      err << "  " << tus[i] << "\n";
    }
    if (tus.size() > cap) {
      err << "  ... and " << (tus.size() - cap) << " more\n";
    }
  };
  report(pending, "translation unit(s) not yet indexed for includes");
  report(stale, "indexed translation unit(s) with no include-tier coverage");
}

// -- graph -------------------------------------------------------------------

int emit_cycles(const hygiene::IncludeGraph &g, const std::string &format,
                std::ostream &out) {
  const std::vector<std::vector<std::string>> cycles = g.cycles();
  if (format == "json") {
    json_out::Array arr;
    for (const auto &comp : cycles) {
      json_out::Array c;
      for (const std::string &p : comp) {
        c.push_back(Value::of(rel(p)));
      }
      arr.push_back(Value::arr(std::move(c)));
    }
    out << json_out::dumps_indent2(
               Value::obj({{"cycles", Value::arr(std::move(arr))}}))
        << "\n";
    return 0;
  }
  if (format == "dot") {
    out << "digraph include_cycles {\n";
    for (const auto &comp : cycles) {
      for (std::size_t i = 0; i < comp.size(); ++i) {
        out << "  \"" << rel(comp[i]) << "\" -> \""
            << rel(comp[(i + 1) % comp.size()]) << "\";\n";
      }
    }
    out << "}\n";
    return 0;
  }
  if (cycles.empty()) {
    out << "no include cycles\n";
    return 0;
  }
  for (const auto &comp : cycles) {
    out << "cycle (" << comp.size() << " files):\n";
    for (const std::string &p : comp) {
      out << "  " << rel(p) << "\n";
    }
  }
  return 0;
}

int emit_graph_edges(const hygiene::IncludeGraph &g,
                     const std::vector<std::string> &roots, bool reverse,
                     bool transitive, int depth, const std::string &format,
                     std::ostream &out) {
  // (from, to) pairs, deterministically ordered.
  std::set<std::pair<std::string, std::string>> pairs;
  for (const std::string &root : roots) {
    if (transitive) {
      for (const std::string &other :
           reverse ? g.transitive_to(root, depth) : g.transitive_from(root, depth)) {
        pairs.insert(reverse ? std::make_pair(other, root)
                             : std::make_pair(root, other));
      }
      continue;
    }
    for (const hygiene::GraphEdge &e :
         reverse ? g.direct_to(root) : g.direct_from(root)) {
      pairs.insert({e.src_path, e.dst_path});
    }
  }

  if (format == "dot") {
    out << "digraph includes {\n";
    for (const auto &[from, to] : pairs) {
      out << "  \"" << rel(from) << "\" -> \"" << rel(to) << "\";\n";
    }
    out << "}\n";
    return 0;
  }
  if (format == "json") {
    json_out::Array arr;
    for (const auto &[from, to] : pairs) {
      arr.push_back(Value::obj({{"from", Value::of(rel(from))},
                                {"to", Value::of(rel(to))}}));
    }
    out << json_out::dumps_indent2(
               Value::obj({{"edges", Value::arr(std::move(arr))}}))
        << "\n";
    return 0;
  }
  for (const auto &[from, to] : pairs) {
    out << rel(from) << " -> " << rel(to) << "\n";
  }
  if (pairs.empty()) {
    out << "no include edges\n";
  }
  return 0;
}

} // namespace

int cmd_include_graph(const ParsedArgs &args, Context &ctx) {
  cidx::Storage db(args.index_db.value_or(ctx.index_path));
  if (refuse_empty_graph(db, *ctx.err)) {
    return 1;
  }
  std::vector<std::string> scope;
  if (!resolve_scope(args, scope, *ctx.err)) {
    return 2;
  }
  const hygiene::IncludeGraph g =
      hygiene::IncludeGraph::load(db, args.inc_system);

  if (args.inc_cycles) {
    return emit_cycles(g, args.inc_format, *ctx.out);
  }
  // No scope means every node: `cidx include graph` alone is the whole graph.
  std::vector<std::string> roots = scope.empty() ? g.nodes() : scope;
  for (const std::string &r : roots) {
    if (!scope.empty() && !g.has_node(r)) {
      *ctx.err << "not in the include graph: " << r << "\n";
      return 1;
    }
  }
  return emit_graph_edges(g, roots, args.inc_reverse, args.inc_transitive,
                          args.inc_depth, args.inc_format, *ctx.out);
}

int cmd_include_check(const ParsedArgs &args, Context &ctx) {
  cidx::Storage db(args.index_db.value_or(ctx.index_path));
  if (refuse_empty_graph(db, *ctx.err)) {
    return 1;
  }
  hygiene::AnalysisOptions opts;
  if (!resolve_scope(args, opts.scope_paths, *ctx.err)) {
    return 2;
  }
  opts.want_duplicates = args.inc_duplicates;
  opts.want_unused = args.inc_unused;

  const hygiene::AnalysisResult res = hygiene::analyze(db, opts);
  warn_uncovered(res, *ctx.err);
  if (opts.scope_paths.empty()) {
    warn_unscoped_coverage(db, *ctx.err);
  }

  // `used` findings are the analyzer's internal state, not a report: a user
  // asking what is wrong does not want a line per working include.
  std::vector<hygiene::IncludeCandidate> findings;
  for (const hygiene::IncludeCandidate &c : res.candidates) {
    if (c.cls != hygiene::Classification::Used) {
      findings.push_back(c);
    }
  }

  if (args.inc_json) {
    json_out::Array arr;
    for (const hygiene::IncludeCandidate &c : findings) {
      json_out::Array owners;
      json_out::Array syms;
      json_out::Array macros;
      json_out::Array caveats;
      for (const std::string &s : c.owners) {
        owners.push_back(Value::of(s));
      }
      for (const std::string &s : c.header_symbols) {
        syms.push_back(Value::of(s));
      }
      for (const std::string &s : c.macro_uses) {
        macros.push_back(Value::of(s));
      }
      for (const std::string &s : c.caveats) {
        caveats.push_back(Value::of(s));
      }
      arr.push_back(Value::obj({
          {"id", Value::of(c.id)},
          {"file", Value::of(rel(c.src_path))},
          {"line", Value::of(c.line)},
          {"col", Value::of(c.col)},
          {"target", Value::of(rel(c.dst_path))},
          {"spelling", Value::of(c.spelling)},
          {"classification",
           Value::of(std::string(hygiene::classification_name(c.cls)))},
          {"guarded", Value::of(c.guarded)},
          {"proof", Value::obj({
                        {"owners", Value::arr(std::move(owners))},
                        {"header_symbols", Value::arr(std::move(syms))},
                        {"intersection_count", Value::of(c.intersection_count)},
                        {"macro_uses", Value::arr(std::move(macros))},
                    })},
          {"caveats", Value::arr(std::move(caveats))},
      }));
    }
    *ctx.out << json_out::dumps_indent2(
                    Value::obj({{"findings", Value::arr(std::move(arr))}}))
             << "\n";
    return 0;
  }

  if (findings.empty()) {
    *ctx.out << "no duplicate or unused includes found\n";
    return 0;
  }
  for (const hygiene::IncludeCandidate &c : findings) {
    *ctx.out << rel(c.src_path) << ":" << c.line << ":" << c.col << ": "
             << hygiene::classification_name(c.cls) << ": "
             << (c.is_angled ? "<" : "\"") << c.spelling
             << (c.is_angled ? ">" : "\"") << "\n";
    if (c.cls == hygiene::Classification::UnusedByReference) {
      // The proof, stated so a reviewer can check it without rerunning:
      // N owners referenced nothing among M header symbols.
      *ctx.out << "    " << c.owners.size() << " symbol(s) in this file "
               << "reference 0 of " << c.header_symbols.size()
               << " symbol(s) declared in " << rel(c.dst_path) << "\n";
    }
    for (const std::string &w : c.caveats) {
      *ctx.out << "    note: " << w << "\n";
    }
  }
  *ctx.out << "\n" << findings.size() << " finding(s)\n";
  return 0;
}

int cmd_include_plan(const ParsedArgs &args, Context &ctx) {
  cidx::Storage db(args.index_db.value_or(ctx.index_path));
  if (refuse_empty_graph(db, *ctx.err)) {
    return 1;
  }
  hygiene::AnalysisOptions opts;
  if (!resolve_scope(args, opts.scope_paths, *ctx.err)) {
    return 2;
  }
  opts.want_duplicates = args.inc_duplicates;
  opts.want_unused = args.inc_unused;

  const hygiene::AnalysisResult res = hygiene::analyze(db, opts);
  warn_uncovered(res, *ctx.err);
  if (opts.scope_paths.empty()) {
    warn_unscoped_coverage(db, *ctx.err);
  }
  hygiene::CleanupPlan plan =
      hygiene::build_plan(db, res, args.index_db.value_or(ctx.index_path));

  const std::string text = hygiene::serialize(plan);
  std::ofstream out(*args.inc_output, std::ios::binary | std::ios::trunc);
  if (!out) {
    *ctx.err << "cannot write " << *args.inc_output << "\n";
    return 1;
  }
  out << text << "\n";
  if (!out) {
    *ctx.err << "write failed: " << *args.inc_output << "\n";
    return 1;
  }

  int64_t accepted = 0;
  int64_t manual = 0;
  int64_t rejected = 0;
  for (const hygiene::PlanItem &it : plan.items) {
    switch (it.state) {
    case hygiene::PlanState::Accepted: ++accepted; break;
    case hygiene::PlanState::ManualReview: ++manual; break;
    case hygiene::PlanState::Rejected: ++rejected; break;
    }
  }
  *ctx.out << "wrote " << *args.inc_output << "\n"
           << "  " << accepted << " validated for apply\n"
           << "  " << manual << " need manual review\n"
           << "  " << rejected << " rejected\n";
  if (accepted > 0) {
    *ctx.out << "\nReview the plan, then: cidx include apply "
             << *args.inc_output << "\n";
  }
  return 0;
}

int cmd_include_apply(const ParsedArgs &args, Context &ctx) {
  std::ifstream in(args.inc_plan, std::ios::binary);
  if (!in) {
    *ctx.err << "cannot read plan " << args.inc_plan << "\n";
    return 1;
  }
  std::ostringstream ss;
  ss << in.rdbuf();

  hygiene::CleanupPlan plan;
  try {
    plan = hygiene::deserialize(ss.str());
  } catch (const CidxError &e) {
    *ctx.err << "refusing to apply " << args.inc_plan << ": " << e.what()
             << "\n";
    return 1;
  }

  cidx::Storage db(args.index_db.value_or(ctx.index_path));
  hygiene::ExecuteOptions opts;
  opts.dry_run = args.dry_run;
  opts.only = args.inc_only;

  const hygiene::ExecuteResult r = hygiene::execute(db, plan, opts);
  if (!r.ok) {
    *ctx.err << "refusing to apply " << args.inc_plan << ":\n";
    for (const std::string &why : r.refusals) {
      *ctx.err << "  " << why << "\n";
    }
    return 1;
  }

  if (args.dry_run) {
    *ctx.out << "dry run: " << r.removed << " directive(s) would be removed "
             << "from " << r.edited_files.size() << " file(s)\n";
    for (const std::string &f : r.edited_files) {
      *ctx.out << "  " << rel(f) << "\n";
    }
    return 0;
  }

  *ctx.out << "removed " << r.removed << " directive(s) from "
           << r.edited_files.size() << " file(s)";
  if (r.skipped > 0) {
    *ctx.out << "; skipped " << r.skipped
             << " (not validated for apply, or not selected)";
  }
  *ctx.out << "\n";
  for (const std::string &f : r.edited_files) {
    *ctx.out << "  " << rel(f) << "\n";
  }
  if (!r.reindexed.empty()) {
    *ctx.out << "\n" << r.reindexed.size()
             << " file(s) marked for reindex; run `cidx index` to refresh the "
                "index\n";
  }
  return 0;
}

} // namespace cidx::cli
