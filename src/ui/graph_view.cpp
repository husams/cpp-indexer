#include "ui/graph_view.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <map>
#include <set>
#include <string_view>
#include <utility>

#include "graph/query.hpp"
#include "graph/records.hpp"
#include "query/exec.hpp"
#include "query/plan.hpp"
#include "util/errors.hpp"
#include "util/pathutil.hpp"

namespace cidx::ui {
namespace {

using json_out::Array;
using json_out::Object;
using json_out::Value;

constexpr int kGraphViewVersion = 1;

Value optional_string(const std::optional<std::string> &value) {
  return value ? Value::of(*value) : Value::null();
}

Value optional_int(const std::optional<int64_t> &value) {
  return value ? Value::of(*value) : Value::null();
}

std::string redacted_path(const std::string &path,
                          const std::optional<std::string> &workspace) {
  if (workspace && !workspace->empty()) {
    const std::string prefix = *workspace + "/";
    if (path.starts_with(prefix)) {
      return path.substr(prefix.size());
    }
  }
  return pathutil::basename(path);
}

// render_html() script-escapes these bytes from one byte to six. Keep the
// GraphView budget honest for both the raw JSON and the static-export form.
std::size_t script_safe_size(std::string_view json) {
  std::size_t size = json.size();
  for (const char byte : json) {
    if (byte == '<' || byte == '>' || byte == '&') {
      size += 5;
    }
  }
  return size;
}

std::string location(const graph::Sym &sym,
                     const std::optional<std::string> &workspace) {
  if (!sym.file) {
    return "<no-location>";
  }
  std::string out = redacted_path(*sym.file, workspace);
  if (sym.line) {
    out += ":" + std::to_string(*sym.line);
    if (sym.col) {
      out += ":" + std::to_string(*sym.col);
    }
  }
  return out;
}

struct PortableReference {
  std::string semantic_universe;
  std::string identity_key;
  std::string usr;
};

std::string length_field(std::string_view value) {
  return std::to_string(value.size()) + ":" + std::string(value);
}

std::string hex_field(std::string_view value) {
  constexpr std::string_view hex = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(value.size() * 2);
  for (const unsigned char byte : value) {
    encoded.push_back(hex[byte >> 4]);
    encoded.push_back(hex[byte & 0x0f]);
  }
  return length_field(encoded);
}

// The semantic universe plus identity key is portable across database
// rebuilds. Length-prefixed fields make the wire form reversible even when
// either value contains the delimiters used elsewhere in an identity key.
std::string portable_id(const graph::Sym &sym) {
  return "symbol:v1:" + hex_field(sym.semantic_universe) +
         hex_field(sym.identity_key) + hex_field(sym.usr);
}

std::string portable_edge_id(const graph::Sym &source, const graph::Edge &edge,
                             const graph::Sym &target) {
  return "edge:v1:" + length_field(portable_id(source)) +
         length_field(edge.kind) + length_field(portable_id(target));
}

std::optional<std::string_view> length_value(std::string_view encoded,
                                             std::size_t &offset) {
  const std::size_t separator = encoded.find(':', offset);
  if (separator == std::string_view::npos || separator == offset) {
    return std::nullopt;
  }
  std::size_t length = 0;
  const auto *begin = encoded.data() + offset;
  const auto *end = encoded.data() + separator;
  const auto parsed = std::from_chars(begin, end, length);
  if (parsed.ec != std::errc{} || parsed.ptr != end) {
    return std::nullopt;
  }
  const std::size_t value_start = separator + 1;
  if (length > encoded.size() - value_start) {
    return std::nullopt;
  }
  offset = value_start + length;
  return encoded.substr(value_start, length);
}

std::optional<std::string> decode_hex(std::string_view encoded) {
  if (encoded.size() % 2 != 0) {
    return std::nullopt;
  }
  std::string decoded;
  decoded.reserve(encoded.size() / 2);
  for (std::size_t i = 0; i < encoded.size(); i += 2) {
    const auto digit = [](char value) -> std::optional<unsigned char> {
      if (value >= '0' && value <= '9') {
        return static_cast<unsigned char>(value - '0');
      }
      if (value >= 'a' && value <= 'f') {
        return static_cast<unsigned char>(value - 'a' + 10);
      }
      if (value >= 'A' && value <= 'F') {
        return static_cast<unsigned char>(value - 'A' + 10);
      }
      return std::nullopt;
    };
    const auto high = digit(encoded[i]);
    const auto low = digit(encoded[i + 1]);
    if (!high || !low) {
      return std::nullopt;
    }
    decoded.push_back(static_cast<char>((*high << 4) | *low));
  }
  return decoded;
}

std::optional<PortableReference> parse_portable_id(std::string_view value) {
  constexpr std::string_view prefix = "symbol:v1:";
  if (!value.starts_with(prefix)) {
    return std::nullopt;
  }
  std::size_t offset = prefix.size();
  const auto universe_encoded = length_value(value, offset);
  const auto identity_key_encoded = length_value(value, offset);
  const auto usr_encoded = length_value(value, offset);
  if (!universe_encoded || !identity_key_encoded || !usr_encoded ||
      offset != value.size()) {
    return std::nullopt;
  }
  const auto universe = decode_hex(*universe_encoded);
  const auto identity_key = decode_hex(*identity_key_encoded);
  const auto usr = decode_hex(*usr_encoded);
  if (!universe || !identity_key || !usr) {
    return std::nullopt;
  }
  return PortableReference{.semantic_universe = *universe,
                           .identity_key = *identity_key,
                           .usr = *usr};
}

std::string symbol_color(const graph::Sym &sym) {
  if (sym.external) {
    return "#f37777";
  }
  return sym.resolved ? "#65d6c3" : "#f0b35e";
}

Value status(const graph::Sym &sym, const std::string &freshness,
             bool truncated) {
  Object out;
  out.emplace_back("completeness", Value::of(std::string(
                                       sym.resolved ? "complete" : "partial")));
  out.emplace_back("freshness", Value::of(freshness));
  out.emplace_back("resolved", Value::of(sym.resolved));
  out.emplace_back("external", Value::of(sym.external));
  out.emplace_back("stub", Value::of(sym.is_stub()));
  out.emplace_back("truncated", Value::of(truncated));
  out.emplace_back("inferred", Value::of(false));
  out.emplace_back("assumed", Value::of(false));
  out.emplace_back("refuted", Value::of(false));
  out.emplace_back("proved", Value::of(false));
  return Value::obj(std::move(out));
}

Value node_value(const graph::Sym &sym, const std::string &freshness,
                 bool truncated, int depth,
                 const std::optional<std::string> &workspace) {
  Object out;
  out.emplace_back("id", Value::of(portable_id(sym)));
  out.emplace_back("usr", Value::of(sym.usr));
  out.emplace_back("semantic_universe", Value::of(sym.semantic_universe));
  out.emplace_back("identity_key", Value::of(sym.identity_key));
  out.emplace_back("name", Value::of(sym.name.empty() ? sym.usr : sym.name));
  out.emplace_back("kind", Value::of(sym.kind));
  out.emplace_back("location", Value::of(location(sym, workspace)));
  out.emplace_back("file", sym.file
                               ? Value::of(redacted_path(*sym.file, workspace))
                               : Value::null());
  out.emplace_back("line", optional_int(sym.line));
  out.emplace_back("col", optional_int(sym.col));
  out.emplace_back("depth", Value::of(depth));
  out.emplace_back("status", status(sym, freshness, truncated));
  out.emplace_back("color", Value::of(symbol_color(sym)));
  out.emplace_back("border", Value::of(symbol_color(sym)));
  out.emplace_back("evidence", [&] {
    Object evidence;
    evidence.emplace_back("location", Value::of(location(sym, workspace)));
    evidence.emplace_back("bounded", Value::of(true));
    return Value::obj(std::move(evidence));
  }());
  return Value::obj(std::move(out));
}

Value site_value(const graph::Site &site,
                 const std::optional<std::string> &workspace) {
  Object out;
  out.emplace_back("file", site.file
                               ? Value::of(redacted_path(*site.file, workspace))
                               : Value::null());
  out.emplace_back("line", optional_int(site.line));
  out.emplace_back("col", optional_int(site.col));
  out.emplace_back("conditional", Value::of(site.conditional));
  out.emplace_back("args_sig", optional_string(site.args_sig));
  std::string loc =
      site.file ? redacted_path(*site.file, workspace) : "<no-location>";
  if (site.line) {
    loc += ":" + std::to_string(*site.line);
    if (site.col) {
      loc += ":" + std::to_string(*site.col);
    }
  }
  out.emplace_back("location", Value::of(std::move(loc)));
  return Value::obj(std::move(out));
}

std::string site_sort_key(const graph::Site &site,
                          const std::optional<std::string> &workspace) {
  return (site.file ? redacted_path(*site.file, workspace) : "") + "\x1f" +
         std::to_string(site.line.value_or(0)) + "\x1f" +
         std::to_string(site.col.value_or(0)) + "\x1f" +
         site.args_sig.value_or("");
}

Value edge_value(const graph::Edge &edge, const graph::Sym &source,
                 const graph::Sym &target, const std::string &freshness,
                 bool truncated, bool sites_truncated,
                 const std::vector<graph::Site> &sites,
                 const std::optional<std::string> &workspace) {
  Object out;
  out.emplace_back("id", Value::of(portable_edge_id(source, edge, target)));
  out.emplace_back("source", Value::of(portable_id(source)));
  out.emplace_back("target", Value::of(portable_id(target)));
  out.emplace_back("kind", Value::of(edge.kind));
  out.emplace_back("count", Value::of(edge.count));
  out.emplace_back("status", [&] {
    Object s;
    s.emplace_back("completeness", Value::of(std::string("complete")));
    s.emplace_back("freshness", Value::of(freshness));
    s.emplace_back("truncated", Value::of(truncated));
    s.emplace_back("evidence_truncated", Value::of(sites_truncated));
    s.emplace_back("external", Value::of(edge.peer.external));
    return Value::obj(std::move(s));
  }());
  Array site_values;
  site_values.reserve(sites.size());
  for (const auto &site : sites) {
    site_values.push_back(site_value(site, workspace));
  }
  out.emplace_back("sites", Value::arr(std::move(site_values)));
  out.emplace_back("evidence", [&] {
    Object evidence;
    evidence.emplace_back("bounded", Value::of(true));
    evidence.emplace_back("sites_truncated", Value::of(sites_truncated));
    return Value::obj(std::move(evidence));
  }());
  out.emplace_back("color", Value::of(std::string(
                                edge.peer.external ? "#f37777" : "#7891aa")));
  return Value::obj(std::move(out));
}

struct RootResolution {
  std::optional<graph::Sym> symbol;
  std::string status = "none";
  std::vector<graph::Sym> candidates;
};

query::Query portable_lookup_plan(const PortableReference &reference) {
  std::vector<query::Pred> predicates{
      query::eq("semantic_universe", reference.semantic_universe)};
  if (!reference.identity_key.empty()) {
    predicates.push_back(query::eq("identity_key", reference.identity_key));
  } else {
    predicates.push_back(query::eq("usr", reference.usr));
  }
  return query::start(query::codebase()) |
         query::nodes(query::all_of(std::move(predicates))) | query::limit(2);
}

std::vector<graph::Sym> portable_matches(Storage &db, graph::GraphQuery &graph,
                                         const PortableReference &reference) {
  query::SqliteQueryReadAdapter read(db);
  const query::Result result =
      query::Executor(read).run(portable_lookup_plan(reference).plan());
  std::vector<graph::Sym> matches;
  for (const auto &row : result.rows) {
    if (row.empty() || !std::holds_alternative<int64_t>(row.front())) {
      continue;
    }
    if (const auto symbol = graph.get_by_id(std::get<int64_t>(row.front()))) {
      matches.push_back(*symbol);
    }
  }
  return matches;
}

RootResolution resolve_root(graph::GraphQuery &graph, Storage &db,
                            const std::optional<std::string> &root) {
  RootResolution result;
  if (!root || root->empty()) {
    return result;
  }
  if (std::ranges::all_of(
          *root, [](unsigned char c) { return std::isdigit(c) != 0; })) {
    try {
      result.symbol = graph.get_by_id(std::stoll(*root));
    } catch (const std::exception &) {
      result.symbol = std::nullopt;
    }
    result.status = result.symbol ? "exact_local_id" : "unknown";
    return result;
  }

  if (const auto reference = parse_portable_id(*root)) {
    result.candidates = portable_matches(db, graph, *reference);
    if (result.candidates.size() == 1) {
      result.symbol = result.candidates.front();
      result.status = "exact_portable";
    } else if (result.candidates.size() > 1) {
      result.status = "ambiguous";
    } else {
      result.status = "unknown";
    }
    return result;
  }

  const auto usr_matches = db.lookup_symbols_by_usr(*root);
  for (const auto &match : usr_matches) {
    if (const auto symbol = graph.get_by_id(match.id)) {
      result.candidates.push_back(*symbol);
    }
  }
  if (result.candidates.size() == 1) {
    result.symbol = result.candidates.front();
    result.status = "exact_usr";
    return result;
  }
  if (result.candidates.size() > 1) {
    result.status = "ambiguous";
    return result;
  }

  result.candidates = graph.find(*root, std::nullopt, 32);
  if (result.candidates.size() == 1) {
    result.symbol = result.candidates.front();
    result.status = "unique_name";
  } else if (result.candidates.empty()) {
    result.status = "unknown";
  } else {
    result.status = "ambiguous";
  }
  return result;
}

void add_root_candidate_metadata(Object &metadata,
                                 const RootResolution &resolution) {
  Object root;
  root.emplace_back("status", Value::of(resolution.status));
  Array candidates;
  std::vector<graph::Sym> ordered = resolution.candidates;
  std::ranges::sort(ordered, [](const graph::Sym &a, const graph::Sym &b) {
    return portable_id(a) < portable_id(b);
  });
  if (ordered.size() > 10) {
    ordered.resize(10);
  }
  for (const auto &candidate : ordered) {
    Object item;
    item.emplace_back("id", Value::of(portable_id(candidate)));
    item.emplace_back("usr", Value::of(candidate.usr));
    item.emplace_back("name", Value::of(candidate.name));
    item.emplace_back("kind", Value::of(candidate.kind));
    candidates.push_back(Value::obj(std::move(item)));
  }
  root.emplace_back("candidates", Value::arr(std::move(candidates)));
  metadata.emplace_back("root_resolution", Value::obj(std::move(root)));
}

query::Query root_seed(const graph::Sym &root) {
  if (root.identity_key.empty()) {
    return query::start(query::symbol(root.usr));
  }
  return query::start(query::codebase()) |
         query::nodes(query::all_of(
             {query::eq("semantic_universe", root.semantic_universe),
              query::eq("identity_key", root.identity_key)}));
}

query::Query make_query_plan(const graph::Sym &root,
                             const GraphViewRequest &request, int node_budget) {
  query::Query seed = root_seed(root);
  if (request.depth <= 0) {
    return seed | query::limit(node_budget + 1);
  }

  std::vector<std::string> relations;
  if (request.edge_kinds && !request.edge_kinds->empty()) {
    relations = *request.edge_kinds;
  } else {
    for (const auto &relation : query::relation_catalog()) {
      const bool symbol_result =
          request.direction == "in"
              ? relation.layer == query::View::Symbol
              : relation.target_view == query::View::Symbol;
      if (relation.layer == query::View::Symbol && symbol_result) {
        relations.push_back(relation.name);
      }
    }
  }
  if (relations.empty()) {
    return seed | query::limit(node_budget + 1);
  }

  query::Query combined = seed;
  for (const auto &relation : relations) {
    const query::Query branch =
        request.direction == "in"
            ? seed | query::in_(relation, 1, request.depth)
            : seed | query::out(relation, 1, request.depth);
    combined = combined | query::union_(branch);
  }
  return combined | query::limit(node_budget + 1);
}

Value *member(Value &value, std::string_view key) {
  for (auto &[name, child] : value.o) {
    if (name == key) {
      return &child;
    }
  }
  return nullptr;
}

const Value *member(const Value &value, std::string_view key) {
  for (const auto &[name, child] : value.o) {
    if (name == key) {
      return &child;
    }
  }
  return nullptr;
}

void set_bool_member(Value &value, std::string_view key, bool enabled) {
  if (Value *child = member(value, key)) {
    *child = Value::of(enabled);
  }
}

void set_int_member(Value &value, std::string_view key, int64_t number) {
  if (Value *child = member(value, key)) {
    *child = Value::of(number);
  }
}

} // namespace

Value build_graph_view(Storage &db, const GraphViewRequest &request) {
  graph::GraphQuery graph(db, "<ui>");
  const IndexIdentity identity = db.index_identity();
  const std::string freshness = identity.freshness;
  const int node_budget = std::clamp(request.node_budget, 1, 10000);
  const int edge_budget = std::clamp(request.edge_budget, 1, 20000);
  const int site_budget = std::clamp(request.site_budget, 0, 20000);
  const int byte_budget =
      std::clamp(request.byte_budget, 1024, 64 * 1024 * 1024);

  if (request.query &&
      request.query->find_first_of(" |()[]{}") != std::string::npos) {
    throw CidxError(
        "cidx ui: --query accepts one portable symbol reference; textual CXQ "
        "operators are not supported by this GraphView surface");
  }
  if (request.root && request.query && *request.root != *request.query) {
    throw CidxError(
        "cidx ui: --root and --query must identify the same symbol");
  }
  const std::optional<std::string> effective_root =
      request.root ? request.root : request.query;
  const RootResolution resolution = resolve_root(graph, db, effective_root);

  Array nodes;
  Array edges;
  bool truncated = false;
  bool evidence_truncated = false;
  int sites_used = 0;
  Object metadata;
  metadata.emplace_back("contract",
                        Value::of(std::string("cidx.graph-view.v1")));
  metadata.emplace_back("version", Value::of(kGraphViewVersion));
  metadata.emplace_back("freshness", Value::of(freshness));
  metadata.emplace_back("graph_resolved", Value::of(db.graph_resolved()));
  metadata.emplace_back("node_budget", Value::of(node_budget));
  metadata.emplace_back("edge_budget", Value::of(edge_budget));
  metadata.emplace_back("site_budget", Value::of(site_budget));
  metadata.emplace_back("byte_budget", Value::of(byte_budget));
  metadata.emplace_back("sites_used", Value::of(sites_used));
  metadata.emplace_back("depth", Value::of(request.depth));
  metadata.emplace_back("direction", Value::of(request.direction));
  metadata.emplace_back("query", optional_string(request.query));
  metadata.emplace_back("workspace",
                        request.workspace
                            ? Value::of(pathutil::basename(*request.workspace))
                            : Value::null());
  add_root_candidate_metadata(metadata, resolution);

  Object index;
  index.emplace_back("schema_version", Value::of(identity.schema_version));
  index.emplace_back("source_revision",
                     optional_string(identity.source_revision));
  index.emplace_back("source_fingerprint",
                     optional_string(identity.source_fingerprint));
  index.emplace_back("index_config", optional_string(identity.index_config));
  index.emplace_back("index_config_fingerprint",
                     optional_string(identity.index_config_fingerprint));
  index.emplace_back("freshness", Value::of(identity.freshness));
  metadata.emplace_back("index", Value::obj(std::move(index)));

  if (resolution.symbol) {
    const query::Query plan =
        make_query_plan(*resolution.symbol, request, node_budget);
    query::SqliteQueryReadAdapter read(db);
    const query::Result result = query::Executor(read).run(plan.plan());
    metadata.emplace_back("query_plan",
                          Value::of(query::canonical_json(plan.plan())));
    truncated = result.truncated ||
                result.rows.size() > static_cast<std::size_t>(node_budget);

    std::map<int64_t, graph::Sym> symbols_by_id;
    for (const auto &row : result.rows) {
      if (row.empty() || !std::holds_alternative<int64_t>(row.front())) {
        continue;
      }
      const auto symbol = graph.get_by_id(std::get<int64_t>(row.front()));
      if (symbol) {
        symbols_by_id.emplace(symbol->id, *symbol);
      }
    }
    std::vector<graph::Sym> ordered_nodes;
    ordered_nodes.reserve(symbols_by_id.size());
    for (const auto &[id, symbol] : symbols_by_id) {
      (void)id;
      ordered_nodes.push_back(symbol);
    }
    std::ranges::sort(ordered_nodes,
                      [](const graph::Sym &a, const graph::Sym &b) {
                        return portable_id(a) < portable_id(b);
                      });
    if (ordered_nodes.size() > static_cast<std::size_t>(node_budget)) {
      ordered_nodes.resize(node_budget);
      truncated = true;
    }
    std::set<int64_t> selected_ids;
    std::map<int64_t, int> depths;
    for (const auto &symbol : ordered_nodes) {
      selected_ids.insert(symbol.id);
      depths.emplace(symbol.id, symbol.id == resolution.symbol->id ? 0 : 1);
      nodes.push_back(node_value(symbol, freshness, truncated,
                                 depths[symbol.id], request.workspace));
    }

    std::map<std::string, graph::Edge> by_key;
    const auto kind_ids = graph::GraphQuery::kind_ids(request.edge_kinds);
    for (const auto &symbol : ordered_nodes) {
      const auto adjacent = graph.edges(symbol.id, request.direction, kind_ids,
                                        edge_budget + 1, false);
      for (const auto &edge : adjacent) {
        if (!selected_ids.contains(edge.src_id) ||
            !selected_ids.contains(edge.dst_id)) {
          continue;
        }
        const auto source = symbols_by_id.find(edge.src_id);
        const auto target = symbols_by_id.find(edge.dst_id);
        if (source == symbols_by_id.end() || target == symbols_by_id.end()) {
          continue;
        }
        by_key.emplace(portable_edge_id(source->second, edge, target->second),
                       edge);
      }
    }
    if (by_key.size() > static_cast<std::size_t>(edge_budget)) {
      truncated = true;
    }
    int emitted = 0;
    int sites_remaining = site_budget;
    for (const auto &[key, edge] : by_key) {
      (void)key;
      if (emitted++ == edge_budget) {
        break;
      }
      const auto source = symbols_by_id.find(edge.src_id);
      const auto target = symbols_by_id.find(edge.dst_id);
      if (source == symbols_by_id.end() || target == symbols_by_id.end()) {
        continue;
      }
      const int fetch_limit = sites_remaining + 1;
      auto sites = graph.sites(edge.edge_id, fetch_limit);
      std::ranges::sort(sites, [&](const graph::Site &a, const graph::Site &b) {
        return site_sort_key(a, request.workspace) <
               site_sort_key(b, request.workspace);
      });
      bool sites_truncated =
          sites.size() > static_cast<std::size_t>(sites_remaining);
      if (sites_truncated) {
        sites.resize(static_cast<std::size_t>(sites_remaining));
      }
      sites_used += static_cast<int>(sites.size());
      sites_remaining -= static_cast<int>(sites.size());
      evidence_truncated = evidence_truncated || sites_truncated;
      edges.push_back(edge_value(edge, source->second, target->second,
                                 freshness, truncated, sites_truncated, sites,
                                 request.workspace));
    }
  } else {
    metadata.emplace_back("query_plan", Value::null());
    metadata.emplace_back(
        "empty_reason",
        Value::of(std::string(
            resolution.status == "none"
                ? "a bounded --root is required when --query is absent"
                : "root is unknown or ambiguous")));
  }

  if (evidence_truncated) {
    truncated = true;
  }
  for (auto &[name, value] : metadata) {
    if (name == "sites_used") {
      value = Value::of(sites_used);
    }
  }
  metadata.emplace_back("truncated", Value::of(truncated));
  metadata.emplace_back("evidence_truncated", Value::of(evidence_truncated));
  metadata.emplace_back("continuation", [&] {
    Object continuation;
    continuation.emplace_back("available", Value::of(truncated));
    continuation.emplace_back(
        "reason", Value::of(std::string(truncated ? "budget" : "complete")));
    return Value::obj(std::move(continuation));
  }());

  Object out;
  out.emplace_back("schema", Value::of(std::string("cidx.graph-view.v1")));
  out.emplace_back("request", [&] {
    Object r;
    r.emplace_back("root", optional_string(request.root));
    r.emplace_back("query", optional_string(request.query));
    r.emplace_back("direction", Value::of(request.direction));
    r.emplace_back("depth", Value::of(request.depth));
    r.emplace_back("node_budget", Value::of(node_budget));
    r.emplace_back("edge_budget", Value::of(edge_budget));
    r.emplace_back("site_budget", Value::of(site_budget));
    r.emplace_back("byte_budget", Value::of(byte_budget));
    return Value::obj(std::move(r));
  }());
  out.emplace_back("metadata", Value::obj(std::move(metadata)));
  out.emplace_back("nodes", Value::arr(std::move(nodes)));
  out.emplace_back("edges", Value::arr(std::move(edges)));
  out.emplace_back("view_state", Value::obj({}));
  Value result = Value::obj(std::move(out));

  const auto mark_byte_truncated = [&] {
    if (Value *metadata_value = member(result, "metadata")) {
      set_bool_member(*metadata_value, "truncated", true);
      set_bool_member(*metadata_value, "evidence_truncated", true);
      if (Value *continuation = member(*metadata_value, "continuation")) {
        set_bool_member(*continuation, "available", true);
        for (auto &[name, value] : continuation->o) {
          if (name == "reason") {
            value = Value::of(std::string("byte_budget"));
          }
        }
      }
    }
    if (Value *nodes_value = member(result, "nodes")) {
      for (Value &node : nodes_value->a) {
        if (Value *status_value = member(node, "status")) {
          set_bool_member(*status_value, "truncated", true);
        }
        if (Value *evidence_value = member(node, "evidence")) {
          set_bool_member(*evidence_value, "truncated", true);
        }
      }
    }
    if (Value *edges_value = member(result, "edges")) {
      for (Value &edge : edges_value->a) {
        if (Value *status_value = member(edge, "status")) {
          set_bool_member(*status_value, "truncated", true);
          set_bool_member(*status_value, "evidence_truncated", true);
        }
        if (Value *evidence_value = member(edge, "evidence")) {
          set_bool_member(*evidence_value, "truncated", true);
          set_bool_member(*evidence_value, "sites_truncated", true);
        }
      }
    }
  };
  if (script_safe_size(json_out::dumps_indent2(result)) >
      static_cast<std::size_t>(byte_budget)) {
    mark_byte_truncated();
    while (script_safe_size(json_out::dumps_indent2(result)) >
               static_cast<std::size_t>(byte_budget) &&
           (member(result, "edges") != nullptr &&
            !member(result, "edges")->a.empty())) {
      member(result, "edges")->a.pop_back();
    }
    while (script_safe_size(json_out::dumps_indent2(result)) >
               static_cast<std::size_t>(byte_budget) &&
           (member(result, "nodes") != nullptr &&
            !member(result, "nodes")->a.empty())) {
      member(result, "nodes")->a.pop_back();
    }
    if (Value *metadata_value = member(result, "metadata")) {
      int64_t retained_sites = 0;
      if (Value *edges_value = member(result, "edges")) {
        for (const Value &edge : edges_value->a) {
          if (const Value *sites_value = member(edge, "sites")) {
            retained_sites += static_cast<int64_t>(sites_value->a.size());
          }
        }
      }
      set_int_member(*metadata_value, "sites_used", retained_sites);
    }
  }
  if (script_safe_size(json_out::dumps_indent2(result)) >
      static_cast<std::size_t>(byte_budget)) {
    throw CidxError(
        "cidx ui: byte budget is too small for fixed GraphView metadata");
  }
  return result;
}

} // namespace cidx::ui
