// set, file and dump-compile-commands, with their parsing helpers.
// Split out of commands.cpp; run_command's dispatch is unchanged.
#include "cli/commands_detail.hpp"

namespace cidx::cli {

namespace {

std::string str_trim(const std::string &s) {
  std::size_t a = s.find_first_not_of(" \t");
  if (a == std::string::npos) {
    return "";
  }
  std::size_t b = s.find_last_not_of(" \t");
  return s.substr(a, b - a + 1);
}

std::string str_lower(std::string s) {
  std::ranges::transform(s, s.begin(),
                         [](unsigned char c) { return std::tolower(c); });
  return s;
}

// Parse 'FIELD = VALUE' in any spacing -> (field, value). Returns false on a
// malformed assignment. Mirrors cli.py _parse_assignment.
bool parse_assignment(const std::vector<std::string> &tokens, std::string &key,
                      std::string &val) {
  std::string expr;
  for (std::size_t k = 0; k < tokens.size(); ++k) {
    if (k != 0) {
      expr += " ";
    }
    expr += tokens[k];
  }
  const std::size_t eq = expr.find('=');
  if (eq != std::string::npos) {
    key = expr.substr(0, eq);
    val = expr.substr(eq + 1);
  } else {
    // exactly two whitespace-separated tokens ("pending False")
    std::vector<std::string> parts;
    std::size_t p = 0;
    while (p < expr.size()) {
      while (p < expr.size() && (expr[p] == ' ' || expr[p] == '\t')) {
        ++p;
      }
      std::size_t q = p;
      while (q < expr.size() && expr[q] != ' ' && expr[q] != '\t') {
        ++q;
      }
      if (q > p) {
        parts.push_back(expr.substr(p, q - p));
      }
      p = q;
    }
    if (parts.size() != 2) {
      return false;
    }
    key = parts[0];
    val = parts[1];
  }
  key = str_lower(str_trim(key));
  val = str_trim(val);
  return !key.empty() && !val.empty();
}

// true/false/1/0/yes/no/on/off (case-insensitive). Mirrors _parse_set_bool.
bool parse_set_bool(const std::string &raw, bool &out) {
  const std::string t = str_lower(str_trim(raw));
  if (t == "true" || t == "1" || t == "yes" || t == "on" || t == "t" ||
      t == "y") {
    out = true;
    return true;
  }
  if (t == "false" || t == "0" || t == "no" || t == "off" || t == "f" ||
      t == "n") {
    out = false;
    return true;
  }
  return false;
}

// -- `cidx file` / `dump-compile-commands` helpers --------------------------

// JSON string literal matching Python json.dumps default (ensure_ascii):
// escapes " \ and the C0 control chars (short forms for \b\f\n\r\t, \u00xx
// otherwise); bytes >= 0x20 pass through (ASCII inputs are byte-identical to
// Python — paths/flags are ASCII).
std::string py_json_string(const std::string &s) {
  std::string out = "\"";
  for (const unsigned char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    default:
      if (c < 0x20) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
        out += buf;
      } else {
        out += static_cast<char>(c);
      }
    }
  }
  out += "\"";
  return out;
}

// json.dumps(list[str]) default form: ["a", "b"] (", " separator). Used by
// `cidx file -dump-args`.
std::string py_json_str_array(const std::vector<std::string> &items) {
  std::string out = "[";
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i != 0) {
      out += ", ";
    }
    out += py_json_string(items[i]);
  }
  out += "]";
  return out;
}

struct CcEntry {
  std::string directory;
  std::string file;
  std::vector<std::string> arguments;
};

// json.dumps(entries, indent=2): the compile_commands.json array. Byte-matches
// Python's pretty form (2-space indent, ": " / ",\n" separators).
std::string dump_cc_json(const std::vector<CcEntry> &entries) {
  if (entries.empty()) {
    return "[]";
  }
  std::string out = "[\n";
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const CcEntry &e = entries[i];
    out += "  {\n";
    out += "    \"directory\": " + py_json_string(e.directory) + ",\n";
    out += "    \"file\": " + py_json_string(e.file) + ",\n";
    out += "    \"arguments\": ";
    if (e.arguments.empty()) {
      out += "[]";
    } else {
      out += "[\n";
      for (std::size_t j = 0; j < e.arguments.size(); ++j) {
        out += "      " + py_json_string(e.arguments[j]);
        out += (j + 1 < e.arguments.size()) ? ",\n" : "\n";
      }
      out += "    ]";
    }
    out += "\n  }";
    out += (i + 1 < entries.size()) ? ",\n" : "\n";
  }
  out += "]";
  return out;
}

// compiledb.commands_from_text (Python): write the JSON (a lone entry object is
// wrapped in an array) to a throwaway compile_commands.json and load it through
// the same CompilationDatabase path `import` uses, so `-import-args` strips
// args identically. Each entry needs directory, file, and arguments/command.
std::vector<CompileCommand> commands_from_text(const std::string &text) {
  std::size_t b = 0;
  while (b < text.size() &&
         std::isspace(static_cast<unsigned char>(text[b])) != 0) {
    ++b;
  }
  const std::string payload =
      (b < text.size() && text[b] == '{') ? ("[" + text + "]") : text;
  char tmpl[] = "/tmp/cidx_file_XXXXXX";
  char *dir = ::mkdtemp(tmpl);
  if (dir == nullptr) {
    throw CidxError("could not create a temporary directory for -import-args");
  }
  const std::string dpath = dir;
  const std::string fpath = dpath + "/compile_commands.json";
  {
    std::ofstream fh(fpath);
    fh << payload;
  }
  std::vector<CompileCommand> out;
  try {
    out = CompileDb::load(dpath);
  } catch (...) {
    ::unlink(fpath.c_str());
    ::rmdir(dpath.c_str());
    throw;
  }
  ::unlink(fpath.c_str());
  ::rmdir(dpath.c_str());
  return out;
}

// _parse_file_target (cli.py): 'COMPONENT://RELPATH' -> (component, abs_path).
// false + err set on a malformed target or unknown component. The relative
// path resolves against the component root; a leading '/' is stripped so the
// address can never escape the component.
bool parse_file_target(Storage &db, const std::string &target,
                       std::optional<Component> &comp, std::string &abs_path,
                       std::string &err) {
  const std::string sep = "://";
  const std::size_t pos = target.find(sep);
  const std::string malformed =
      "expected COMPONENT://PATH (e.g. 'mylib://src/foo.c'), got " +
      fmt::py_repr(target);
  if (pos == std::string::npos) {
    err = malformed;
    return false;
  }
  const std::string comp_name = target.substr(0, pos);
  std::string rel = target.substr(pos + sep.size());
  if (comp_name.empty() || rel.empty()) {
    err = malformed;
    return false;
  }
  comp = db.get_component_by_name(comp_name);
  if (!comp) {
    err = "no component named " + fmt::py_repr(comp_name);
    return false;
  }
  while (!rel.empty() && rel.front() == '/') {
    rel.erase(rel.begin());
  }
  abs_path = pathutil::normpath(pathutil::join(db.component_abs_base(*comp), rel));
  return true;
}

} // namespace

// cmd_set (cli.py cmd_set): set a mutable file attribute (the pending/indexed
// flag) over a component's files or one file, WITHOUT deleting any symbols.
int cmd_set(const ParsedArgs &args, Context &ctx) {
  std::string key;
  std::string raw_val;
  if (!parse_assignment(args.assignment, key, raw_val)) {
    *ctx.err << "error: expected 'FIELD=VALUE' (e.g. pending=False)\n";
    return 1;
  }
  // field -> invert (pending is the inverse of the 'indexed' flag).
  bool invert = false;
  if (key == "pending") {
    invert = true;
  } else if (key == "indexed") {
    invert = false;
  } else {
    *ctx.err << "error: unknown field '" << key
             << "'; supported: indexed, pending\n";
    return 1;
  }
  bool bval = false;
  if (!parse_set_bool(raw_val, bval)) {
    *ctx.err << "error: expected a boolean (true/false), got '" << raw_val
             << "'\n";
    return 1;
  }
  const bool indexed_value = invert ? !bval : bval;

  Storage db(ctx.index_path);
  std::optional<Component> comp;
  if (!lookup_component(db, args.component, comp, *ctx.err)) {
    return 1;
  }
  std::vector<std::pair<int64_t, std::string>> matches; // (id, abs path)
  if (args.file_filter) {
    const std::string ap = files::resolve_file_arg(
        *args.file_filter,
        comp ? std::optional<std::string>(db.component_abs_base(*comp)) : std::nullopt);
    if (std::optional<File> rec = db.get_file(ap)) {
      if (under_component(db, ap, comp)) {
        matches.emplace_back(rec->id, ap);
      }
    }
  } else {
    for (const std::pair<File, std::string> &pr : db.list_files(
             comp ? std::optional<int64_t>(comp->id) : std::nullopt)) {
      matches.emplace_back(pr.first.id, pr.second);
    }
  }
  if (matches.empty()) {
    *ctx.err << "error: no files match the given selector\n";
    return 1;
  }
  for (const std::pair<int64_t, std::string> &m : matches) {
    *ctx.out << "  #" << m.first << "  " << m.second << "\n";
  }
  if (!args.dry_run) {
    for (const std::pair<int64_t, std::string> &m : matches) {
      db.set_file_indexed(m.first, indexed_value);
    }
  }
  *ctx.out << (args.dry_run ? "would set " : "set ") << key << "="
           << (bval ? "True" : "False") << " on " << matches.size() << " "
           << plural(matches.size(), "file", "files") << "\n";
  return 0;
}

// cmd_file (cli.py cmd_file): inspect or edit one file's stored compile flags,
// addressed as COMPONENT://RELPATH. Edits mark the file args_overridden so a
// later `import` (without --force) keeps them.
int cmd_file(const ParsedArgs &args, Context &ctx) {
  std::vector<std::string> op = args.op;
  if (op.empty()) {
    op.emplace_back("-dump-args");
  }
  const std::string action = op[0];
  const std::vector<std::string> rest(op.begin() + 1, op.end());
  static const char *const kFileOps[] = {"-set-flag", "-unset-flag",
                                         "-import-args", "-dump-args"};
  bool known = false;
  for (const char *o : kFileOps) {
    if (action == o) {
      known = true;
    }
  }
  if (!known) {
    *ctx.err << "error: unknown operation " << fmt::py_repr(action)
             << "; supported: -set-flag, -unset-flag, -import-args, "
                "-dump-args\n";
    return 2;
  }

  Storage db(ctx.index_path);
  std::optional<Component> comp;
  std::string ap;
  std::string err;
  if (!parse_file_target(db, args.target, comp, ap, err)) {
    *ctx.err << "error: " << err << "\n";
    return 1;
  }
  const std::optional<File> rec = db.get_file(ap);
  if (!rec) {
    *ctx.err << "error: not in index database: " << ap << "\n";
    return 1;
  }
  std::vector<std::string> opts =
      rec->compile_options ? *rec->compile_options : std::vector<std::string>{};

  if (action == "-dump-args") {
    *ctx.out << py_json_str_array(opts) << "\n";
    return 0;
  }

  if (action == "-set-flag" || action == "-unset-flag") {
    if (rest.size() != 1) {
      *ctx.err << "error: " << action << " takes exactly one FLAG\n";
      return 2;
    }
    const std::string &flag = rest[0];
    if (action == "-set-flag") {
      if (std::ranges::find(opts, flag) != opts.end()) {
        *ctx.out << "flag already present on " << ap << ": " << flag << "\n";
        return 0;
      }
      opts.push_back(flag);
      db.set_file_compile_options(rec->id, opts);
      *ctx.out << "added flag to " << ap << ": " << flag << "\n";
      return 0;
    }
    const auto n =
        static_cast<std::size_t>(std::count(opts.begin(), opts.end(), flag));
    if (n == 0) {
      *ctx.out << "flag not present on " << ap << ": " << flag << "\n";
      return 0;
    }
    std::vector<std::string> kept;
    for (const std::string &o : opts) {
      if (o != flag) {
        kept.push_back(o);
      }
    }
    db.set_file_compile_options(rec->id, kept);
    *ctx.out << "removed flag from " << ap << ": " << flag << " (" << n << " "
             << plural(n, "occurrence", "occurrences") << ")\n";
    return 0;
  }

  // -import-args
  if (rest.size() != 1) {
    *ctx.err << "error: -import-args takes exactly one JSON entry (or @FILE)\n";
    return 2;
  }
  std::string raw = rest[0];
  if (!raw.empty() && raw[0] == '@') {
    const std::string path = raw.substr(1);
    std::ifstream fh(path);
    if (!fh) {
      *ctx.err << "error: cannot read " << path << ": "
               << std::strerror(errno) << "\n";
      return 1;
    }
    std::stringstream ss;
    ss << fh.rdbuf();
    raw = ss.str();
  }
  std::vector<CompileCommand> commands;
  try {
    commands = commands_from_text(raw);
  } catch (const CidxError &e) {
    *ctx.err << "error: -import-args: cannot parse compile command: " << e.what()
             << "\n";
    return 1;
  }
  if (commands.empty()) {
    *ctx.err << "error: -import-args: no compile command found (need directory, "
                "file, and arguments/command)\n";
    return 1;
  }
  const CompileCommand &cmd = commands[0];
  db.set_file_compile_options(rec->id, cmd.args, cmd.driver, true);
  *ctx.out << "imported " << cmd.args.size() << " arg(s) for " << ap << "\n";
  return 0;
}

// cmd_dump_compile_commands (cli.py): emit a compile_commands.json for a
// component — one entry per file that has stored compile flags.
int cmd_dump_compile_commands(const ParsedArgs &args, Context &ctx) {
  Storage db(ctx.index_path);
  std::optional<Component> comp;
  if (!lookup_component(db, args.component, comp, *ctx.err)) {
    return 1;
  }
  std::vector<CcEntry> entries;
  const auto files = db.list_files(
      comp ? std::optional<int64_t>(comp->id) : std::nullopt);
  for (const std::pair<File, std::string> &pr : files) {
    const File &f = pr.first;
    const std::string &ap = pr.second;
    if (!f.compile_options || f.compile_options->empty()) {
      continue;
    }
    CcEntry e;
    // v14: use effective root (base+version) as the directory field so the
    // emitted compile_commands.json paths are consistent with the stored flags.
    e.directory =
        comp ? db.component_abs_base(*comp) : pathutil::split(ap).first;
    e.file = ap;
    e.arguments.push_back(f.driver ? *f.driver : "cc");
    for (const std::string &o : *f.compile_options) {
      e.arguments.push_back(o);
    }
    e.arguments.push_back(ap);
    entries.push_back(std::move(e));
  }
  *ctx.out << dump_cc_json(entries) << "\n";
  return 0;
}

// ============================================================================
// M6 graph command group helpers
// ============================================================================

} // namespace cidx::cli
