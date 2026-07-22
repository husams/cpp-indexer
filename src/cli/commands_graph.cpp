// the graph query commands and their shared selection helpers.
// Split out of commands.cpp; run_command's dispatch is unchanged.
#include "cli/commands_detail.hpp"

#include <regex>
#include <set>

namespace cidx::cli {

namespace {

// Parse a comma-separated edge-kind spec into a vector.
// Returns nullopt for null/empty string (= all kinds).
// Mirrors cli.py:_edge_kinds (cli.py:999-1004).
std::optional<std::vector<std::string>>
graph_edge_kinds(const std::optional<std::string> &spec) {
  if (!spec || spec->empty()) {
    return std::nullopt;
  }
  std::vector<std::string> out;
  std::string cur;
  for (char c : *spec) {
    if (c == ',') {
      if (!cur.empty()) {
        // strip leading/trailing spaces
        std::size_t b = 0;
        std::size_t e = cur.size();
        while (b < e && cur[b] == ' ') {
          ++b;
        }
        while (e > b && cur[e - 1] == ' ') {
          --e;
        }
        if (b < e) {
          out.push_back(cur.substr(b, e - b));
        }
      }
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) {
    std::size_t b = 0;
    std::size_t e = cur.size();
    while (b < e && cur[b] == ' ') {
      ++b;
    }
    while (e > b && cur[e - 1] == ' ') {
      --e;
    }
    if (b < e) {
      out.push_back(cur.substr(b, e - b));
    }
  }
  if (out.empty()) {
    return std::nullopt;
  }
  return out;
}

// Resolve --usr/--id/--name to a single Sym.
// Returns (nullopt, rc) on failure.
// Mirrors cli.py:_select_one (cli.py:1007-1046).
std::pair<std::optional<graph::Sym>, int>
graph_select_one(graph::GraphQuery &g,
                 const std::optional<std::string> &usr_opt,
                 const std::optional<int64_t> &id_opt,
                 const std::optional<std::string> &name_opt,
                 const std::optional<std::string> &kind_opt, bool first,
                 std::ostream &err_out) {
  if (usr_opt) {
    auto s = g.get_by_usr(*usr_opt);
    if (!s) {
      err_out << "error: no symbol with USR " << format::py_repr(*usr_opt)
              << "\n";
      return {std::nullopt, 1};
    }
    return {s, 0};
  }
  if (id_opt) {
    auto s = g.get_by_id(*id_opt);
    if (!s) {
      err_out << "error: no symbol with id " << *id_opt << "\n";
      return {std::nullopt, 1};
    }
    return {s, 0};
  }
  if (!name_opt) {
    err_out << "error: one of the arguments --usr --id --name is required\n";
    return {std::nullopt, 2};
  }
  const std::string &name = *name_opt;
  auto hits = g.find(name, kind_opt, 50);
  if (hits.empty()) {
    auto compact = [](std::string value) {
      std::erase(value, ' ');
      while (value.contains("<<")) {
        value.replace(value.find("<<"), 2, "<");
      }
      while (value.contains(">>")) {
        value.replace(value.find(">>"), 2, ">");
      }
      return value;
    };
    const std::string wanted = compact(name);
    const auto all = g.find("", kind_opt, 5000);
    for (const auto &candidate : all) {
      const std::string actual = compact(candidate.name);
      if (actual == wanted || actual.starts_with(wanted + "(")) {
        hits.push_back(candidate);
      }
    }
  }
  if (hits.empty()) {
    err_out << "error: no symbol matches --name " << format::py_repr(name);
    if (kind_opt) {
      err_out << " (kind " << *kind_opt << ")";
    }
    err_out << "\n";
    return {std::nullopt, 1};
  }
  if (hits.size() > 1 && !first) {
    err_out << "error: --name " << format::py_repr(name) << " matches "
            << hits.size()
            << " symbols; disambiguate with --usr/--id (or pass --first):\n";
    const std::size_t show = std::min(hits.size(), std::size_t{25});
    for (std::size_t j = 0; j < show; ++j) {
      const auto &s = hits[j];
      err_out << "  #" << s.id << "  " << format::ljust(s.kind, 14) << " "
              << s.name << "  @" << s.loc() << "  [" << s.usr << "]\n";
    }
    if (hits.size() > 25) {
      err_out << "  ... and " << (hits.size() - 25) << " more\n";
    }
    return {std::nullopt, 2};
  }
  return {hits[0], 0};
}

// Open graph + enforce edges. Returns (nullptr, 1) on failure.
// `storage_out` receives the opened Storage (must outlive GraphQuery).
// `require_edges = false` skips the empty-edge-table rejection: the v30
// signature/type queries read parameter/type/symbol_type facts that a valid
// declaration-only index can carry with ZERO symbol edges.
struct GraphHandle {
  std::unique_ptr<Storage> storage;
  std::unique_ptr<graph::GraphQuery> g;
};

std::optional<GraphHandle> open_graph(const ParsedArgs & /*args*/, Context &ctx,
                                      bool require_edges = true) {
  GraphHandle h;

  // Check file exists BEFORE opening Storage (Storage constructor uses
  // SQLITE_OPEN_CREATE which would create the file on disk, making a
  // subsequent stat() always succeed even for a missing index).
  {
    struct stat st{};
    if (::stat(ctx.index_path.c_str(), &st) != 0) {
      const std::string repr = format::py_repr(ctx.index_path);
      *ctx.err << "error: no cidx index at " << repr << ". Build one with:\n"
               << "    cd <repo> && cidx component add --path . && cidx import "
                  "--db <build> && cidx index && cidx resolve\n"
               << "or pass --db PATH / set $INDEXER_CACHE.\n";
      return std::nullopt;
    }
  }

  h.storage = std::make_unique<Storage>(ctx.index_path);
  h.g = std::make_unique<graph::GraphQuery>(*h.storage, ctx.index_path);
  if (require_edges && h.g->edge_count() == 0) {
    const std::string repr = format::py_repr(ctx.index_path);
    *ctx.err << "error: index " << repr
             << " has no graph edges -- it was built with "
                "`cidx index --no-graph`, or the graph was cleared. Re-run "
                "`cidx index` (without --no-graph) then `cidx resolve`.\n";
    return std::nullopt;
  }
  return h;
}

} // namespace

// ============================================================================
// M6 graph sub-command handlers
// ============================================================================

int cmd_graph_callers(const ParsedArgs &args, Context &ctx) {
  auto h = open_graph(args, ctx);
  if (!h) {
    return 1;
  }
  auto [sym, rc] = graph_select_one(*h->g, args.usr, args.graph_id, args.name,
                                    args.kind, args.first, *ctx.err);
  if (!sym) {
    return rc;
  }
  std::vector<std::string> kinds{"calls"};
  if (!args.direct_only) {
    kinds.emplace_back("dispatch_calls");
  }
  auto edges = h->g->edges_in(sym->id, kinds, args.graph_limit);
  graph::emit_edges(*h->g, edges, args.graph_json, *ctx.out,
                    "callers of " + sym->name + " (@" + sym->loc() + "):");
  return 0;
}

int cmd_graph_callees(const ParsedArgs &args, Context &ctx) {
  auto h = open_graph(args, ctx);
  if (!h) {
    return 1;
  }
  auto [sym, rc] = graph_select_one(*h->g, args.usr, args.graph_id, args.name,
                                    args.kind, args.first, *ctx.err);
  if (!sym) {
    return rc;
  }
  std::vector<std::string> kinds{"calls"};
  if (!args.direct_only) {
    kinds.emplace_back("dispatch_calls");
  }
  auto edges = h->g->edges_out(sym->id, kinds, args.graph_limit);
  graph::emit_edges(*h->g, edges, args.graph_json, *ctx.out,
                    "callees of " + sym->name + " (@" + sym->loc() + "):");
  return 0;
}

int cmd_graph_refs(const ParsedArgs &args, Context &ctx) {
  auto h = open_graph(args, ctx);
  if (!h) {
    return 1;
  }
  auto [sym, rc] = graph_select_one(*h->g, args.usr, args.graph_id, args.name,
                                    args.kind, args.first, *ctx.err);
  if (!sym) {
    return rc;
  }
  auto edges = h->g->references(sym->id, args.graph_limit);
  graph::emit_edges(*h->g, edges, args.graph_json, *ctx.out,
                    "references to " + sym->name + " (@" + sym->loc() + "):");
  return 0;
}

int cmd_graph_neighbors(const ParsedArgs &args, Context &ctx) {
  auto h = open_graph(args, ctx);
  if (!h) {
    return 1;
  }
  auto [sym, rc] = graph_select_one(*h->g, args.usr, args.graph_id, args.name,
                                    args.kind, args.first, *ctx.err);
  if (!sym) {
    return rc;
  }
  auto kinds_vec = graph_edge_kinds(args.edge);
  std::optional<std::vector<int64_t>> kid_ids;
  try {
    kid_ids = h->g->kind_ids(kinds_vec);
  } catch (const std::invalid_argument &e) {
    *ctx.err << "error: " << e.what() << "\n";
    return 1;
  }
  auto edges = h->g->edges(sym->id, args.direction, kid_ids, args.graph_limit);
  const std::string kinds_str = args.edge.value_or("all");
  graph::emit_edges(*h->g, edges, args.graph_json, *ctx.out,
                    args.direction + "-neighbors of " + sym->name + " (@" +
                        sym->loc() + ") over " + kinds_str + ":");
  return 0;
}

int cmd_graph_walk(const ParsedArgs &args, Context &ctx) {
  auto h = open_graph(args, ctx);
  if (!h) {
    return 1;
  }
  auto [sym, rc] = graph_select_one(*h->g, args.usr, args.graph_id, args.name,
                                    args.kind, args.first, *ctx.err);
  if (!sym) {
    return rc;
  }
  auto kinds_vec = graph_edge_kinds(args.edge);
  // walk default edge kind is "calls"
  if (!kinds_vec) {
    kinds_vec = std::vector<std::string>{"calls"};
  }
  graph::Traversal tr;
  try {
    tr = h->g->walk(sym->id, kinds_vec, args.direction, args.graph_depth,
                    args.graph_limit);
  } catch (const std::invalid_argument &e) {
    *ctx.err << "error: " << e.what() << "\n";
    return 1;
  }
  // Exclude the start node from output
  std::vector<graph::Sym> nodes;
  for (const auto &n : tr.nodes()) {
    if (n.id != sym->id) {
      nodes.push_back(n);
    }
  }
  // Build kinds comma-separated string for header
  std::string kinds_str;
  for (std::size_t ki = 0; ki < kinds_vec->size(); ++ki) {
    if (ki != 0) {
      kinds_str += ",";
    }
    kinds_str += (*kinds_vec)[ki];
  }
  graph::emit_syms(nodes, args.graph_json, *ctx.out,
                   "reachable from " + sym->name + " (@" + sym->loc() +
                       ") over " + kinds_str + " " + args.direction +
                       ", depth<=" + std::to_string(args.graph_depth) + ":",
                   &tr.depth_by_id);
  return 0;
}

int cmd_graph_path(const ParsedArgs &args, Context &ctx) {
  auto h = open_graph(args, ctx);
  if (!h) {
    return 1;
  }
  auto [src, rc_src] =
      graph_select_one(*h->g, args.usr, args.graph_id, args.name, args.kind,
                       args.first, *ctx.err);
  if (!src) {
    return rc_src;
  }
  auto [dst, rc_dst] =
      graph_select_one(*h->g, args.to_usr, args.to_id, args.to_name,
                       args.to_kind, args.first, *ctx.err);
  if (!dst) {
    return rc_dst;
  }

  auto kinds_vec = graph_edge_kinds(args.edge);
  // path default edge kind is "calls"
  if (!kinds_vec) {
    kinds_vec = std::vector<std::string>{"calls"};
  }
  std::optional<std::vector<graph::Sym>> chain;
  try {
    chain = h->g->reaches(src->id, dst->id, kinds_vec, args.direction,
                          args.graph_depth);
  } catch (const std::invalid_argument &e) {
    *ctx.err << "error: " << e.what() << "\n";
    return 1;
  }
  if (!chain) {
    if (args.graph_json) {
      *ctx.out << "null\n";
    } else {
      std::string ks;
      for (std::size_t ki = 0; ki < kinds_vec->size(); ++ki) {
        if (ki != 0) {
          ks += ",";
        }
        ks += (*kinds_vec)[ki];
      }
      *ctx.out << "no path from " << src->name << " to " << dst->name
               << " over " << ks << " " << args.direction << " within depth "
               << args.graph_depth << "\n";
    }
    return 1;
  }
  graph::emit_syms(*chain, args.graph_json, *ctx.out,
                   "path " + src->name + " -> " + dst->name + " (" +
                       std::to_string(chain->size() - 1) + " hop(s)):");
  return 0;
}

int cmd_graph_hierarchy(const ParsedArgs &args, Context &ctx) {
  auto h = open_graph(args, ctx);
  if (!h) {
    return 1;
  }
  auto [sym, rc] = graph_select_one(*h->g, args.usr, args.graph_id, args.name,
                                    args.kind, args.first, *ctx.err);
  if (!sym) {
    return rc;
  }
  const bool direct = !args.transitive;
  auto bases = h->g->bases(sym->id, direct);
  auto subs = h->g->subclasses(sym->id, direct);
  std::optional<std::string> access_filter;
  if (args.access != "all") {
    access_filter = args.access;
  }
  std::vector<graph::Sym> mems;
  try {
    mems = h->g->members(sym->id, access_filter);
  } catch (const std::invalid_argument &e) {
    *ctx.err << "error: " << e.what() << "\n";
    return 1;
  }
  if (args.graph_json) {
    using namespace json_out;
    Array barr;
    Array sarr;
    Array marr;
    for (const auto &s : bases) {
      barr.push_back(s.to_dict());
    }
    for (const auto &s : subs) {
      sarr.push_back(s.to_dict());
    }
    for (const auto &s : mems) {
      marr.push_back(s.to_dict());
    }
    Object o;
    o.emplace_back("symbol", sym->to_dict());
    o.emplace_back("bases", Value::arr(std::move(barr)));
    o.emplace_back("subclasses", Value::arr(std::move(sarr)));
    o.emplace_back("members", Value::arr(std::move(marr)));
    *ctx.out << dumps_indent2(Value::obj(std::move(o))) << "\n";
    return 0;
  }
  const std::string scope = args.transitive ? "all" : "direct";
  *ctx.out << "hierarchy of " << sym->name << " (@" << sym->loc() << "):\n";
  graph::emit_syms(bases, false, *ctx.out, "  bases (" + scope + "):");
  graph::emit_syms(subs, false, *ctx.out, "  subclasses (" + scope + "):");
  graph::emit_syms(mems, false, *ctx.out, "  members:");
  return 0;
}

int cmd_graph_dispatch(const ParsedArgs &args, Context &ctx) {
  auto h = open_graph(args, ctx);
  if (!h) {
    return 1;
  }
  auto [sym, rc] = graph_select_one(*h->g, args.usr, args.graph_id, args.name,
                                    args.kind, args.first, *ctx.err);
  if (!sym) {
    return rc;
  }
  auto targets = h->g->dispatch_targets(sym->id);
  const bool virt = h->g->is_virtual_method(sym->id);
  if (args.graph_json) {
    using namespace json_out;
    Array tarr;
    for (const auto &t : targets) {
      tarr.push_back(t.to_dict());
    }
    Object o;
    o.emplace_back("method", sym->to_dict());
    o.emplace_back("is_virtual", Value::of(virt));
    o.emplace_back("targets", Value::arr(std::move(tarr)));
    *ctx.out << dumps_indent2(Value::obj(std::move(o))) << "\n";
    return 0;
  }
  const std::string note =
      virt ? "" : "  (not a virtual method -- only itself)";
  graph::emit_syms(targets, false, *ctx.out,
                   "run-time dispatch targets of " + sym->name + " (@" +
                       sym->loc() + ")" + note + ":");
  return 0;
}

// v27: symbols defined in more than one backend (query.py:cmd_graph_redefined).
int cmd_graph_redefined(const ParsedArgs &args, Context &ctx) {
  auto h = open_graph(args, ctx);
  if (!h) {
    return 1;
  }
  auto syms = h->g->redefined(args.graph_limit);
  graph::emit_syms(syms, args.graph_json, *ctx.out,
                   "symbols redefined per backend (multi_def > 1):");
  return 0;
}

// v27: each backend body of a symbol + its possible-call fan-out
// (query.py:cmd_graph_definitions -- output byte-identical).
int cmd_graph_definitions(const ParsedArgs &args, Context &ctx) {
  auto h = open_graph(args, ctx);
  if (!h) {
    return 1;
  }
  auto [sym, rc] = graph_select_one(*h->g, args.usr, args.graph_id, args.name,
                                    args.kind, args.first, *ctx.err);
  if (!sym) {
    return rc;
  }
  auto defs = h->g->definitions(sym->id);
  std::vector<graph::Definition> possible;
  if (!args.direct_only) {
    possible = h->g->possible_callees(sym->id);
  }
  if (args.graph_json) {
    using namespace json_out;
    Array darr;
    for (const auto &d : defs) {
      darr.push_back(d.to_dict());
    }
    Array parr;
    for (const auto &d : possible) {
      parr.push_back(d.to_dict());
    }
    Object o;
    o.emplace_back("symbol", sym->to_dict());
    o.emplace_back("multi_def", Value::of(sym->multi_def));
    o.emplace_back("definitions", Value::arr(std::move(darr)));
    o.emplace_back("possible_callees", Value::arr(std::move(parr)));
    *ctx.out << dumps_indent2(Value::obj(std::move(o))) << "\n";
    return 0;
  }
  *ctx.out << "definitions of " << sym->name << " (" << defs.size()
           << " backend body/bodies):\n";
  for (const auto &d : defs) {
    *ctx.out << "  " << fmt::ljust(d.sym.kind, 14) << " @" << d.loc()
             << "  component=" << (d.component ? *d.component : "None") << "\n";
  }
  if (!possible.empty()) {
    *ctx.out << "possible-call targets from " << sym->name << ":\n";
    std::size_t w = 0;
    for (const auto &d : possible) {
      const std::string &nm = d.sym.name.empty() ? d.sym.usr : d.sym.name;
      w = std::max(nm.size(), w);
    }
    for (const auto &d : possible) {
      const std::string &nm = d.sym.name.empty() ? d.sym.usr : d.sym.name;
      *ctx.out << "  " << fmt::ljust(nm, static_cast<int>(w)) << "  @"
               << d.loc() << "\n";
    }
  }
  return 0;
}

// v30: signature/type facts of one symbol (returns/params for callables,
// of_type for variables/fields, underlying for typedef/alias symbols).
int cmd_graph_signature(const ParsedArgs &args, Context &ctx) {
  auto h = open_graph(args, ctx, /*require_edges=*/false);
  if (!h) {
    return 1;
  }
  auto [sym, rc] = graph_select_one(*h->g, args.usr, args.graph_id, args.name,
                                    args.kind, args.first, *ctx.err);
  if (!sym) {
    return rc;
  }
  const graph::GraphQuery::SignatureInfo sig = h->g->signature(sym->id);
  if (args.graph_json) {
    using namespace json_out;
    const auto type_dict =
        [](const std::optional<graph::GraphQuery::TypeInfo> &t) {
          if (!t) {
            return Value::null();
          }
          Object o;
          o.emplace_back("id", Value::of(t->id));
          o.emplace_back("spelling", Value::of(t->spelling));
          o.emplace_back("kind", Value::of(t->kind));
          o.emplace_back("canonical", t->canonical ? Value::of(*t->canonical)
                                                   : Value::null());
          return Value::obj(std::move(o));
        };
    Object o;
    o.emplace_back("symbol", sym->to_dict());
    o.emplace_back("returns", type_dict(sig.returns));
    Array parr;
    for (const auto &p : sig.params) {
      Object po;
      po.emplace_back("position", Value::of(p.position));
      po.emplace_back("name", p.name ? Value::of(*p.name) : Value::null());
      po.emplace_back("type", type_dict(p.type));
      parr.push_back(Value::obj(std::move(po)));
    }
    o.emplace_back("params", Value::arr(std::move(parr)));
    o.emplace_back("of_type", type_dict(sig.of_type));
    o.emplace_back("underlying_type", type_dict(sig.underlying));
    *ctx.out << dumps_indent2(Value::obj(std::move(o))) << "\n";
    return 0;
  }
  *ctx.out << "signature of " << sym->name << " (@" << sym->loc() << "):\n";
  const auto named_decl = [&h](const graph::GraphQuery::TypeInfo &initial) {
    auto t = initial;
    std::set<int64_t> seen;
    while (seen.insert(t.id).second) {
      if (t.decl_usr) {
        if (const auto d = h->g->get_by_usr(*t.decl_usr)) {
          return d->name;
        }
      }
      const int64_t edge =
          t.kind == "alias" ? 3
          : (t.kind == "pointer" || t.kind == "array" ||
             t.kind == "lvalue-reference" || t.kind == "rvalue-reference")
              ? (t.kind == "array" ? 2 : 1)
              : 0;
      if (edge == 0) {
        break;
      }
      const auto child = h->g->type_child(t.id, edge);
      if (!child) {
        break;
      }
      t = *child;
    }
    return std::string{};
  };
  const auto facts = [&h,
                      &named_decl](const graph::GraphQuery::TypeInfo &declared,
                                   const graph::GraphQuery::TypeInfo &adjusted,
                                   bool forwarding) {
    std::string mode = "value";
    graph::GraphQuery::TypeInfo base = adjusted;
    if (declared.kind == "lvalue-reference" ||
        declared.kind == "rvalue-reference") {
      mode = declared.kind;
      if (const auto child = h->g->type_child(declared.id, 1)) {
        base = *child;
      }
    }
    std::string kind = base.kind;
    if (base.spelling.ends_with("...")) {
      kind = "pack-expansion";
    }
    std::string out = mode + ", " + kind;
    if (forwarding) {
      out += ", forwarding";
    }
    const std::string name = named_decl(base);
    return std::pair{out, name};
  };
  const auto type_line = [&h,
                          &named_decl](const graph::GraphQuery::TypeInfo &t) {
    std::string out = t.spelling;
    const std::string name = named_decl(t);
    bool qualify = !name.empty() && t.kind != "alias";
    if (qualify && t.kind == "pointer") {
      if (const auto child = h->g->type_child(t.id, 1);
          child && child->kind == "alias") {
        qualify = false;
      }
    }
    if (qualify) {
      const auto sep = name.rfind("::");
      const std::string leaf =
          name.substr(sep == std::string::npos ? 0 : sep + 2);
      const auto pos = out.find(leaf);
      if (pos != std::string::npos &&
          (pos < 2 || out.substr(pos - 2, 2) != "::")) {
        out.replace(pos, leaf.size(), name);
      }
    }
    out = std::regex_replace(out, std::regex(R"(\[\])"), " []");
    out = std::regex_replace(out, std::regex(R"(\*const)"), "* const");
    return out;
  };
  if (sig.empty()) {
    *ctx.out << "  (no signature/type facts)\n";
    return 0;
  }
  if (sig.returns) {
    const auto [facts_text, name] = facts(*sig.returns, *sig.returns, false);
    *ctx.out << "  return: " << type_line(*sig.returns) << " [" << facts_text
             << "]";
    if (!name.empty()) {
      *ctx.out << " -> " << name;
    }
    *ctx.out << "\n";
  }
  for (const auto &p : sig.params) {
    if (!p.declared_type || !p.adjusted_type) {
      continue;
    }
    const auto [facts_text, name] =
        facts(*p.declared_type, *p.adjusted_type,
              p.reference_semantics && *p.reference_semantics == "forwarding");
    *ctx.out << "  param " << p.position << ": " << (p.name ? *p.name : "_")
             << ": " << type_line(*p.declared_type);
    if (p.declared_type->spelling != p.adjusted_type->spelling) {
      *ctx.out << " => " << type_line(*p.adjusted_type);
    }
    *ctx.out << " [" << facts_text << "]";
    if (!name.empty()) {
      *ctx.out << " -> " << name;
    }
    if (p.default_text) {
      *ctx.out << " = " << *p.default_text;
    }
    if (p.default_origin) {
      *ctx.out << " [from " << *p.default_origin << "]";
    }
    *ctx.out << "\n";
  }
  if (sig.of_type) {
    *ctx.out << "  type: " << sig.of_type->spelling << "\n";
  }
  if (sig.underlying) {
    *ctx.out << "  underlying: " << sig.underlying->spelling << "\n";
  }
  return 0;
}

int cmd_graph_template(const ParsedArgs &args, Context &ctx) {
  auto h = open_graph(args, ctx, /*require_edges=*/false);
  if (!h) {
    return 1;
  }
  auto [sym, rc] = graph_select_one(*h->g, args.usr, args.graph_id, args.name,
                                    args.kind, args.first, *ctx.err);
  if (!sym) {
    return rc;
  }
  auto relationships = h->g->edges_out(
      sym->id, std::vector<std::string>{"instantiates", "specializes"}, 20);
  for (const auto &e : relationships) {
    *ctx.out << e.kind << ": " << e.peer.name << "\n";
  }
  auto st = h->storage->raw_db().prepare(
      "SELECT position, arg_kind, literal, ref_id, type_id FROM template_arg "
      "WHERE owner_id=? ORDER BY position, pack_index");
  st.bind(1, sym->id);
  while (st.step()) {
    const int kind = static_cast<int>(st.col_int64(1));
    const std::string literal = st.col_text(2);
    const std::string label = kind == 1   ? "type"
                              : kind == 2 ? "non-type"
                                          : "template";
    *ctx.out << "arg " << st.col_int64(0) << ": " << label << ": " << literal;
    if (!st.col_is_null(4)) {
      if (const auto t = h->storage->type_node_by_id(st.col_int64(4))) {
        *ctx.out << (kind == 1 ? " -> " : ": ") << t->spelling;
      }
    }
    *ctx.out << "\n";
  }
  return 0;
}

// v30: callables accepting/returning the type + variables/fields/aliases of
// it, through pointer/reference/array/alias/template-argument layers.
int cmd_graph_typeusers(const ParsedArgs &args, Context &ctx) {
  auto h = open_graph(args, ctx, /*require_edges=*/false);
  if (!h) {
    return 1;
  }
  auto [sym, rc] = graph_select_one(*h->g, args.usr, args.graph_id, args.name,
                                    args.kind, args.first, *ctx.err);
  if (!sym) {
    return rc;
  }
  const auto users = h->g->type_users(sym->usr, args.graph_limit);
  if (args.graph_json) {
    using namespace json_out;
    Array uarr;
    for (const auto &u : users) {
      Value v = u.sym.to_dict();
      v.o.emplace_back("role", Value::of(u.role));
      v.o.emplace_back("position",
                       u.position ? Value::of(*u.position) : Value::null());
      uarr.push_back(std::move(v));
    }
    Object o;
    o.emplace_back("symbol", sym->to_dict());
    o.emplace_back("users", Value::arr(std::move(uarr)));
    *ctx.out << dumps_indent2(Value::obj(std::move(o))) << "\n";
    return 0;
  }
  *ctx.out << "users of type " << sym->name << " (@" << sym->loc() << "):\n";
  std::size_t width = 0;
  for (const auto &u : users) {
    const std::string &nm = u.sym.name.empty() ? u.sym.usr : u.sym.name;
    width = std::max(nm.size(), width);
  }
  for (const auto &u : users) {
    const std::string &nm = u.sym.name.empty() ? u.sym.usr : u.sym.name;
    std::string role = u.role;
    if (u.position) {
      role += " " + std::to_string(*u.position);
    }
    *ctx.out << "  " << fmt::ljust(u.sym.kind, 14) << " "
             << fmt::ljust(nm, static_cast<int>(width)) << "  " << role << "  @"
             << u.sym.loc() << "\n";
  }
  *ctx.out << users.size() << " result(s)\n";
  return 0;
}

// ---------------------------------------------------------------------------
// Portable-paths commands (v14): component show/set-version
// ---------------------------------------------------------------------------

} // namespace cidx::cli
