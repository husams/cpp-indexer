// Deterministic text and JSON rendering (docs/diff.md "Report"): fixed key
// order, entities sorted by (kind, qualified name, USR), edits in source
// order, LF endings. Blocks excluded by --mode render as null / are omitted.
#include "diff/report.hpp"

#include <algorithm>
#include <fstream>
#include <tuple>

#include "cli/args.hpp"
#include "cli/json_out.hpp"

namespace cidx {
namespace diff {

namespace {

namespace jo = json_out;

constexpr int kReportVersion = 1;

const Entity *any_entity(const EntityPair &p) {
  return p.left != nullptr ? p.left : p.right;
}

std::vector<const EntityPair *> sorted_rows(const Comparison &cmp) {
  std::vector<const EntityPair *> rows;
  rows.reserve(cmp.pairs.size());
  for (const EntityPair &p : cmp.pairs)
    rows.push_back(&p);
  std::sort(rows.begin(), rows.end(),
            [](const EntityPair *a, const EntityPair *b) {
              const auto key = [](const EntityPair *p) {
                return std::make_tuple(
                    any_entity(*p)->kind, any_entity(*p)->name,
                    p->left != nullptr ? p->left->usr : std::string(),
                    p->right != nullptr ? p->right->usr : std::string());
              };
              return key(a) < key(b);
            });
  return rows;
}

std::string fmt_range(const SrcRange &r) {
  return std::to_string(r.line) + ":" + std::to_string(r.col) + "-" +
         std::to_string(r.end_line) + ":" + std::to_string(r.end_col);
}

std::string fmt_op_ranges(const EditOp &e) {
  std::string out = "L";
  out += e.left_range ? fmt_range(*e.left_range) : "-";
  out += " R";
  out += e.right_range ? fmt_range(*e.right_range) : "-";
  return out;
}

jo::Value opt_str(const std::optional<std::string> &v) {
  return v ? jo::Value::of(*v) : jo::Value::null();
}

jo::Value str_or_null(const std::string &s) {
  return s.empty() ? jo::Value::null() : jo::Value::of(s);
}

jo::Value range_value(const SrcRange &r) {
  return jo::Value::obj({{"line", jo::Value::of(r.line)},
                         {"col", jo::Value::of(r.col)},
                         {"end_line", jo::Value::of(r.end_line)},
                         {"end_col", jo::Value::of(r.end_col)}});
}

jo::Value side_value(const ParseConfig &c) {
  return jo::Value::obj({{"file", jo::Value::of(c.file)},
                         {"db", jo::Value::of(c.db)},
                         {"tu", opt_str(c.tu)},
                         {"driver", opt_str(c.driver)},
                         {"std", opt_str(c.classes.standard)},
                         {"target", opt_str(c.classes.target)}});
}

jo::Value pair_value(const std::optional<std::pair<std::string, std::string>> &p) {
  if (!p)
    return jo::Value::null();
  return jo::Value::arr({jo::Value::of(p->first), jo::Value::of(p->second)});
}

jo::Value str_array(const std::vector<std::string> &v) {
  jo::Array a;
  for (const std::string &s : v)
    a.push_back(jo::Value::of(s));
  return jo::Value::arr(std::move(a));
}

jo::Value delta_value(const ConfigDelta &d) {
  return jo::Value::obj(
      {{"identical", jo::Value::of(d.identical)},
       {"std", pair_value(d.standard)},
       {"target", pair_value(d.target)},
       {"driver", pair_value(d.driver)},
       {"definitions_added", str_array(d.definitions_added)},
       {"definitions_removed", str_array(d.definitions_removed)},
       {"definitions_reordered", jo::Value::of(d.definitions_reordered)},
       {"includes_changed", jo::Value::of(d.includes_changed)},
       {"options_left_only", str_array(d.options_left_only)},
       {"options_right_only", str_array(d.options_right_only)},
       {"options_reordered", jo::Value::of(d.options_reordered)}});
}

jo::Value edit_value(const EditOp &e) {
  return jo::Value::obj(
      {{"op", jo::Value::of(e.op)},
       {"node", jo::Value::of(e.node)},
       {"detail", jo::Value::of(e.detail)},
       {"left_range",
        e.left_range ? range_value(*e.left_range) : jo::Value::null()},
       {"right_range",
        e.right_range ? range_value(*e.right_range) : jo::Value::null()}});
}

jo::Value unsupported_value(const Unsupported &u) {
  return jo::Value::obj({{"what", jo::Value::of(u.what)},
                         {"side", jo::Value::of(u.side)},
                         {"range", range_value(u.range)}});
}

jo::Value entity_value(const EntityPair &p, bool with_syntax,
                       bool with_semantic) {
  const Entity *e = any_entity(p);
  jo::Value syntax = jo::Value::null();
  if (with_syntax) {
    jo::Array edits;
    for (const EditOp &op : p.edits)
      edits.push_back(edit_value(op));
    syntax = jo::Value::obj(
        {{"status", jo::Value::of(std::string(
              p.subtree_differs ? "changed" : "unchanged"))},
         {"edit_count", jo::Value::of(static_cast<long long>(
                            p.edits.size()))},
         {"truncated", jo::Value::of(p.truncated)},
         {"edits", jo::Value::arr(std::move(edits))}});
  }
  jo::Value semantic = jo::Value::null();
  if (with_semantic) {
    jo::Array unsupported;
    for (const Unsupported &u : p.unsupported)
      unsupported.push_back(unsupported_value(u));
    semantic = jo::Value::obj(
        {{"verdict", jo::Value::of(p.verdict)},
         {"evidence", jo::Value::of(p.evidence)},
         {"detail", jo::Value::of(p.detail)},
         {"changes", str_array(p.changes)},
         {"unsupported", jo::Value::arr(std::move(unsupported))}});
  }
  return jo::Value::obj(
      {{"kind", jo::Value::of(e->kind)},
       {"name", jo::Value::of(e->name)},
       {"signature", str_or_null(e->signature)},
       {"status", jo::Value::of(p.status)},
       {"match", str_or_null(p.match)},
       {"confidence", p.confidence > 0 ? jo::Value::of(p.confidence)
                                       : jo::Value::null()},
       {"left_usr", p.left != nullptr ? str_or_null(p.left->usr)
                                      : jo::Value::null()},
       {"right_usr", p.right != nullptr ? str_or_null(p.right->usr)
                                        : jo::Value::null()},
       {"left_range",
        p.left != nullptr ? range_value(p.left->range) : jo::Value::null()},
       {"right_range",
        p.right != nullptr ? range_value(p.right->range) : jo::Value::null()},
       {"syntax", std::move(syntax)},
       {"semantic", std::move(semantic)}});
}

void render_json(const ReportSpec &spec, const SideAnalysis &left,
                 const SideAnalysis &right, const ConfigDelta &delta,
                 const Comparison &cmp,
                 const std::vector<const EntityPair *> &rows, bool with_syntax,
                 bool with_semantic, std::ostream &out) {
  jo::Array entities;
  for (const EntityPair *p : rows)
    entities.push_back(entity_value(*p, with_syntax, with_semantic));
  jo::Value syntax = jo::Value::null();
  if (with_syntax)
    syntax = jo::Value::obj(
        {{"status", jo::Value::of(std::string(
              cmp.syntax_changed ? "changed" : "unchanged"))},
         {"edit_count", jo::Value::of(cmp.edit_count)},
         {"truncated", jo::Value::of(cmp.truncated)}});
  jo::Value semantic = jo::Value::null();
  if (with_semantic)
    semantic = jo::Value::obj(
        {{"verdict", jo::Value::of(cmp.verdict)},
         {"evidence", jo::Value::of(cmp.evidence)},
         {"assumptions", str_array(cmp.assumptions)},
         {"unsupported_count", jo::Value::of(cmp.unsupported_count)},
         {"detail", jo::Value::of(cmp.detail)}});
  const jo::Value root = jo::Value::obj(
      {{"tool", jo::Value::of(std::string("cidx-diff"))},
       {"version", jo::Value::of(std::string(cli::kVersion))},
       {"report_version", jo::Value::of(kReportVersion)},
       {"mode", jo::Value::of(spec.mode)},
       {"scope", jo::Value::of(spec.scope)},
       {"match", jo::Value::of(spec.match)},
       {"left", side_value(left.config)},
       {"right", side_value(right.config)},
       {"config_delta", delta_value(delta)},
       {"entities", jo::Value::arr(std::move(entities))},
       {"syntax", std::move(syntax)},
       {"semantic", std::move(semantic)}});
  out << jo::dumps_indent2(root) << "\n";
}

std::vector<std::string> read_lines(const std::string &path) {
  std::ifstream in(path);
  std::vector<std::string> lines;
  std::string l;
  while (std::getline(in, l))
    lines.push_back(l);
  return lines;
}

void print_context(char side, const std::vector<std::string> &lines,
                   const SrcRange &r, int n, const std::string &indent,
                   std::ostream &out) {
  // 64-bit bounds: a huge --context must clamp, not overflow int.
  const long long lo =
      std::max(1LL, static_cast<long long>(r.line) - static_cast<long long>(n));
  const long long hi =
      std::min(static_cast<long long>(lines.size()),
               static_cast<long long>(r.end_line) + static_cast<long long>(n));
  for (long long ln = lo; ln <= hi; ++ln)
    out << indent << "  " << side << ln << " | "
        << lines[static_cast<std::size_t>(ln - 1)] << "\n";
}

std::string op_line(const EditOp &e) {
  std::string s = e.op;
  if (s.size() < 7)
    s.resize(7, ' ');
  s += "  " + e.node;
  if (!e.detail.empty())
    s += "  " + e.detail;
  s += "  " + fmt_op_ranges(e);
  return s;
}

void render_delta_text(const ConfigDelta &d, std::ostream &out) {
  if (d.identical) {
    out << "config: identical\n";
    return;
  }
  out << "config: different\n";
  const auto axis = [&out](const char *name,
                           const std::optional<std::pair<std::string,
                                                         std::string>> &p) {
    if (p)
      out << "  " << name << ": " << (p->first.empty() ? "-" : p->first)
          << " -> " << (p->second.empty() ? "-" : p->second) << "\n";
  };
  axis("std", d.standard);
  axis("target", d.target);
  axis("driver", d.driver);
  const auto list = [&out](const char *name,
                           const std::vector<std::string> &v) {
    if (v.empty())
      return;
    out << "  " << name << ":";
    for (const std::string &s : v)
      out << " " << s;
    out << "\n";
  };
  list("definitions added", d.definitions_added);
  list("definitions removed", d.definitions_removed);
  if (d.definitions_reordered)
    out << "  definitions: reordered\n";
  if (d.includes_changed)
    out << "  includes: changed\n";
  list("options left only", d.options_left_only);
  list("options right only", d.options_right_only);
  if (d.options_reordered)
    out << "  options: reordered\n";
}

void render_semantic_notes(const EntityPair &p, const std::string &indent,
                           std::ostream &out) {
  for (const std::string &c : p.changes)
    out << indent << "change: " << c << "\n";
  for (const Unsupported &u : p.unsupported)
    out << indent << "unsupported: " << u.what << " (" << u.side << " "
        << fmt_range(u.range) << ")\n";
}

void render_text(const ReportSpec &spec, const SideAnalysis &left,
                 const SideAnalysis &right, const ConfigDelta &delta,
                 const Comparison &cmp,
                 const std::vector<const EntityPair *> &rows, bool with_syntax,
                 bool with_semantic, std::ostream &out) {
  const bool symbol = spec.scope == "symbol";
  out << "cidx-diff: " << spec.scope << "  mode: " << spec.mode
      << "  match: " << spec.match << "\n";
  const EntityPair *main_pair = nullptr;
  if (symbol)
    for (const EntityPair *p : rows)
      if (!p->member)
        main_pair = p;
  const auto side_line = [&](const char *label, const std::string &file,
                             const Entity *e) {
    out << label << file;
    if (e != nullptr)
      out << " :: " << (e->signature.empty() ? e->name : e->signature);
    out << "\n";
  };
  side_line("left:  ", left.config.file,
            main_pair != nullptr ? main_pair->left : nullptr);
  side_line("right: ", right.config.file,
            main_pair != nullptr ? main_pair->right : nullptr);
  render_delta_text(delta, out);
  if (with_syntax) {
    if (!cmp.syntax_changed) {
      out << "syntax: unchanged\n";
    } else {
      out << "syntax: changed (" << cmp.edit_count
          << (cmp.edit_count == 1 ? " edit" : " edits")
          << (cmp.truncated ? ", truncated" : "") << ")\n";
      std::vector<std::string> left_lines;
      std::vector<std::string> right_lines;
      if (spec.context > 0) {
        left_lines = read_lines(left.config.file);
        right_lines = read_lines(right.config.file);
      }
      const std::string indent = symbol ? "  " : "    ";
      for (const EntityPair *p : rows) {
        // Member rows repeat edits already inside the record pair's script.
        if (p->member || !p->subtree_differs)
          continue;
        if (!symbol) {
          out << "  " << any_entity(*p)->kind << " " << any_entity(*p)->name
              << "  " << p->status;
          if (!p->match.empty())
            out << " (" << p->match << " " << p->confidence << ")";
          out << "\n";
        }
        for (const EditOp &e : p->edits) {
          out << indent << op_line(e) << "\n";
          if (spec.context > 0) {
            if (e.left_range)
              print_context('L', left_lines, *e.left_range, spec.context,
                            indent, out);
            if (e.right_range)
              print_context('R', right_lines, *e.right_range, spec.context,
                            indent, out);
          }
        }
      }
    }
  }
  if (!with_semantic)
    return;
  out << "semantic: " << cmp.verdict << " (" << cmp.detail << ")\n";
  if (!cmp.assumptions.empty()) {
    out << "  assumptions:";
    for (const std::string &a : cmp.assumptions)
      out << " " << a;
    out << "\n";
  }
  if (main_pair != nullptr)
    render_semantic_notes(*main_pair, "  ", out);
  for (const EntityPair *p : rows) {
    if (p == main_pair)
      continue;
    if (p->verdict == verdict::kEquivalent && p->changes.empty() &&
        p->unsupported.empty())
      continue;
    out << "  " << any_entity(*p)->kind << " " << any_entity(*p)->name << "  "
        << p->verdict << " (" << p->detail << ")\n";
    render_semantic_notes(*p, "    ", out);
  }
}

} // namespace

void render_report(const ReportSpec &spec, const SideAnalysis &left,
                   const SideAnalysis &right, const ConfigDelta &delta,
                   const Comparison &cmp, std::ostream &out) {
  const std::vector<const EntityPair *> rows = sorted_rows(cmp);
  const bool with_syntax = spec.mode == "syntax" || spec.mode == "both";
  const bool with_semantic = spec.mode == "semantic" || spec.mode == "both";
  if (spec.json)
    render_json(spec, left, right, delta, cmp, rows, with_syntax,
                with_semantic, out);
  else
    render_text(spec, left, right, delta, cmp, rows, with_syntax,
                with_semantic, out);
}

} // namespace diff
} // namespace cidx
