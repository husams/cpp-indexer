// CLI grammar (docs/diff.md "CLI") + orchestration. Parse failures map to
// UsageError ("<usage>\n<prog>: error: <detail>\n", exit 2) exactly like
// cidx (src/cli/args.cpp); everything else surfaces as CidxError (exit 1).
#include "diff/driver.hpp"

#include <sys/stat.h>

#include <memory>
#include <optional>

#include "CLI11.hpp"
#include "cli/args.hpp"     // kVersion
#include "cli/commands.hpp" // resolve_cache_dir()
#include "diff/analyze.hpp"
#include "diff/compare.hpp"
#include "diff/report.hpp"
#include "diff/target.hpp"
#include "util/errors.hpp"
#include "util/logger.hpp"
#include "util/pathutil.hpp"

namespace cidx {
namespace diff {

namespace {

bool file_exists(const std::string &path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0;
}

struct CliOptions {
  std::string scope; // file|symbol
  std::string mode = "both";
  std::string match = "heuristic";
  std::string left_file;
  std::string right_file;
  std::optional<std::string> db;
  std::optional<std::string> left_db;
  std::optional<std::string> right_db;
  std::optional<std::string> left_tu;
  std::optional<std::string> right_tu;
  std::string left_sel;
  std::string right_sel;
  std::optional<std::string> kind;
  bool json = false;
  int context = 0;
  std::optional<std::string> help_text;
  bool version = false;
};

// Deepest parsed subcommand and its "cidx-diff symbol"-style program path
// (same shape as src/cli/args.cpp).
std::pair<const CLI::App *, std::string> parsed_leaf(const CLI::App &root) {
  const CLI::App *leaf = &root;
  std::string prog = root.get_name();
  while (true) {
    const std::vector<CLI::App *> subs = leaf->get_subcommands();
    if (subs.empty()) {
      break;
    }
    leaf = subs[0];
    prog += " " + leaf->get_name();
  }
  return {leaf, prog};
}

[[noreturn]] void usage_fail(const CLI::App &root, const CLI::ParseError &e) {
  const auto [leaf, prog] = parsed_leaf(root);
  std::string usage;
  if (const auto fmt = std::dynamic_pointer_cast<const CLI::Formatter>(
          leaf->get_formatter())) {
    usage = fmt->make_usage(leaf, prog);
  }
  std::string detail = e.what();
  const std::vector<std::string> extras =
      dynamic_cast<const CLI::ExtrasError *>(&e) != nullptr
          ? std::vector<std::string>{}
          : leaf->remaining(/*recurse=*/false);
  if (!extras.empty()) {
    detail += " (unrecognized: ";
    for (std::size_t i = 0; i < extras.size(); ++i) {
      detail += (i != 0 ? " " : "") + extras[i];
    }
    detail += ")";
  }
  throw UsageError(usage + prog + ": error: " + detail + "\n", 2);
}

CliOptions parse_cli(const std::vector<std::string> &argv) {
  CliOptions o;
  CLI::App app{"Syntax-aware and semantic C/C++ source diff over the cidx "
               "index",
               "cidx-diff"};
  app.require_subcommand(1);
  const std::string version_text = std::string("cidx-diff ") + cli::kVersion;
  app.set_version_flag("--version", version_text,
                       "show program's version number and exit");

  const auto add_common = [&o, &version_text](CLI::App *sub) {
    sub->set_version_flag("--version", version_text,
                          "show program's version number and exit");
    sub->add_option("--mode", o.mode,
                    "report syntax, semantic, or both (default: both)")
        ->check(CLI::IsMember({"both", "semantic", "syntax"}));
    sub->add_option("--db", o.db,
                    "one index for both sides (default: the standard cache "
                    "index)");
    sub->add_option("--left-db", o.left_db,
                    "index for the left side (overrides --db)");
    sub->add_option("--right-db", o.right_db,
                    "index for the right side (overrides --db)");
    sub->add_option("--left-tu", o.left_tu,
                    "registered TU providing compile context for a left-side "
                    "header");
    sub->add_option("--right-tu", o.right_tu,
                    "registered TU providing compile context for a "
                    "right-side header");
    sub->add_option("--match", o.match,
                    "file-scope entity pairing: strict or heuristic "
                    "(default: heuristic)")
        ->check(CLI::IsMember({"heuristic", "strict"}));
    sub->add_flag("--json", o.json,
                  "emit the deterministic machine-readable report");
    sub->add_option("--context", o.context,
                    "source-context lines in text output (default: 0)")
        ->check(CLI::NonNegativeNumber);
  };

  CLI::App *file =
      app.add_subcommand("file", "compare two whole registered files");
  file->add_option("LEFT_FILE", o.left_file, "left source file")->required();
  file->add_option("RIGHT_FILE", o.right_file, "right source file")
      ->required();
  add_common(file);
  file->callback([&o] { o.scope = "file"; });

  CLI::App *symbol = app.add_subcommand(
      "symbol", "compare one class/function/method between two files");
  symbol->add_option("LEFT_FILE", o.left_file, "left source file")->required();
  symbol->add_option("RIGHT_FILE", o.right_file, "right source file")
      ->required();
  symbol
      ->add_option("--left", o.left_sel,
                   "left selector: USR, qualified signature, qualified name, "
                   "or line:N")
      ->required();
  symbol
      ->add_option("--right", o.right_sel,
                   "right selector: USR, qualified signature, qualified "
                   "name, or line:N")
      ->required();
  symbol
      ->add_option("--kind", o.kind,
                   "restrict a name selector to one entity kind")
      ->check(CLI::IsMember({"class", "function", "method", "struct",
                             "union"}));
  add_common(symbol);
  symbol->callback([&o] { o.scope = "symbol"; });

  // CLI11's vector-parse consumes tokens from the back.
  std::vector<std::string> tokens(argv.rbegin(), argv.rend());
  try {
    app.parse(tokens);
  } catch (const CLI::CallForHelp &) {
    o.help_text = app.help(); // delegates to the parsed subcommand
    return o;
  } catch (const CLI::CallForVersion &) {
    if (app.get_help_ptr() != nullptr && app.get_help_ptr()->count() > 0) {
      o.help_text = app.help();
    } else {
      o.version = true;
    }
    return o;
  } catch (const CLI::ParseError &e) {
    usage_fail(app, e);
  }
  return o;
}

} // namespace

int run(const std::vector<std::string> &argv, std::ostream &out,
        std::ostream &err) {
  try {
    const CliOptions o = parse_cli(argv);
    if (o.help_text) {
      out << *o.help_text;
      return 0;
    }
    if (o.version) {
      out << "cidx-diff " << cli::kVersion << "\n";
      return 0;
    }

    const std::string cache_dir = cli::resolve_cache_dir();
    const std::string default_db = pathutil::join(cache_dir, "index.db");
    // Same log sink as cidx: parse-failure dumps and toolchain probes go to
    // cidx.log, never the terminal.
    if (file_exists(cache_dir)) {
      Logger::root().set_file(pathutil::join(cache_dir, "cidx.log"));
    }

    const std::string shared_db = o.db.value_or(default_db);
    const SideSpec left{"left", o.left_file, o.left_db.value_or(shared_db),
                        o.left_tu};
    const SideSpec right{"right", o.right_file, o.right_db.value_or(shared_db),
                         o.right_tu};
    const ParseConfig left_cfg = resolve_parse_config(left);
    const ParseConfig right_cfg = resolve_parse_config(right);

    std::optional<Selector> left_sel;
    std::optional<Selector> right_sel;
    if (o.scope == "symbol") {
      left_sel = Selector{o.left_sel, o.kind};
      right_sel = Selector{o.right_sel, o.kind};
    }
    const SideAnalysis left_side = analyze_side(left_cfg, left_sel);
    const SideAnalysis right_side = analyze_side(right_cfg, right_sel);

    const ConfigDelta delta = config_delta(left_cfg, right_cfg);
    const Comparison cmp =
        compare_sides(left_side, right_side, o.match, delta, o.scope);
    const ReportSpec spec{o.scope, o.mode, o.match, o.context, o.json};
    render_report(spec, left_side, right_side, delta, cmp, out);
    return 0;
  } catch (const UsageError &e) {
    err << e.what();
    return e.exit_code();
  } catch (const CidxError &e) {
    err << "error: " << e.what() << "\n";
    return 1;
  } catch (const std::exception &e) {
    err << "error: " << e.what() << "\n";
    return 1;
  } catch (...) {
    return 1;
  }
}

} // namespace diff
} // namespace cidx
