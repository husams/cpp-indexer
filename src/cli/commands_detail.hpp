// Helpers shared by the commands_*.cpp translation units. These were
// file-local to commands.cpp; splitting the command table across translation
// units made them shared. The command entry points themselves are already
// declared in commands.hpp.
#pragma once

#include "cli/commands.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast/clang_version.hpp"
#include "ast/index_engine.hpp"
#include "cli/format.hpp"
#include "cli/json_out.hpp"
#include "cli/kind_names.hpp"
#include "compiledb/compiledb.hpp"
#include "graph/emit.hpp"
#include "graph/query.hpp"
#include "graph/records.hpp"
#include "storage/records.hpp"
#include "storage/storage.hpp"
#include "toolchain/toolchain.hpp"
#include "util/env.hpp"
#include "util/errors.hpp"
#include "util/files.hpp"
#include "util/hashing.hpp"
#include "util/pathutil.hpp"
#include "util/repo.hpp"

namespace cidx::cli {
namespace detail {

namespace fmt = format;

inline bool is_digits(const std::string &s) {
  if (s.empty()) {
    return false;
  }
  for (char c : s) {
    if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
      return false;
    }
  }
  return true;
}

inline bool is_directory(const std::string &path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// clang diagnostic severity labels (cli.py _DIAG_SEVERITY_LABELS).
inline const char *diag_severity_label(int severity) {
  switch (severity) {
  case 2:
    return "warning";
  case 3:
    return "error";
  case 4:
    return "fatal";
  default:
    return nullptr;
  }
}

// cli.py _diag_flag: compact `list files` indicator from {severity: n}: "-"
// when clean, else e.g. "2E"/"3W"/"1E2W" (errors+fatals fold into E).
inline std::string diag_flag(const std::map<int, int64_t> &counts) {
  int64_t errs = 0;
  int64_t warns = 0;
  for (const auto &[sev, n] : counts) {
    if (sev >= 3) {
      errs += n;
    } else if (sev == 2) {
      warns += n;
    }
  }
  if (errs == 0 && warns == 0) {
    return "-";
  }
  std::string out;
  if (errs != 0) {
    out += std::to_string(errs) + "E";
  }
  if (warns != 0) {
    out += std::to_string(warns) + "W";
  }
  return out;
}

// cli.py _diag_summary: "2 error(s), 1 warning(s)" — severity desc, nonzero.
inline std::string diag_summary(const std::map<int, int64_t> &counts) {
  std::string out;
  for (int sev : {4, 3, 2}) {
    const auto it = counts.find(sev);
    if (it == counts.end() || it->second == 0) {
      continue;
    }
    if (!out.empty()) {
      out += ", ";
    }
    out += std::to_string(it->second) + " " + diag_severity_label(sev) + "(s)";
  }
  return out;
}

// os.path.exists parity: any stat success (file or directory).
inline bool path_exists(const std::string &path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0;
}

// os.path.getmtime float parity: sec + nsec * 1e-9.
inline std::optional<double> file_mtime(const std::string &path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) {
    return std::nullopt;
  }
#ifdef __APPLE__
  return static_cast<double>(st.st_mtimespec.tv_sec) +
         (static_cast<double>(st.st_mtimespec.tv_nsec) * 1e-9);
#else
  return static_cast<double>(st.st_mtim.tv_sec) +
         static_cast<double>(st.st_mtim.tv_nsec) * 1e-9;
#endif
}

// compiledb.source_path: _abs(cmd.filename, cmd.directory) — absolute
// filenames returned unchanged (not normalized), relative ones
// normpath(join(...)).
inline std::string source_path(const CompileCommand &cmd) {
  if (pathutil::isabs(cmd.filename)) {
    return cmd.filename;
  }
  return pathutil::normpath(pathutil::join(cmd.directory, cmd.filename));
}

// The default import universe is derived from declared build evidence, not a
// repository display name.  The command set is normalized to source paths
// relative to the workspace root plus driver/parse options, then anchored to
// the repository's declared remote when one is available.  --universe remains
// the explicit override for intentional composition.
inline std::string
build_evidence_universe_key(const std::vector<CompileCommand> &commands,
                            const std::string &root,
                            const std::optional<std::string> &remote_url) {
  std::vector<std::string> records;
  records.reserve(commands.size());
  for (const CompileCommand &cmd : commands) {
    std::string record = pathutil::relpath(source_path(cmd), root);
    record += '\0';
    record += cmd.driver;
    for (const std::string &arg : cmd.args) {
      record += '\0';
      record += arg;
    }
    records.push_back(std::move(record));
  }
  std::ranges::sort(records);
  std::string evidence = remote_url.value_or("workspace");
  for (const std::string &record : records) {
    evidence += '\0';
    evidence += record;
  }
  return "build:" + sha1_hex(evidence);
}

// _lookup_component (cli.py:162-171): nullopt name -> no scoping; unknown
// name -> "error: no component named '<name>'" printed, false returned
// (LookupError -> return 1 in every caller).
inline bool lookup_component(Storage &db,
                             const std::optional<std::string> &name,
                             std::optional<Component> &out, std::ostream &err) {
  out.reset();
  if (!name || name->empty()) {
    return true;
  }
  out = db.get_component_by_name(*name);
  if (!out) {
    err << "error: no component named " << fmt::py_repr(*name) << "\n";
    return false;
  }
  return true;
}

const char kDirNeedsComponent[] =
    "error: --dir requires --component (directory paths are relative to a "
    "component root)";

// -- index helpers (cli.py:180-231) -----------------------------------------

// _index_one (cli.py:180-197): parse + index one pending file (main TU + its
// headers); returns 0/1. Only ClangParseError is tolerated (error printed,
// fail flag, the run continues with the rest); everything else propagates to
// main() (D23). The ParsedTu lives only inside the try block: its destructor
// frees the TU + Index BEFORE mark_file_indexed runs — Python's
// `del tu` in index_source's finally (one-AST peak memory, design §7).
inline int index_one(Storage &db, const File &rec, const std::string &path,
                     bool graph_enabled, Context &ctx) {
  // All indexing runs through the LibTooling engine (parity-proven visitors
  // over the Clang C++ API): same DB effects, counters, and per-file output
  // line as the retired libclang cursor walk.
  {
    ast::IndexOneOutcome out = ast::run_index_one(db, rec, path, graph_enabled);
    if (ctx.index_outcome_sink) {
      ctx.index_outcome_sink(out);
    }
    if (out.parse_failed) {
      if (ctx.logger != nullptr && !out.failed_flags.empty()) {
        std::string flags;
        for (std::size_t i = 0; i < out.failed_flags.size(); ++i) {
          if (i != 0) {
            flags += " ";
          }
          flags += out.failed_flags[i];
        }
        ctx.logger->error(
            "cidx.clang",
            path + ": failed parse flags: " + flags +
                "; clang: " + std::to_string(clang_version_major()));
        // log_diagnostics parity: the ERROR-severity diagnostics as INFO
        // lines (capped at 20; classic logs error_diagnostics()).
        std::size_t shown = 0;
        for (const Diagnostic &d : out.diagnostics) {
          if (d.severity < 3) {
            continue;
          }
          if (shown++ >= 25) {
            break;
          }
          ctx.logger->info("cidx.clang",
                           path + ": diag " + d.file_path.value_or("") + ":" +
                               std::to_string(d.line.value_or(0)) + ": " +
                               d.spelling);
        }
      }
      std::vector<Diagnostic> failed = out.diagnostics;
      if (failed.empty()) {
        Diagnostic d;
        d.severity = 4;
        d.spelling = out.error;
        d.file_path = path;
        failed.push_back(std::move(d));
      }
      db.replace_diagnostics(rec.id, failed);
      *ctx.err << "error: " << out.error << "\n";
      return 1;
    }
    if (out.source_changed) {
      db.replace_diagnostics(rec.id, out.diagnostics);
      db.set_file_indexed(rec.id, false);
      *ctx.err << "error: " << out.error << "\n";
      return 1;
    }
    db.replace_diagnostics(rec.id, out.diagnostics);
    db.mark_file_indexed(rec.id, file_mtime(path), out.source_md5);
    *ctx.out << "  -> " << out.stored
             << " symbols; headers: " << out.headers.indexed << " indexed (+"
             << out.headers.symbols << " symbols), " << out.headers.already
             << " already, " << out.headers.system << " system, "
             << out.headers.unowned << " unowned\n";
    return 0;
  }
}

// _index_files (cli.py:200-215): index FILE...; unknown files set the fail
// flag but the loop continues.
inline int index_files(Storage &db, const std::vector<std::string> &file_args,
                       const std::optional<std::string> &root,
                       bool graph_enabled, Context &ctx) {
  int rc = 0;
  for (const std::string &f : file_args) {
    const std::string path = files::resolve_file_arg(f, root);
    const std::optional<File> rec = db.get_file(path);
    if (!rec) {
      *ctx.err << "error: not in index database: " << path << "\n";
      rc = 1;
      continue;
    }
    *ctx.out << "file: " << path << "\n";
    if (files::index_status(*rec, path) == files::IndexStatus::kOk) {
      *ctx.out << "  already indexed\n";
      continue;
    }
    rc |= index_one(db, *rec, path, graph_enabled, ctx);
  }
  return rc;
}

// _index_pending (cli.py:218-231): index every file still pending. Python
// iterates db.files() — EVERY row (header rows included) with the md5-only
// skip (analysis §4); list_files() with no filters is the same query/order
// (ORDER BY c.path, d.path, f.name), snapshotted before the loop so header
// rows added while indexing are not re-visited this run.
inline int index_pending(Storage &db, bool graph_enabled, Context &ctx) {
  int done = 0;
  int skipped = 0;
  int failed = 0;
  int deferred = 0;
  for (const auto &row : db.list_files()) {
    const File &rec = row.first;
    const std::string &path = row.second;
    if (files::index_status(rec, path) == files::IndexStatus::kOk) {
      ++skipped;
      continue;
    }
    // A header (by extension) is indexed via its including TU's
    // index_headers() pass (full -I/-std context, deduped once per run against
    // the live DB), never parsed standalone. Defer it here. A TU source is
    // indexed even when its compile command sanitizes to no flags (e.g.
    // `cc -c x.c -o x.o` -> []) -- it is still a real TU, so its parse
    // diagnostics land on its row.
    if (files::is_header(path)) {
      ++deferred;
      continue;
    }
    *ctx.out << "indexing " << path << "\n";
    if (index_one(db, rec, path, graph_enabled, ctx) == 0) {
      ++done;
    } else {
      ++failed;
    }
  }
  *ctx.out << "index: " << done << " indexed, " << failed << " failed, "
           << skipped << " already indexed";
  if (deferred > 0) {
    *ctx.out << ", " << deferred << " headers via TUs";
  }
  *ctx.out << "\n";
  return failed != 0 ? 1 : 0;
}

inline bool all_files_current(Storage &db) {
  return std::ranges::all_of(db.list_files(), [](const auto &entry) {
    return files::index_status(entry.first, entry.second) ==
           files::IndexStatus::kOk;
  });
}

// -- delete helpers (cli.py _plural / _selector_str / _under_component /
//    _finish_delete) -----------------------------------------------------

inline const char *plural(std::size_t n, const char *singular,
                          const char *plural) {
  return n == 1 ? singular : plural;
}

// The selector the user passed, for error messages: "--name foo".
inline std::string selector_str(const ParsedArgs &args) {
  if (args.del_id) {
    return "--id " + std::to_string(*args.del_id);
  }
  if (args.name) {
    return "--name " + *args.name;
  }
  if (args.del_path) {
    return "--path " + *args.del_path;
  }
  if (args.usr) {
    return "--usr " + *args.usr;
  }
  return "<no selector>";
}

// True when comp is unset, or abs_path lies within the component root.
inline bool under_component(Storage &db,
                            const std::optional<std::string> &abs_path,
                            const std::optional<Component> &comp) {
  if (!comp) {
    return true;
  }
  if (!abs_path) {
    return false;
  }
  // v24: resolve the (possibly clone-relative) component base.
  std::string root = db.component_abs_base(*comp);
  while (!root.empty() && root.back() == '/') {
    root.pop_back();
  }
  return *abs_path == root || abs_path->starts_with(root + "/");
}

// Shared tail: print matched rows, delete (unless --dry-run), summarize.
inline int finish_delete(const ParsedArgs &args, Context &ctx,
                         const std::vector<int64_t> &ids,
                         const std::vector<std::string> &lines,
                         const std::function<void(int64_t)> &del_fn,
                         const char *singular, const char *plural_word) {
  for (const std::string &line : lines) {
    *ctx.out << line << "\n";
  }
  if (!args.dry_run) {
    for (const int64_t id : ids) {
      del_fn(id);
    }
  }
  *ctx.out << (args.dry_run ? "would delete " : "deleted ") << ids.size() << " "
           << plural(ids.size(), singular, plural_word) << "\n";
  return 0;
}

} // namespace detail

using namespace detail;

// Command entry points that commands.hpp does not declare. Each is defined
// in the commands_*.cpp unit for its tier; run_command is the only caller.
int cmd_graph_definitions(const ParsedArgs &args, Context &ctx);
int cmd_graph_redefined(const ParsedArgs &args, Context &ctx);
int cmd_realias(const ParsedArgs &args, Context &ctx);
int cmd_resolve(const ParsedArgs &args, Context &ctx);
int cmd_set(const ParsedArgs &args, Context &ctx);

} // namespace cidx::cli
