// cidx-astgraph entry point — dump one TU's Clang AST into <TU name>.db
// for Soufflé/Datalog reasoning (see astgraph.hpp for the schema contract).
//
// Configuration is SHARED with cidx: the source file's compile args + driver
// are read from the cidx index.db `file` row (same sanitize/resolve pipeline
// as `cidx index`), and the parse goes through the same Toolchain/Parser.
// Exit codes mirror cidx main.cpp: usage=2, any other error=1.
#include <sys/stat.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "astgraph/astgraph.hpp"
#include "astgraph/souffle_runner.hpp"
#include "cli/args.hpp"     // kVersion
#include "cli/commands.hpp" // resolve_cache_dir()
#include "cli/json_out.hpp"
#include "compiledb/compiledb.hpp"
#include "storage/artifacts.hpp"
#include "storage/storage.hpp"
#include "util/errors.hpp"
#include "util/hashing.hpp"
#include "util/logger.hpp"
#include "util/pathutil.hpp"

namespace {

constexpr const char *kUsage =
    "usage: cidx-astgraph [-h] [--version] [--db PATH] [--out DIR] "
    "[--output PATH] [--main-only] SOURCE\n"
    "       cidx-astgraph analyze --rule callgraph [--jobs N] [--db PATH] "
    "[--out DIR] [--output PATH] [--main-only] SOURCE\n";

constexpr const char *kHelp =
    "Dump one translation unit's Clang AST into a per-TU SQLite graph DB\n"
    "(<basename(SOURCE)>.<identity>.db) for Datalog/Souffle reasoning.\n"
    "\n"
    "positional arguments:\n"
    "  SOURCE       source file of the TU; must be registered in the cidx\n"
    "               index (cidx import) so its compile args can be shared\n"
    "\n"
    "options:\n"
    "  -h, --help   show this help message and exit\n"
    "  --version    show the tool version and exit\n"
    "  --db PATH    cidx index.db to read compile args from\n"
    "               (default: <cache dir>/index.db)\n"
    "  --out DIR    directory for the output DB (default: current dir)\n"
    "  --output PATH\n"
    "               exact output DB path (atomically replaced on success)\n"
    "  --main-only  restrict the structural walk to main-file cursors;\n"
    "               header entities still appear when referenced\n"
    "\n"
    "analysis options:\n"
    "  analyze      run an embedded native Souffle rule after dumping\n"
    "  --rule NAME  native rule to run (currently: callgraph)\n"
    "  --jobs N     Souffle worker count (default: 1)\n";

bool file_exists(const std::string &path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0;
}

struct CliArgs {
  std::optional<std::string> index_db;
  std::optional<std::string> out_dir;
  std::optional<std::string> output_path;
  std::optional<std::string> rule;
  bool analyze = false;
  bool main_only = false;
  int jobs = 1;
  std::string source;
};

CliArgs parse_cli(const std::vector<std::string> &argv) {
  CliArgs out;
  for (std::size_t i = 0; i < argv.size(); ++i) {
    const std::string &a = argv[i];
    auto value = [&](const char *flag) -> std::string {
      if (i + 1 >= argv.size()) {
        throw cidx::UsageError(std::string(kUsage) + "cidx-astgraph: error: " +
                               flag + " expects a value\n");
      }
      return argv[++i];
    };
    if (a == "--db") {
      out.index_db = value("--db");
    } else if (a == "--out") {
      out.out_dir = value("--out");
    } else if (a == "--output") {
      out.output_path = value("--output");
    } else if (a == "--rule") {
      if (out.rule) {
        throw cidx::UsageError(
            std::string(kUsage) +
            "cidx-astgraph: error: --rule specified more than once\n");
      }
      out.rule = value("--rule");
    } else if (a == "--jobs") {
      const std::string n = value("--jobs");
      try {
        out.jobs = std::stoi(n);
      } catch (...) {
        throw cidx::UsageError(
            std::string(kUsage) +
            "cidx-astgraph: error: --jobs expects an integer\n");
      }
      if (out.jobs < 1) {
        throw cidx::UsageError(
            std::string(kUsage) +
            "cidx-astgraph: error: --jobs must be at least 1\n");
      }
    } else if (a == "--main-only") {
      out.main_only = true;
    } else if (a == "analyze") {
      if (out.analyze) {
        throw cidx::UsageError(
            std::string(kUsage) +
            "cidx-astgraph: error: analyze specified more than once\n");
      }
      out.analyze = true;
    } else if (!a.empty() && a[0] == '-') {
      throw cidx::UsageError(std::string(kUsage) +
                             "cidx-astgraph: error: unknown option " + a +
                             "\n");
    } else if (out.source.empty()) {
      out.source = a;
    } else {
      throw cidx::UsageError(std::string(kUsage) +
                             "cidx-astgraph: error: exactly one SOURCE\n");
    }
  }
  if (out.source.empty()) {
    throw cidx::UsageError(std::string(kUsage) +
                           "cidx-astgraph: error: SOURCE is required\n");
  }
  if (out.analyze && !out.rule) {
    throw cidx::UsageError(
        std::string(kUsage) +
        "cidx-astgraph: error: analyze requires --rule NAME\n");
  }
  if (!out.analyze && out.rule) {
    throw cidx::UsageError(std::string(kUsage) +
                           "cidx-astgraph: error: --rule requires analyze\n");
  }
  return out;
}

cidx::json_out::Value
callgraph_json(const std::string &source, const std::string &out_path,
               const std::vector<cidx::astgraph::CallFact> &calls) {
  using cidx::json_out::Array;
  using cidx::json_out::Object;
  using cidx::json_out::Value;
  Array rows;
  rows.reserve(calls.size());
  for (const auto &call : calls) {
    rows.push_back(Value::obj(Object{
        {"caller_node", Value::of(call.caller_node)},
        {"caller_usr", Value::of(call.caller_usr)},
        {"caller_name", Value::of(call.caller_name)},
        {"callee_node", Value::of(call.callee_node)},
        {"callee_usr", Value::of(call.callee_usr)},
        {"callee_name", Value::of(call.callee_name)},
        {"line", Value::of(call.line)},
    }));
  }
  return Value::obj(Object{
      {"rule", Value::of(true)},
      {"source", Value::of(source)},
      {"ast_db", Value::of(out_path)},
      {"calls", Value::arr(std::move(rows))},
  });
}

} // namespace

int main(int argc, char **argv) {
  const std::vector<std::string> raw(argv + 1, argv + argc);
  for (const std::string &a : raw) {
    if (a == "-h" || a == "--help") {
      std::cout << kUsage << "\n" << kHelp;
      return 0;
    }
    if (a == "--version") {
      std::cout << "cidx-astgraph " << cidx::cli::kVersion << "\n";
      return 0;
    }
  }
  try {
    const CliArgs cli = parse_cli(raw);

    const std::string cache_dir = cidx::cli::resolve_cache_dir();
    const std::string index_path =
        cli.index_db ? *cli.index_db
                     : cidx::pathutil::join(cache_dir, "index.db");
    if (!file_exists(index_path)) {
      throw cidx::CidxError("cidx index not found at " + index_path +
                            " (run `cidx import` first, or pass --db)");
    }
    // Same log sink as cidx: parse-failure flag dumps and toolchain probes
    // go to cidx.log, never the terminal.
    if (file_exists(cache_dir)) {
      cidx::Logger::root().set_file(
          cidx::pathutil::join(cache_dir, "cidx.log"));
    }

    const std::string source = cidx::pathutil::abspath(cli.source);
    if (!file_exists(source)) {
      throw cidx::CidxError("source file not found: " + source);
    }

    cidx::Storage db(index_path);
    const std::optional<cidx::File> rec = db.get_file(source);
    if (!rec) {
      throw cidx::CidxError(
          source + " is not registered in the cidx index (" + index_path +
          "); run `cidx import <compile_commands.json>` for its project");
    }

    const std::vector<cidx::TranslationUnitConfig> descriptors =
        db.translation_unit_configs_for_file(rec->id);
    if (descriptors.size() != 1U) {
      throw cidx::CidxError(
          source + " has " + std::to_string(descriptors.size()) +
          " registered translation-unit descriptors; astgraph requires one");
    }
    const cidx::TranslationUnitConfig &descriptor = descriptors.front();
    if (descriptor.state != cidx::TranslationUnitConfigState::registered ||
        descriptor.association_state !=
            cidx::TranslationUnitConfigState::registered) {
      throw cidx::CidxError(
          source + " has a non-registered translation-unit descriptor");
    }
    const std::vector<std::string> &opts = descriptor.arguments;

    cidx::astgraph::Options dump_opts;
    dump_opts.main_only = cli.main_only;
    dump_opts.descriptor_hash = descriptor.descriptor_hash;
    dump_opts.resource_dir = descriptor.resource_dir;
    const std::string default_name =
        cidx::pathutil::basename(source) + "." +
        cidx::astgraph::artifact_key(source, opts, descriptor.driver, dump_opts)
            .substr(0, 12) +
        ".db";
    const std::string out_path =
        cli.output_path ? cidx::pathutil::abspath(*cli.output_path)
                        : cidx::pathutil::join(cli.out_dir ? *cli.out_dir : ".",
                                               default_name);
    const cidx::astgraph::DumpStats stats = cidx::astgraph::dump_tu(
        source, opts, descriptor.driver, out_path, dump_opts);

    // Per-TU AST databases are optional sidecars.  Publish them through the
    // core manifest so readers can validate provenance and attach read-only;
    // the authoritative index remains untouched by the AST node/edge dump.
    cidx::ArtifactStore artifacts(
        db, std::filesystem::path(index_path).parent_path());
    const auto component = db.component_for_path(source);
    const std::string workspace_identity = [&]() {
      if (!component) {
        return std::string("workspace:unregistered");
      }
      if (!component->repository_id) {
        return "component:" + component->name;
      }
      const auto repository =
          db.get_repository_by_id(*component->repository_id);
      if (!repository) {
        return "component:" + component->name;
      }
      return repository->remote_url
                 ? "repository:url:" + *repository->remote_url
                 : "repository:name:" + repository->name;
    }();
    const std::string component_root =
        component ? db.component_abs_base(*component)
                  : std::filesystem::path(source).parent_path().string();
    const std::string relative_tu =
        std::filesystem::relative(source, component_root).generic_string();
    std::string configuration_material;
    for (const auto &option : opts) {
      configuration_material += std::to_string(option.size()) + ":" + option;
    }
    configuration_material += "driver:" + rec->driver.value_or("") +
                              ":main-only:" + (dump_opts.main_only ? "1" : "0");
    const std::string separator(1, '\0');
    const std::string configuration_identity =
        cidx::sha1_hex(workspace_identity + separator + configuration_material);
    const std::string tu_identity =
        cidx::sha1_hex(workspace_identity + separator + relative_tu +
                       separator + configuration_identity);
    cidx::ArtifactSpec artifact;
    artifact.logical_id = "astgraph:" + workspace_identity + ":" + relative_tu;
    artifact.kind = "astgraph";
    artifact.artifact_schema =
        "cidx-astgraph/v" + std::to_string(cidx::astgraph::kSchemaVersion);
    artifact.catalog_version = cidx::catalog::kCatalogVersion;
    artifact.catalog_hash = std::string(cidx::catalog::kCatalogHash);
    artifact.producer_version =
        std::string("cidx-astgraph ") + cidx::cli::kVersion;
    artifact.engine_version = std::string("cidx ") + cidx::cli::kVersion;
    artifact.workspace_identity = workspace_identity;
    artifact.tu_identity = tu_identity;
    artifact.configuration_identity = configuration_identity;
    artifact.input_fact_set_identity =
        cidx::sha1_hex("facts:" + workspace_identity + ":" + tu_identity + ":" +
                       std::string(cidx::catalog::kCatalogHash));
    artifact.completeness = cidx::ArtifactCompleteness::complete;
    artifact.truncation = cidx::ArtifactTruncation::none;
    artifact.trust = cidx::ArtifactTrust::producer_verified;
    artifact.evidence = "source";
    artifact.attachment_name = "astgraph_" + tu_identity.substr(0, 16);
    artifact.exposed_relations = {"node", "edge", "symbol", "meta"};
    const cidx::ArtifactRecord published =
        artifacts.publish_existing(artifact, out_path);
    out_path = (std::filesystem::path(index_path).parent_path() /
                published.relative_path)
                   .string();

    // USRs are the stable cross-database identity. Preserve both successful
    // and unresolved joins so readers can distinguish absence from a mapping
    // that was never attempted.
    cidx::SqliteDb sidecar(out_path, true);
    auto sidecar_symbols =
        sidecar.prepare("SELECT usr FROM symbol WHERE usr <> '' ORDER BY usr");
    while (sidecar_symbols.step()) {
      const std::string usr = sidecar_symbols.col_text(0);
      const auto core_symbol = db.lookup_symbol(usr);
      artifacts.record_identity_mapping(
          published.spec.logical_id, usr, "usr", usr,
          core_symbol ? "resolved" : "unresolved",
          core_symbol ? std::optional<std::int64_t>(core_symbol->id)
                      : std::nullopt,
          core_symbol ? "" : "core symbol is not present in index");
    }

    if (cli.analyze) {
      const std::string rule = cli.rule.value_or("");
      if (rule != "callgraph") {
        throw cidx::CidxError("unsupported native rule: " + rule);
      }
      const auto calls = cidx::astgraph::run_callgraph(out_path, cli.jobs);
      std::cout << cidx::json_out::dumps_indent2(
                       callgraph_json(source, out_path, calls))
                << "\n";
      return 0;
    }

    std::cout << out_path << ": " << stats.cursor_nodes << " cursor nodes, "
              << stats.type_nodes << " type nodes, " << stats.edges
              << " edges, " << stats.symbols << " symbols, " << stats.files
              << " files\n";
    return 0;
  } catch (const cidx::UsageError &e) {
    std::cerr << e.what();
    return e.exit_code();
  } catch (const cidx::CidxError &e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  } catch (...) {
    return 1;
  }
}
