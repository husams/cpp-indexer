// Configuration resolution + option classification (docs/diff.md). The index
// is opened read-only and only ever read; the options pipeline is exactly the
// `cidx index` / `cidx-astgraph` one (sanitize + alias decode).
#include "diff/target.hpp"

#include <sys/stat.h>

#include <algorithm>

#include "storage/storage.hpp"
#include "util/errors.hpp"
#include "util/files.hpp"
#include "util/pathutil.hpp"
#include "workspace/context.hpp"
#include "toolchain/toolchain.hpp"

namespace cidx::diff {

namespace {

bool file_exists(const std::string &path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0;
}

// Multiset difference a - b of two option lists, sorted.
std::vector<std::string> only_in(std::vector<std::string> a,
                                 std::vector<std::string> b) {
  std::ranges::sort(a);
  std::ranges::sort(b);
  std::vector<std::string> out;
  std::ranges::set_difference(a, b, std::back_inserter(out));
  return out;
}

} // namespace

OptionClasses classify_options(const std::vector<std::string> &options) {
  OptionClasses out;
  for (std::size_t i = 0; i < options.size(); ++i) {
    const std::string &a = options[i];
    const auto spaced_value = [&]() -> std::optional<std::string> {
      if (i + 1 < options.size()) {
        return options[++i];
      }
      return std::nullopt; // dangling flag: classified as other
    };
    if (a.starts_with("-std=")) {
      out.standard = a.substr(5);
    } else if (a.starts_with("--std=")) {
      out.standard = a.substr(6);
    } else if (a.starts_with("--target=")) {
      out.target = a.substr(9);
    } else if (a == "-target" || a == "--target") {
      if (const auto v = spaced_value()) {
        out.target = *v;
      } else {
        out.other.push_back(a);
      }
    } else if (a == "-D" || a == "-U") {
      if (const auto v = spaced_value()) {
        out.definitions.push_back(a + *v);
      } else {
        out.other.push_back(a);
      }
    } else if (a.size() > 2 && (a.starts_with("-D") || a.starts_with("-U"))) {
      out.definitions.push_back(a);
    } else if (a == "-I" || a == "-isystem" || a == "-iquote" || a == "-F") {
      if (const auto v = spaced_value()) {
        out.includes.push_back(a + " " + *v);
      } else {
        out.other.push_back(a);
      }
    } else if (a.size() > 8 && a.starts_with("-isystem")) {
      out.includes.push_back("-isystem " + a.substr(8));
    } else if (a.size() > 7 && a.starts_with("-iquote")) {
      out.includes.push_back("-iquote " + a.substr(7));
    } else if (a.size() > 2 && (a.starts_with("-I") || a.starts_with("-F"))) {
      out.includes.push_back(a.substr(0, 2) + " " + a.substr(2));
    } else {
      out.other.push_back(a);
    }
  }
  return out;
}

ConfigDelta config_delta(const ParseConfig &left, const ParseConfig &right) {
  ConfigDelta d;
  const auto txt = [](const std::optional<std::string> &v) {
    return v.value_or("");
  };
  if (txt(left.classes.standard) != txt(right.classes.standard)) {
    d.standard = {txt(left.classes.standard), txt(right.classes.standard)};
  }
  if (txt(left.classes.target) != txt(right.classes.target)) {
    d.target = {txt(left.classes.target), txt(right.classes.target)};
  }
  if (txt(left.driver) != txt(right.driver)) {
    d.driver = {txt(left.driver), txt(right.driver)};
  }
  d.definitions_added =
      only_in(right.classes.definitions, left.classes.definitions);
  d.definitions_removed =
      only_in(left.classes.definitions, right.classes.definitions);
  // -D/-U are order-sensitive: `-DX=1 -UX` and `-UX -DX=1` share a multiset
  // but leave X defined vs undefined. When the added/removed sets are empty
  // yet the ordered sequences differ, the only change is a reorder -- which
  // still changes the final macro state, so it is not an identical config.
  d.definitions_reordered =
      d.definitions_added.empty() && d.definitions_removed.empty() &&
      left.classes.definitions != right.classes.definitions;
  d.includes_changed = left.classes.includes != right.classes.includes;
  d.options_left_only = only_in(left.classes.other, right.classes.other);
  d.options_right_only = only_in(right.classes.other, left.classes.other);
  // `other` covers ordered, paired flags whose final effect is order-sensitive
  // (e.g. `-include a.h -include b.h` vs the reverse, or `-Xclang` sequences).
  // When the multiset difference is empty yet the ordered sequences differ, the
  // only change is a reorder -- which can still change what the parser sees, so
  // conservatively treat it as a configuration delta, not an identical config.
  d.options_reordered = d.options_left_only.empty() &&
                        d.options_right_only.empty() &&
                        left.classes.other != right.classes.other;
  d.identical = !d.standard && !d.target && !d.driver &&
                d.definitions_added.empty() && d.definitions_removed.empty() &&
                !d.definitions_reordered && !d.includes_changed &&
                d.options_left_only.empty() && d.options_right_only.empty() &&
                !d.options_reordered;
  return d;
}

ParseConfig resolve_parse_config(const SideSpec &spec) {
  ParseConfig cfg;
  cfg.db = pathutil::abspath(spec.db);
  if (!file_exists(cfg.db)) {
    throw CidxError(spec.side + " side: cidx index not found at " + cfg.db +
                    " (run `cidx import` first, or pass --db/--" + spec.side +
                    "-db)");
  }
  Storage db(cfg.db, Storage::OpenMode::read_only);
  cfg.file = pathutil::abspath(spec.file);
  const std::optional<File> rec = db.get_file(cfg.file);
  if (!rec) {
    throw CidxError(spec.side + " file " + cfg.file +
                    " is not registered in the cidx index (" + cfg.db +
                    "); run `cidx import <compile_commands.json>` for its "
                    "project");
  }
  WorkspaceContext context =
      WorkspaceContext::borrow(db, WorkspaceReadWriteMode::read_only);
  Toolchain toolchain;
  TranslationUnitConfigurationService resolver(context, toolchain);
  std::string resolution_path = cfg.file;
  if (spec.tu) {
    cfg.tu = pathutil::abspath(*spec.tu);
    const std::optional<File> turec = db.get_file(*cfg.tu);
    if (!turec) {
      throw CidxError("--" + spec.side + "-tu " + *cfg.tu +
                      " is not registered in the cidx index (" + cfg.db + ")");
    }
    cfg.parse_file = *cfg.tu;
    cfg.restrict_to_file = true;
    resolution_path = *cfg.tu;
  } else {
    cfg.parse_file = cfg.file;
    resolution_path = cfg.file;
    if (files::is_header(cfg.file) &&
        db.translation_unit_configs_for_file(rec->id).empty()) {
      throw CidxError(spec.side + " file " + cfg.file +
                      " has no stored translation-unit configuration; pass --" +
                      spec.side + "-tu");
    }
  }
  TranslationUnitDescriptor descriptor;
  try {
    descriptor = resolver.resolve(resolution_path);
  } catch (const WorkspaceError &error) {
    if (error.code() == WorkspaceErrorCode::unreproducible_configuration) {
      throw CidxError("stale, unavailable, or ambiguous stored configuration "
                      "for " + cfg.file + "; re-index the translation unit");
    }
    throw;
  }
  cfg.driver = descriptor.configuration.driver;
  cfg.config_hash = descriptor.configuration.descriptor_hash;
  const auto resolved_record = db.get_file(resolution_path);
  cfg.args = resolved_record && resolved_record->compile_options
                 ? resolver.normalized_arguments(*resolved_record->compile_options)
                 : descriptor.configuration.arguments;
  cfg.classes = classify_options(cfg.args);
  return cfg;
}

} // namespace cidx::diff
