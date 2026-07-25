// `cidx analyze` is an adapter over the shared fact-provider analysis runner.
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "analysis/facts.hpp"
#include "analysis/runner.hpp"
#include "cli/args.hpp"
#include "cli/commands.hpp"
#include "cli/json_out.hpp"
#include "cli/souffle_rules.hpp"
#include "util/errors.hpp"
#include "util/pathutil.hpp"

namespace cidx::cli {

namespace {

struct BuiltinRule {
  const char *name;
  const char *description;
  std::string_view body;
};

const std::array kBuiltinRules = {
    BuiltinRule{.name = "callgraph",
                .description =
                    "direct and transitive call graph (outputs: call, "
                    "call_transitive)",
                .body = dlrules::k_callgraph},
    BuiltinRule{.name = "cycles",
                .description =
                    "call cycles over calls/dispatch_calls edges (outputs: "
                    "cycle_member, cycle_edge)",
                .body = dlrules::k_cycles},
    BuiltinRule{.name = "unused",
                .description =
                    "defined functions with no incoming call or override edge "
                    "(outputs: unused)",
                .body = dlrules::k_unused}};

bool is_regular_file(const std::string &path) {
  struct stat status{};
  return ::stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode);
}

std::string read_text_file(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw CidxError("cannot read " + path);
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

json_out::Value relation_json(const analysis::FactRelation &relation) {
  json_out::Array rows;
  rows.reserve(relation.rows.size());
  for (const analysis::FactRow &row : relation.rows) {
    json_out::Array cells;
    cells.reserve(row.size());
    for (const analysis::FactValue &cell : row) {
      cells.push_back(json_out::Value::of(analysis::fact_value_text(cell)));
    }
    rows.push_back(json_out::Value::arr(std::move(cells)));
  }
  return json_out::Value::arr(std::move(rows));
}

const BuiltinRule *find_builtin_rule(std::string_view name) {
  const auto *const found =
      std::ranges::find(kBuiltinRules, name, &BuiltinRule::name);
  return found == kBuiltinRules.end() ? nullptr : &*found;
}

} // namespace

int cmd_analyze(const ParsedArgs &args, Context &ctx) {
  const int modes =
      (args.analyze_list ? 1 : 0) + (args.analyze_export ? 1 : 0) +
      (args.analyze_rule ? 1 : 0) + (args.analyze_rules_file ? 1 : 0);
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
      rules.push_back(json_out::Value::obj(
          {{"name", json_out::Value::of(std::string(rule.name))},
           {"description",
            json_out::Value::of(std::string(rule.description))}}));
    }
    *ctx.out << json_out::dumps_indent2(json_out::Value::obj(
                    {{"rules", json_out::Value::arr(std::move(rules))}}))
             << "\n";
    return 0;
  }

  if (!is_regular_file(ctx.index_path)) {
    *ctx.err << "error: index not found at " << ctx.index_path
             << " (run 'cidx import' first, or pass --db)\n";
    return 1;
  }

  try {
    const analysis::SqliteFactProvider provider(ctx.index_path);
    const analysis::FactRequest request;
    const analysis::FactSnapshot snapshot = provider.snapshot(request);
    if (args.analyze_export) {
      const std::string out_dir = pathutil::abspath(*args.analyze_export);
      const analysis::FactExportStats stats =
          analysis::write_fact_files(snapshot, out_dir, dlrules::k_prelude);
      *ctx.out << out_dir << ": " << stats.files << " fact files, "
               << stats.rows << " rows\n";
      return 0;
    }

    std::string label;
    std::string program;
    if (args.analyze_rule) {
      const BuiltinRule *rule = find_builtin_rule(*args.analyze_rule);
      if (rule == nullptr) {
        *ctx.err << "error: unknown rule: " << *args.analyze_rule
                 << " (see cidx analyze --list)\n";
        return 1;
      }
      label = rule->name;
      program = std::string(rule->body);
    } else {
      if (!args.analyze_rules_file) {
        throw CidxError("rules file is required");
      }
      const std::string path = pathutil::abspath(*args.analyze_rules_file);
      if (!is_regular_file(path)) {
        *ctx.err << "error: rules file not found: " << path << "\n";
        return 1;
      }
      label = path;
      program = read_text_file(path);
    }

    const analysis::AnalysisPackage package{.name = label,
                                            .version = "builtin",
                                            .entry_point = label,
                                            .engine = "souffle",
                                            .program = std::move(program),
                                            .content_hash = {},
                                            .required_relations = {}};
    const analysis::AnalysisRun result =
        analysis::AnalysisRunner(
            std::make_unique<analysis::SouffleAnalysisEngine>())
            .run(package, provider, request,
                 analysis::AnalysisOptions{.jobs = args.analyze_jobs});
    if (result.status == analysis::AnalysisStatus::error ||
        result.status == analysis::AnalysisStatus::unknown) {
      if (result.diagnostics.empty()) {
        throw CidxError("analysis did not produce a result");
      }
      throw CidxError(result.diagnostics.front().message);
    }

    json_out::Object relations;
    for (const auto &[name, relation] : result.relations) {
      relations.emplace_back(name, relation_json(relation));
    }
    *ctx.out
        << json_out::dumps_indent2(json_out::Value::obj(
               {{"rule", json_out::Value::of(label)},
                {"db", json_out::Value::of(pathutil::abspath(ctx.index_path))},
                {"relations", json_out::Value::obj(std::move(relations))}}))
        << "\n";
    return 0;
  } catch (const CidxError &error) {
    *ctx.err << "error: " << error.what() << "\n";
    return 1;
  }
}

} // namespace cidx::cli
