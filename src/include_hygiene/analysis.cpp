#include "include_hygiene/analysis.hpp"

#include "graph/query.hpp"
#include "include_hygiene/graph.hpp"
#include "storage/storage.hpp"
#include "util/hashing.hpp"
#include "util/pathutil.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace cidx::hygiene {

const char *classification_name(Classification c) {
  switch (c) {
  case Classification::Duplicate:
    return "duplicate";
  case Classification::UnusedByReference:
    return "unused_by_reference";
  case Classification::Used:
    return "used";
  case Classification::ManualReview:
    return "manual_review";
  }
  return "manual_review";
}

std::string candidate_id(const std::string &src_path, int64_t begin_offset) {
  return cidx::sha1_hex(src_path + std::string("\0", 1) +
                        std::to_string(begin_offset))
      .substr(0, 12);
}

namespace {

std::string display_name_of(const Symbol &s) {
  if (s.qual_name && !s.qual_name->empty()) {
    return *s.qual_name;
  }
  return s.spelling;
}

// The exact bytes a removal would delete, read back from disk. Used both as
// plan evidence (a reviewer sees the literal directive) and to detect a source
// file that has drifted from the index.
std::string read_range(const std::string &path, int64_t begin, int64_t end) {
  std::ifstream in(path, std::ios::binary);
  if (!in || begin < 0 || end < begin) {
    return {};
  }
  in.seekg(begin);
  std::string buf(static_cast<std::size_t>(end - begin), '\0');
  in.read(buf.data(), end - begin);
  buf.resize(static_cast<std::size_t>(in.gcount()));
  return buf;
}

// Everything one source file needs, computed once and reused for each of its
// includes: recomputing Refs(Owners(S)) per directive would re-run the whole
// closure for every #include in the file.
struct SourceContext {
  std::vector<int64_t> owners;
  std::vector<std::string> owner_names;
  std::set<int64_t> refs; // Refs(Owners(S)), symbol ids
};

SourceContext build_source_context(cidx::Storage &db, int64_t src_file_id) {
  SourceContext ctx;
  ctx.owners = db.symbol_ids_for_file(src_file_id);
  for (const int64_t id : ctx.owners) {
    if (const std::optional<Symbol> s = db.lookup_symbol_by_id(id)) {
      ctx.owner_names.push_back(display_name_of(*s));
    }
  }
  std::sort(ctx.owner_names.begin(), ctx.owner_names.end());
  ctx.owner_names.erase(
      std::unique(ctx.owner_names.begin(), ctx.owner_names.end()),
      ctx.owner_names.end());

  // Refs has three parts:
  //
  //  1. every semantic edge target out of the owners -- this carries the
  //     declaration relations (inherits, method_of, field_of, friend,
  //     specializes) that exist nowhere else;
  //  2. every def_edge target for a body defined in THIS file -- `edge` is
  //     keyed by symbol and symbol.usr is UNIQUE, so bodies sharing a USR
  //     across TUs (every `int main`) collapse to one last-writer-wins row and
  //     lose their call graph; def_edge is per-body and survives that;
  //  3. every symbol named by the types the owners mention (return, declared,
  //     underlying, and parameter types, through the structural closure),
  //     which is what makes `void f(const Foo&);` -- a signature with no body
  //     -- count as using Foo.
  //
  // Part 1 can over-approximate for a collapsed symbol (it may carry another
  // TU's edges). That direction is safe: it reports FEWER unused includes, so
  // a cleanup is missed rather than a working include deleted.
  for (const int64_t id : db.edge_targets_from(ctx.owners)) {
    ctx.refs.insert(id);
  }
  for (const int64_t id : db.def_edge_targets_for_file(src_file_id)) {
    ctx.refs.insert(id);
  }
  const std::vector<int64_t> types = db.type_ids_used_by(ctx.owners);
  for (const int64_t id : db.symbols_named_by_types(types)) {
    ctx.refs.insert(id);
  }
  return ctx;
}

// Which relation makes `target` referenced by one of `owners`, for display.
std::string relation_for(cidx::Storage &db, const std::vector<int64_t> &owners,
                         int64_t target, std::string &owner_name_out) {
  for (const int64_t o : owners) {
    for (const Storage::GraphEdgeRow &row :
         db.graph_edges(o, "out", {}, /*count_resolved=*/false, 1000)) {
      if (row.dst_id != target) {
        continue;
      }
      if (const std::optional<Symbol> s = db.lookup_symbol_by_id(o)) {
        owner_name_out = display_name_of(*s);
      }
      for (const auto &[name, id] : cidx::graph::edge_kinds_map()) {
        if (id == row.ekind) {
          return name;
        }
      }
      return "edge";
    }
  }
  // Reached through the type closure rather than a direct edge.
  if (!owners.empty()) {
    if (const std::optional<Symbol> s = db.lookup_symbol_by_id(owners.front())) {
      owner_name_out = display_name_of(*s);
    }
  }
  return "type";
}

// Decide one directive's verdict and attach its proof.
void classify(cidx::Storage &db, const IncludeGraph &graph,
              const SourceContext &ctx, const IncludeEdge &e,
              const IncludeSite &s, bool is_repeat, IncludeCandidate &c) {
  c.owners = ctx.owner_names;

  // --- caveats: reasons an automatic verdict would be unsound ---------------
  // These never change the zero-reference RESULT; they change whether it is
  // executable. Collected first so even a `used` finding carries them.
  if (!s.resolved) {
    c.caveats.push_back("directive did not resolve to a file");
  }
  if (s.directive == kIncludeDirectiveIncludeNext) {
    c.caveats.push_back("#include_next depends on search-path position");
  } else if (s.directive == kIncludeDirectiveImport) {
    c.caveats.push_back("#import has no multiple-include contract here");
  } else if (s.directive == kIncludeDirectiveIncludeMacros) {
    c.caveats.push_back("__include_macros is a preprocessor-internal directive");
  }
  if (!s.cond_fingerprint.empty()) {
    c.caveats.push_back(
        "inside a conditional region: only proven for this configuration");
  }
  if (e.is_system) {
    c.caveats.push_back("system header: its internals are not indexed");
  }
  if (!e.dst_file_id) {
    c.caveats.push_back(
        "target is not owned by any component: its symbols are not indexed");
  }

  // A macro dependency is real usage that the reference graph structurally
  // cannot see. This is the single most important caveat: without it, every
  // macro-only header looks unused.
  for (const IncludeMacroUse &m :
       db.include_macro_uses(e.src_file_id, e.dst_path)) {
    c.macro_uses.push_back(m.name);
  }
  std::sort(c.macro_uses.begin(), c.macro_uses.end());
  if (!c.macro_uses.empty()) {
    c.caveats.push_back("source expands macro(s) defined in this header");
  }

  // --- repeated UNGUARDED header: the X-macro pattern -----------------------
  // e.count is this header's occurrence count in THIS file under THIS
  // configuration. When an unguarded header is included more than once, every
  // one of its directives is load-bearing -- the file is deliberately re-read
  // to expand a redefined macro -- so none of them is ever automatic.
  //
  // This must be checked BEFORE the reference rule, and it must cover the FIRST
  // occurrence too, not just the repeats. The first include of an X-macro
  // header declares symbols nothing references and compiles fine without it, so
  // both the reference rule and the compile gate would wave it through while
  // silently deleting declarations. Guardedness, not compilation, is the only
  // sound gate here.
  if (!s.guarded && e.count > 1) {
    c.cls = Classification::ManualReview;
    c.caveats.push_back(
        "UNGUARDED header included " + std::to_string(e.count) +
        " times in this file: re-inclusion is intentional (X-macro), so no "
        "occurrence can be removed automatically");
    return;
  }

  // --- duplicate ------------------------------------------------------------
  if (is_repeat) {
    // Guarded by construction: the unguarded case returned above.
    c.cls = Classification::Duplicate;
    return;
  }

  // --- unused by reference --------------------------------------------------
  // Symbols(H) is only what H declares or defines ITSELF. A symbol from a
  // header H includes belongs to that header, not to H -- which is exactly why
  // "used through a transitive provider" does not make the direct include used.
  std::vector<int64_t> header_symbols;
  if (e.dst_file_id) {
    header_symbols = db.symbol_ids_for_file(*e.dst_file_id);
  }
  for (const int64_t id : header_symbols) {
    if (const std::optional<Symbol> sym = db.lookup_symbol_by_id(id)) {
      c.header_symbols.push_back(display_name_of(*sym));
    }
  }
  std::sort(c.header_symbols.begin(), c.header_symbols.end());
  c.header_symbols.erase(
      std::unique(c.header_symbols.begin(), c.header_symbols.end()),
      c.header_symbols.end());

  // The verdict is defined purely by the reference intersection
  // Refs(Owners(S)) ∩ Symbols(H): that is the contract, and nothing else may
  // reclassify a zero-reference finding as `used`.
  std::vector<int64_t> hit;
  for (const int64_t id : header_symbols) {
    if (ctx.refs.count(id) != 0) {
      hit.push_back(id);
    }
  }
  c.intersection_count = static_cast<int64_t>(hit.size());

  if (!hit.empty()) {
    c.cls = Classification::Used;
    for (const int64_t id : hit) {
      const std::optional<Symbol> sym = db.lookup_symbol_by_id(id);
      if (!sym) {
        continue;
      }
      Evidence ev;
      ev.target = display_name_of(*sym);
      ev.relation = relation_for(db, ctx.owners, id, ev.owner);
      c.evidence.push_back(std::move(ev));
    }
    std::sort(c.evidence.begin(), c.evidence.end(),
              [](const Evidence &a, const Evidence &b) {
                return std::tie(a.owner, a.target, a.relation) <
                       std::tie(b.owner, b.target, b.relation);
              });
    return;
  }

  // A symbol DECLARED in H and DEFINED in S is one symbol in both sets, and it
  // never references itself -- so the reference intersection is legitimately
  // empty for the most common include in C++: a .cpp including its own header.
  // That is a true `unused_by_reference` finding, but NOT an automatically
  // removable one: removing the directive usually still compiles (a definition
  // does not need its declaration), so the compile gate cannot catch it either.
  // The overlap is a safety CAVEAT, not a reference -- it never makes the
  // include `used`; it forces manual_review while the reference verdict stands.
  const std::set<int64_t> owner_set(ctx.owners.begin(), ctx.owners.end());
  for (const int64_t id : header_symbols) {
    if (owner_set.count(id) != 0) {
      c.caveats.push_back(
          "this file defines symbol(s) this header declares (its own header): "
          "removal cannot be proven safe automatically");
      break;
    }
  }

  // Zero references by the contract. Whether that is executable depends on the
  // caveats: any caveat (own header, macro use, conditional region, ...)
  // downgrades an otherwise-clean removal to manual_review.
  if (c.caveats.empty()) {
    c.cls = Classification::UnusedByReference;
  } else {
    c.cls = Classification::ManualReview;
  }

  // A header's removal has to hold for every TU that can reach it, so the plan
  // records the reverse closure the validator will have to cover.
  for (const std::string &p : graph.transitive_to(c.src_path, 0)) {
    c.reverse_dependants.push_back(p);
  }
}

// The conservative combined verdict for ONE physical directive seen under
// several configurations. Removal is only offered when every configuration
// agrees it is removable, in the same way:
//   * any `used`            -> used (the directive is doing work somewhere);
//   * else any manual_review-> manual_review (a caveat somewhere);
//   * else all duplicate    -> duplicate;
//   * else all unused       -> unused_by_reference;
//   * else (mixed removable kinds) -> manual_review (configs disagree).
Classification combine_class(Classification a, Classification b) {
  if (a == Classification::Used || b == Classification::Used) {
    return Classification::Used;
  }
  if (a == Classification::ManualReview || b == Classification::ManualReview) {
    return Classification::ManualReview;
  }
  if (a == b) {
    return a; // both Duplicate, or both UnusedByReference
  }
  return Classification::ManualReview; // one Duplicate, one UnusedByReference
}

void sort_unique(std::vector<std::string> &v) {
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
}

// Fold one per-(edge, config) candidate into the merged set keyed by physical
// directive (candidate_id == src_path + begin_offset). candidate_id identifies
// one directive; without this fold, restored per-config facts would emit a
// separate item per configuration for the very same source bytes.
void merge_candidate(std::map<std::string, IncludeCandidate> &into,
                     IncludeCandidate &&c) {
  const auto it = into.find(c.id);
  if (it == into.end()) {
    into.emplace(c.id, std::move(c));
    return;
  }
  IncludeCandidate &d = it->second;
  d.cls = combine_class(d.cls, c.cls);
  d.intersection_count = std::max(d.intersection_count, c.intersection_count);
  d.guarded = d.guarded && c.guarded; // unguarded under ANY config is unguarded
  for (auto &s : c.configs) {
    d.configs.push_back(std::move(s));
  }
  for (auto &s : c.caveats) {
    d.caveats.push_back(std::move(s));
  }
  for (auto &s : c.macro_uses) {
    d.macro_uses.push_back(std::move(s));
  }
  for (auto &s : c.owners) {
    d.owners.push_back(std::move(s));
  }
  for (auto &s : c.header_symbols) {
    d.header_symbols.push_back(std::move(s));
  }
  for (auto &s : c.reverse_dependants) {
    d.reverse_dependants.push_back(std::move(s));
  }
  for (auto &e : c.evidence) {
    d.evidence.push_back(std::move(e));
  }
}

// Deduplicate the unioned evidence/string sets of a merged candidate.
void finalize_merged(IncludeCandidate &c) {
  sort_unique(c.configs);
  sort_unique(c.caveats);
  sort_unique(c.macro_uses);
  sort_unique(c.owners);
  sort_unique(c.header_symbols);
  sort_unique(c.reverse_dependants);
  std::sort(c.evidence.begin(), c.evidence.end(),
            [](const Evidence &a, const Evidence &b) {
              return std::tie(a.owner, a.target, a.relation) <
                     std::tie(b.owner, b.target, b.relation);
            });
  c.evidence.erase(
      std::unique(c.evidence.begin(), c.evidence.end(),
                  [](const Evidence &a, const Evidence &b) {
                    return std::tie(a.owner, a.target, a.relation) ==
                           std::tie(b.owner, b.target, b.relation);
                  }),
      c.evidence.end());
}

} // namespace

AnalysisResult analyze(cidx::Storage &db, const AnalysisOptions &opts) {
  AnalysisResult result;
  if (!db.include_graph_populated()) {
    // An empty tier means the index predates v31 or has not been reindexed.
    // Reporting "no unused includes" here would be a vacuous truth, and the
    // most dangerous possible answer: it looks like a clean bill of health.
    result.include_graph_empty = true;
    return result;
  }

  // Reverse closure is only needed for headers, but building the graph once is
  // cheaper than answering "who includes H?" per candidate.
  const IncludeGraph graph = IncludeGraph::load(db, /*include_system=*/false);

  std::unordered_set<std::string> scope;
  for (const std::string &p : opts.scope_paths) {
    const std::string abs = cidx::pathutil::abspath(p);
    scope.insert(abs);
    // A requested path the tier never observed would silently contribute zero
    // findings and read as clean. Surface it instead of vanishing it.
    const std::optional<cidx::File> f = db.get_file(abs);
    if (!f || !db.include_tier_covers_file(f->id)) {
      result.uncovered_scope.push_back(abs);
    }
  }
  std::sort(result.uncovered_scope.begin(), result.uncovered_scope.end());
  result.uncovered_scope.erase(
      std::unique(result.uncovered_scope.begin(), result.uncovered_scope.end()),
      result.uncovered_scope.end());

  std::unordered_map<int64_t, std::string> paths;
  for (const auto &[row, abs] : db.list_files()) {
    paths.emplace(row.id, abs);
  }

  // Group edges by source file so each file's Refs set is built exactly once.
  std::map<int64_t, std::vector<IncludeEdge>> by_src;
  for (const IncludeEdge &e : db.all_include_edges(/*include_system=*/true)) {
    const auto p = paths.find(e.src_file_id);
    if (p == paths.end()) {
      continue;
    }
    if (!scope.empty() && scope.count(p->second) == 0) {
      continue;
    }
    by_src[e.src_file_id].push_back(e);
  }

  for (const auto &[src_id, edges] : by_src) {
    const std::string &src_path = paths.at(src_id);
    const SourceContext ctx = build_source_context(db, src_id);

    // Duplicate detection is per (target, configuration, conditional region):
    // the SAME header reached twice under the same conditions. A repeat in a
    // different #if branch is a different program, never a duplicate.
    std::set<std::tuple<std::string, int64_t, std::string>> seen_sites;

    // One physical directive can appear under several configurations (a header
    // included by many TUs). Classify each occurrence, then fold them into one
    // candidate per source-byte range so the report is not config-multiplied.
    std::map<std::string, IncludeCandidate> merged;

    for (const IncludeEdge &e : edges) {
      const std::optional<IncludeConfig> cfg =
          db.include_config_by_id(e.config_id);
      for (const IncludeSite &s : db.include_sites_for(e.id)) {
        IncludeCandidate c;
        c.id = candidate_id(src_path, s.begin_offset);
        c.src_path = src_path;
        c.dst_path = e.dst_path;
        c.spelling = s.spelling;
        c.is_angled = s.is_angled;
        c.line = s.line;
        c.col = s.col;
        c.begin_offset = s.begin_offset;
        c.end_offset = s.end_offset;
        c.cond_fingerprint = s.cond_fingerprint;
        c.guarded = s.guarded;
        c.directive_kind = s.directive;
        c.directive_text = read_range(src_path, s.begin_offset, s.end_offset);
        if (cfg) {
          c.configs = {cfg->digest};
        }

        const bool is_dup =
            !seen_sites
                 .insert({e.dst_path, e.config_id, s.cond_fingerprint})
                 .second;

        classify(db, graph, ctx, e, s, is_dup, c);
        merge_candidate(merged, std::move(c));
      }
    }

    for (auto &[id, c] : merged) {
      finalize_merged(c);
      const bool keep = (c.cls == Classification::Duplicate)
                            ? opts.want_duplicates
                            : opts.want_unused;
      if (keep || c.cls == Classification::Used) {
        result.candidates.push_back(std::move(c));
      }
    }
  }

  // Deterministic order: path, then source offset, then configuration digest.
  std::sort(result.candidates.begin(), result.candidates.end(),
            [](const IncludeCandidate &a, const IncludeCandidate &b) {
              return std::tie(a.src_path, a.begin_offset, a.configs) <
                     std::tie(b.src_path, b.begin_offset, b.configs);
            });
  return result;
}

} // namespace cidx::hygiene
