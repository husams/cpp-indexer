// Entity matching (contract tiers usr/signature/name/fingerprint), the
// recursive AST edit script (LCS child alignment on fingerprints, bounded by
// kLcsLimit and kEditCap), and the tri-state semantic verdicts of
// docs/diff.md "Semantic model". Clang-free.
#include "diff/compare.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>

#include "diff/behaviour_ir.hpp"

namespace cidx {
namespace diff {

namespace {

std::string strip_ws(const std::string &s) {
  std::string out;
  for (const char c : s)
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
      out += c;
  return out;
}

struct OpSink {
  std::vector<EditOp> *ops;
  int *total;
  bool truncated = false;

  void add(EditOp op) {
    if (*total >= kEditCap) {
      truncated = true;
      return;
    }
    ++*total;
    ops->push_back(std::move(op));
  }
};

std::string describe(const SynNode &n) {
  return n.detail.empty() ? n.kind : n.detail;
}

// "callee reserve" vs "callee resize" -> "callee reserve -> resize": a shared
// leading aspect word is not repeated on the right-hand side.
std::string merged_detail(const std::string &l, const std::string &r) {
  const std::size_t sp = l.find(' ');
  if (sp != std::string::npos && r.size() > sp &&
      r.compare(0, sp + 1, l, 0, sp + 1) == 0)
    return l + " -> " + r.substr(sp + 1);
  return l + " -> " + r;
}

bool is_decl_name(const std::string &detail) {
  return detail.rfind("name ", 0) == 0;
}

void diff_node(const SynNode &l, const SynNode &r, OpSink &sink);

void one_sided(const SynNode &n, bool left, OpSink &sink) {
  EditOp op;
  op.op = left ? "removed" : "added";
  op.node = n.kind;
  op.detail = n.detail;
  if (left)
    op.left_range = n.range;
  else
    op.right_range = n.range;
  sink.add(std::move(op));
}

// Aligned index pairs of children with identical fingerprints (classic LCS).
std::vector<std::pair<std::size_t, std::size_t>>
lcs_pairs(const std::vector<SynNode> &l, const std::vector<SynNode> &r) {
  const std::size_t n = l.size();
  const std::size_t m = r.size();
  std::vector<int> dp((n + 1) * (m + 1), 0);
  const auto at = [&](std::size_t i, std::size_t j) -> int & {
    return dp[i * (m + 1) + j];
  };
  for (std::size_t i = n; i-- > 0;)
    for (std::size_t j = m; j-- > 0;)
      at(i, j) = l[i].fingerprint == r[j].fingerprint
                     ? at(i + 1, j + 1) + 1
                     : std::max(at(i + 1, j), at(i, j + 1));
  std::vector<std::pair<std::size_t, std::size_t>> out;
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < n && j < m) {
    if (l[i].fingerprint == r[j].fingerprint) {
      out.emplace_back(i, j);
      ++i;
      ++j;
    } else if (at(i + 1, j) >= at(i, j + 1)) {
      ++i;
    } else {
      ++j;
    }
  }
  return out;
}

// Unanchored runs between LCS anchors pair positionally; the surplus is
// added/removed.
void diff_gap(const std::vector<SynNode> &l, std::size_t lb, std::size_t le,
              const std::vector<SynNode> &r, std::size_t rb, std::size_t re,
              OpSink &sink) {
  while (lb < le && rb < re)
    diff_node(l[lb++], r[rb++], sink);
  while (lb < le)
    one_sided(l[lb++], /*left=*/true, sink);
  while (rb < re)
    one_sided(r[rb++], /*left=*/false, sink);
}

void diff_children(const std::vector<SynNode> &l, const std::vector<SynNode> &r,
                   OpSink &sink) {
  if (l.size() > static_cast<std::size_t>(kLcsLimit) ||
      r.size() > static_cast<std::size_t>(kLcsLimit)) {
    diff_gap(l, 0, l.size(), r, 0, r.size(), sink);
    return;
  }
  std::size_t i = 0;
  std::size_t j = 0;
  for (const auto &[ai, aj] : lcs_pairs(l, r)) {
    diff_gap(l, i, ai, r, j, aj, sink);
    i = ai + 1;
    j = aj + 1;
  }
  diff_gap(l, i, l.size(), r, j, r.size(), sink);
}

void diff_node(const SynNode &l, const SynNode &r, OpSink &sink) {
  if (l.fingerprint == r.fingerprint)
    return;
  if (l.kind != r.kind) {
    EditOp op;
    op.op = "replaced";
    op.node = l.kind;
    op.detail = describe(l) + " -> " + describe(r);
    op.left_range = l.range;
    op.right_range = r.range;
    sink.add(std::move(op));
    return;
  }
  if (l.label != r.label) {
    EditOp op;
    op.node = l.kind;
    op.left_range = l.range;
    op.right_range = r.range;
    if (l.detail != r.detail && is_decl_name(l.detail) &&
        is_decl_name(r.detail)) {
      op.op = "renamed";
      op.detail = merged_detail(l.detail, r.detail);
    } else {
      op.op = "changed";
      op.detail = l.detail != r.detail ? merged_detail(l.detail, r.detail)
                                       : l.label + " -> " + r.label;
    }
    sink.add(std::move(op));
  }
  diff_children(l.children, r.children, sink);
}

struct Matcher {
  const std::vector<const Entity *> &left;
  const std::vector<const Entity *> &right;
  std::vector<bool> lu;
  std::vector<bool> ru;
  std::vector<EntityPair> pairs;

  Matcher(const std::vector<const Entity *> &l,
          const std::vector<const Entity *> &r)
      : left(l), right(r), lu(l.size(), false), ru(r.size(), false) {}

  void pair(std::size_t i, std::size_t j, const char *tier, int conf,
            const char *status) {
    EntityPair p;
    p.left = left[i];
    p.right = right[j];
    p.status = status;
    p.match = tier;
    p.confidence = conf;
    pairs.push_back(std::move(p));
    lu[i] = true;
    ru[j] = true;
  }

  // Pair remaining entities whose keys are equal, zipped in source order.
  // unique_key restricts the tier to keys occurring exactly once per side.
  template <typename KeyFn>
  void tier(KeyFn key, const char *name, int conf, const char *status,
            bool unique_key) {
    std::map<std::string, int> lcount;
    std::map<std::string, int> rcount;
    if (unique_key) {
      for (const Entity *e : left)
        if (const std::string k = key(*e); !k.empty())
          ++lcount[k];
      for (const Entity *e : right)
        if (const std::string k = key(*e); !k.empty())
          ++rcount[k];
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
      if (lu[i])
        continue;
      const std::string k = key(*left[i]);
      if (k.empty())
        continue;
      if (unique_key && (lcount[k] != 1 || rcount[k] != 1))
        continue;
      for (std::size_t j = 0; j < right.size(); ++j) {
        if (!ru[j] && key(*right[j]) == k) {
          pair(i, j, name, conf, status);
          break;
        }
      }
    }
  }
};

std::vector<EntityPair> match_entities(const std::vector<const Entity *> &l,
                                       const std::vector<const Entity *> &r,
                                       const std::string &match_mode,
                                       bool force_pair) {
  Matcher m(l, r);
  m.tier([](const Entity &e) { return e.usr; }, "usr", 100, "matched", false);
  m.tier(
      [](const Entity &e) {
        return e.signature.empty() ? std::string()
                                   : e.kind + "\x1f" + strip_ws(e.signature);
      },
      "signature", 95, "matched", false);
  if (match_mode == "heuristic") {
    m.tier(
        [](const Entity &e) {
          return e.name.empty() ? std::string() : e.kind + "\x1f" + e.name;
        },
        "name", 85, "matched", true);
    m.tier([](const Entity &e) { return e.kind + "\x1f" + e.fingerprint; },
           "fingerprint", 70, "renamed", true);
  }
  if (force_pair && l.size() == 1 && r.size() == 1 && !m.lu[0] && !m.ru[0])
    m.pair(0, 0, "", 0, "matched");
  for (std::size_t i = 0; i < l.size(); ++i) {
    if (m.lu[i])
      continue;
    EntityPair p;
    p.left = l[i];
    p.status = "removed";
    m.pairs.push_back(std::move(p));
  }
  for (std::size_t j = 0; j < r.size(); ++j) {
    if (m.ru[j])
      continue;
    EntityPair p;
    p.right = r[j];
    p.status = "added";
    m.pairs.push_back(std::move(p));
  }
  return m.pairs;
}

void compute_edits(EntityPair &p, int *total) {
  p.subtree_differs =
      p.left == nullptr || p.right == nullptr ||
      p.left->syntax.fingerprint != p.right->syntax.fingerprint;
  OpSink sink{&p.edits, total};
  if (p.left != nullptr && p.right != nullptr) {
    diff_node(p.left->syntax, p.right->syntax, sink);
  } else {
    // A one-sided entity is itself the edit: one added/removed op naming the
    // whole declaration.
    const Entity &e = p.left != nullptr ? *p.left : *p.right;
    EditOp op;
    op.op = p.left != nullptr ? "removed" : "added";
    op.node = e.syntax.kind;
    op.detail = e.kind + " " + e.name;
    if (p.left != nullptr)
      op.left_range = e.range;
    else
      op.right_range = e.range;
    sink.add(std::move(op));
  }
  p.truncated = sink.truncated;
}

const char *bool_str(bool b) { return b ? "true" : "false"; }

std::string or_dash(const std::string &s) { return s.empty() ? "-" : s; }

std::vector<std::string> signature_changes(const BehaviourIR &l,
                                           const BehaviourIR &r) {
  std::vector<std::string> out;
  if (l.return_type != r.return_type)
    out.push_back("return type: " + l.return_type + " -> " + r.return_type);
  if (l.param_types.size() != r.param_types.size()) {
    out.push_back("parameter count: " + std::to_string(l.param_types.size()) +
                  " -> " + std::to_string(r.param_types.size()));
  } else {
    for (std::size_t i = 0; i < l.param_types.size(); ++i)
      if (l.param_types[i] != r.param_types[i])
        out.push_back("parameter " + std::to_string(i + 1) + " type: " +
                      l.param_types[i] + " -> " + r.param_types[i]);
  }
  if (l.param_defaults.size() == r.param_defaults.size()) {
    for (std::size_t i = 0; i < l.param_defaults.size(); ++i)
      if (l.param_defaults[i] != r.param_defaults[i])
        out.push_back("default argument of parameter " +
                      std::to_string(i + 1) + " changed");
  }
  if (l.quals != r.quals)
    out.push_back("qualifiers: " + or_dash(l.quals) + " -> " +
                  or_dash(r.quals));
  if (l.is_noexcept != r.is_noexcept)
    out.push_back(std::string("noexcept: ") + bool_str(l.is_noexcept) +
                  " -> " + bool_str(r.is_noexcept));
  if (l.is_virtual != r.is_virtual)
    out.push_back(std::string("virtual: ") + bool_str(l.is_virtual) + " -> " +
                  bool_str(r.is_virtual));
  if (l.storage != r.storage)
    out.push_back("storage class: " + or_dash(l.storage) + " -> " +
                  or_dash(r.storage));
  if (l.is_inline != r.is_inline)
    out.push_back(std::string("inline: ") + bool_str(l.is_inline) + " -> " +
                  bool_str(r.is_inline));
  if (l.constexpr_kind != r.constexpr_kind)
    out.push_back("constexpr: " + or_dash(l.constexpr_kind) + " -> " +
                  or_dash(r.constexpr_kind));
  if (l.is_noreturn != r.is_noreturn)
    out.push_back(std::string("noreturn: ") + bool_str(l.is_noreturn) +
                  " -> " + bool_str(r.is_noreturn));
  if (l.is_static_method != r.is_static_method)
    out.push_back(std::string("static method: ") +
                  bool_str(l.is_static_method) + " -> " +
                  bool_str(r.is_static_method));
  return out;
}

// Names every difference between two row sequences: removed/added rows
// (multiset difference) plus a reorder of the rows present on both sides.
void seq_changes(const char *cat, const std::vector<std::string> &l,
                 const std::vector<std::string> &r,
                 std::vector<std::string> &out) {
  if (l == r)
    return;
  std::vector<std::string> ls = l;
  std::vector<std::string> rs = r;
  std::sort(ls.begin(), ls.end());
  std::sort(rs.begin(), rs.end());
  std::vector<std::string> removed;
  std::vector<std::string> added;
  std::set_difference(ls.begin(), ls.end(), rs.begin(), rs.end(),
                      std::back_inserter(removed));
  std::set_difference(rs.begin(), rs.end(), ls.begin(), ls.end(),
                      std::back_inserter(added));
  const auto keep_common = [](const std::vector<std::string> &rows,
                              const std::vector<std::string> &drop) {
    std::map<std::string, int> cnt;
    for (const std::string &s : drop)
      ++cnt[s];
    std::vector<std::string> out;
    for (const std::string &s : rows) {
      if (const auto it = cnt.find(s); it != cnt.end() && it->second > 0) {
        --it->second;
        continue;
      }
      out.push_back(s);
    }
    return out;
  };
  if (keep_common(l, removed) != keep_common(r, added))
    out.push_back(std::string(cat) + " reordered");
  for (const std::string &s : removed)
    out.push_back(std::string(cat) + " removed: " + s);
  for (const std::string &s : added)
    out.push_back(std::string(cat) + " added: " + s);
}

std::vector<std::string> profile_changes(const ClassProfile &l,
                                         const ClassProfile &r) {
  std::vector<std::string> out;
  seq_changes("base", l.bases, r.bases, out);
  seq_changes("field", l.fields, r.fields, out);
  seq_changes("method", l.methods, r.methods, out);
  seq_changes("template parameter", l.template_params, r.template_params, out);
  seq_changes("nested record", l.nested, r.nested, out);
  seq_changes("declaration", l.extras, r.extras, out);
  if (l.polymorphic != r.polymorphic)
    out.push_back(std::string("polymorphic: ") + bool_str(l.polymorphic) +
                  " -> " + bool_str(r.polymorphic));
  if (l.abstract != r.abstract)
    out.push_back(std::string("abstract: ") + bool_str(l.abstract) + " -> " +
                  bool_str(r.abstract));
  return out;
}

constexpr const char *kNoDifference = "no behavioral difference established";

std::optional<std::string> read_all(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.good())
    return std::nullopt;
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

// Computes per-pair verdicts (docs/diff.md "Semantic model"). For a record
// pair the members are matched and judged recursively; member_out collects
// the member rows a symbol-scope class target reports.
struct Verdicts {
  const SideAnalysis &ls;
  const SideAnalysis &rs;
  bool config_identical;
  const std::string &match_mode;

  const BehaviourIR *behaviour_of(const SideAnalysis &s,
                                  const Entity *e) const {
    return e != nullptr && e->behaviour >= 0
               ? &s.behaviours[static_cast<std::size_t>(e->behaviour)]
               : nullptr;
  }

  const ClassProfile *profile_of(const SideAnalysis &s,
                                 const Entity *e) const {
    return e != nullptr && e->profile >= 0
               ? &s.profiles[static_cast<std::size_t>(e->profile)]
               : nullptr;
  }

  void attach(EntityPair &p, const std::vector<Unsupported> &markers,
              const char *side) const {
    for (Unsupported u : markers) {
      u.side = side;
      p.unsupported.push_back(std::move(u));
    }
  }

  void set(EntityPair &p, const char *verdict, const char *evidence,
           std::string detail) const {
    p.verdict = verdict;
    p.evidence = evidence;
    p.detail = std::move(detail);
  }

  void equivalent_with_evidence(EntityPair &p, const char *ir_detail) const {
    if (config_identical && p.left->slice == p.right->slice)
      set(p, verdict::kEquivalent, evidence::kIdenticalSourceAndConfig,
          "identical source and configuration");
    else
      set(p, verdict::kEquivalent, evidence::kNormalizedIr, ir_detail);
  }

  void callable_verdict(EntityPair &p, const BehaviourIR &lb,
                        const BehaviourIR &rb) const {
    p.changes = signature_changes(lb, rb);
    attach(p, lb.unsupported, "left");
    attach(p, rb.unsupported, "right");
    if (!p.changes.empty()) {
      set(p, verdict::kDifferent, evidence::kSummaryContradiction,
          "signature contradiction");
      return;
    }
    if (!p.unsupported.empty()) {
      set(p, verdict::kUnknown, evidence::kUnsupportedOrIncomplete,
          std::string("unsupported constructs; ") + kNoDifference);
      return;
    }
    if (lb.has_body != rb.has_body) {
      set(p, verdict::kUnknown, evidence::kUnsupportedOrIncomplete,
          std::string("body on one side only; ") + kNoDifference);
      return;
    }
    if (!lb.has_body && !rb.has_body) {
      // Signatures already match (changes is empty); say so transparently.
      const char *ev = config_identical && p.left->slice == p.right->slice
                           ? evidence::kIdenticalSourceAndConfig
                           : evidence::kNormalizedIr;
      set(p, verdict::kEquivalent, ev,
          "declaration only (no body in this translation unit)");
      return;
    }
    if (lb.blocks == rb.blocks) {
      equivalent_with_evidence(p,
                               "identical canonical signature and normalized "
                               "IR");
      return;
    }
    set(p, verdict::kUnknown, evidence::kUnsupportedOrIncomplete,
        std::string("normalized IR differs; ") + kNoDifference);
  }

  void record_verdict(EntityPair &p, const ClassProfile &lp,
                      const ClassProfile &rp,
                      const std::vector<EntityPair> &members) const {
    p.changes = profile_changes(lp, rp);
    attach(p, lp.unsupported, "left");
    attach(p, rp.unsupported, "right");
    for (const EntityPair &mp : members)
      p.unsupported.insert(p.unsupported.end(), mp.unsupported.begin(),
                           mp.unsupported.end());
    if (!p.changes.empty()) {
      set(p, verdict::kDifferent, evidence::kSummaryContradiction,
          "class profile contradiction");
      return;
    }
    if (!p.unsupported.empty()) {
      set(p, verdict::kUnknown, evidence::kUnsupportedOrIncomplete,
          std::string("unsupported constructs; ") + kNoDifference);
      return;
    }
    for (const EntityPair &mp : members) {
      if (mp.left == nullptr || mp.right == nullptr ||
          mp.verdict != verdict::kEquivalent) {
        set(p, verdict::kUnknown, evidence::kUnsupportedOrIncomplete,
            std::string("member bodies differ; ") + kNoDifference);
        return;
      }
    }
    // A class equivalent under differing configurations downgrades to
    // unknown (docs/diff.md "Configuration delta").
    if (!config_identical) {
      set(p, verdict::kUnknown, evidence::kUnsupportedOrIncomplete,
          std::string("configuration differs; ") + kNoDifference);
      return;
    }
    equivalent_with_evidence(p, "identical class profile and member bodies");
  }

  void generic_verdict(EntityPair &p) const {
    if (!p.subtree_differs) {
      equivalent_with_evidence(p, "identical normalized syntax");
      return;
    }
    set(p, verdict::kUnknown, evidence::kUnsupportedOrIncomplete,
        std::string("syntax differs; ") + kNoDifference);
  }

  void judge(EntityPair &p, std::vector<EntityPair> *member_out) const {
    if (p.left == nullptr || p.right == nullptr) {
      set(p, verdict::kDifferent, evidence::kSummaryContradiction,
          "entity " + p.status);
      return;
    }
    const BehaviourIR *lb = behaviour_of(ls, p.left);
    const BehaviourIR *rb = behaviour_of(rs, p.right);
    if (lb != nullptr && rb != nullptr) {
      callable_verdict(p, *lb, *rb);
      return;
    }
    const ClassProfile *lp = profile_of(ls, p.left);
    const ClassProfile *rp = profile_of(rs, p.right);
    if (lp != nullptr && rp != nullptr) {
      std::vector<const Entity *> lm;
      std::vector<const Entity *> rm;
      for (const Entity &m : p.left->members)
        lm.push_back(&m);
      for (const Entity &m : p.right->members)
        rm.push_back(&m);
      std::vector<EntityPair> members =
          match_entities(lm, rm, match_mode, /*force_pair=*/false);
      int member_total = 0;
      for (EntityPair &mp : members) {
        mp.member = true;
        compute_edits(mp, &member_total);
        judge(mp, nullptr);
      }
      record_verdict(p, *lp, *rp, members);
      if (member_out != nullptr)
        for (EntityPair &mp : members)
          member_out->push_back(std::move(mp));
      return;
    }
    generic_verdict(p);
  }
};

} // namespace

Comparison compare_sides(const SideAnalysis &left, const SideAnalysis &right,
                         const std::string &match_mode,
                         const ConfigDelta &delta, const std::string &scope) {
  const bool symbol = scope == "symbol";
  std::vector<const Entity *> lt;
  std::vector<const Entity *> rt;
  for (const Entity &e : left.entities)
    lt.push_back(&e);
  for (const Entity &e : right.entities)
    rt.push_back(&e);
  std::vector<EntityPair> tops =
      match_entities(lt, rt, match_mode, /*force_pair=*/symbol);

  Comparison c;
  int total = 0;
  for (EntityPair &p : tops) {
    compute_edits(p, &total);
    if (p.truncated)
      c.truncated = true;
    if (p.subtree_differs)
      c.syntax_changed = true;
  }
  c.edit_count = total;

  const Verdicts v{left, right, delta.identical, match_mode};
  std::vector<EntityPair> member_rows;
  for (EntityPair &p : tops)
    v.judge(p, symbol ? &member_rows : nullptr);

  int added_removed = 0;
  int different_count = 0;
  bool all_equivalent = true;
  bool all_identical_source = true;
  for (const EntityPair &p : tops) {
    c.unsupported_count += static_cast<int>(p.unsupported.size());
    if (p.left == nullptr || p.right == nullptr)
      ++added_removed;
    else if (p.verdict == verdict::kDifferent)
      ++different_count;
    if (p.verdict != verdict::kEquivalent)
      all_equivalent = false;
    if (p.evidence != evidence::kIdenticalSourceAndConfig)
      all_identical_source = false;
  }
  if (symbol && !tops.empty()) {
    // The whole-comparison verdict is the selected pair's verdict (the class
    // tier already aggregates its members and the configuration downgrade).
    c.verdict = tops.front().verdict;
    c.evidence = tops.front().evidence;
    c.detail = tops.front().detail;
  } else if (tops.empty()) {
    // Zero indexed entities on both sides (macro-/pragma-only files): the
    // trees say nothing, so equivalence needs the raw bytes to be identical
    // under an identical configuration (docs/diff.md "Files").
    const std::optional<std::string> lbytes = read_all(left.config.file);
    const std::optional<std::string> rbytes = read_all(right.config.file);
    if (delta.identical && lbytes && rbytes && *lbytes == *rbytes) {
      c.verdict = verdict::kEquivalent;
      c.evidence = evidence::kIdenticalSourceAndConfig;
      c.detail = "identical source and configuration (no indexed entities)";
    } else {
      c.verdict = verdict::kUnknown;
      c.evidence = evidence::kUnsupportedOrIncomplete;
      c.detail = "no indexed entities to compare";
    }
  } else if (added_removed > 0 || different_count > 0) {
    c.verdict = verdict::kDifferent;
    c.evidence = evidence::kSummaryContradiction;
    c.detail = std::to_string(added_removed) + " added/removed, " +
               std::to_string(different_count) + " different";
  } else if (all_equivalent && c.unsupported_count == 0) {
    if (delta.identical) {
      c.verdict = verdict::kEquivalent;
      c.evidence = all_identical_source ? evidence::kIdenticalSourceAndConfig
                                        : evidence::kNormalizedIr;
      c.detail = "all entities equivalent";
    } else {
      c.verdict = verdict::kUnknown;
      c.evidence = evidence::kUnsupportedOrIncomplete;
      c.detail = std::string("configuration differs; ") + kNoDifference;
    }
  } else {
    c.verdict = verdict::kUnknown;
    c.evidence = evidence::kUnsupportedOrIncomplete;
    c.detail = kNoDifference;
  }
  if (c.verdict == verdict::kEquivalent)
    c.assumptions = {"same-standard-library", "no-undefined-behavior"};

  c.pairs = std::move(tops);
  for (EntityPair &mp : member_rows)
    c.pairs.push_back(std::move(mp));
  return c;
}

} // namespace diff
} // namespace cidx
