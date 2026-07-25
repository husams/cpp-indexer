#include "ui/graph_view.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <map>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

#include "catalogs/generated_catalog.hpp"
#include "graph/query.hpp"
#include "graph/records.hpp"
#include "query/cxq.hpp"
#include "query/exec.hpp"
#include "query/plan.hpp"
#include "util/errors.hpp"
#include "util/hashing.hpp"
#include "util/json_read.hpp"
#include "util/pathutil.hpp"

namespace cidx::ui {

const char *graph_input_kind_name(GraphInputKind kind) {
  switch (kind) {
  case GraphInputKind::Symbol:
    return "symbol";
  case GraphInputKind::File:
    return "file";
  case GraphInputKind::Entity:
    return "entity";
  case GraphInputKind::Type:
    return "type";
  case GraphInputKind::Cxq:
    return "cxq";
  case GraphInputKind::QueryPlan:
    return "plan";
  case GraphInputKind::Path:
    return "path";
  }
  return "unknown";
}

std::string GraphViewInput::canonical() const {
  return std::string(graph_input_kind_name(kind)) + ":" +
         std::to_string(value.size()) + ":" + value;
}

GraphViewError::GraphViewError(GraphViewFailureKind kind,
                               const std::string &message,
                               std::string next_action)
    : CidxError(message), kind_(kind), next_action_(std::move(next_action)) {}

const char *GraphViewError::code() const noexcept {
  switch (kind_) {
  case GraphViewFailureKind::InvalidInput:
    return "E_UI_INVALID_INPUT";
  case GraphViewFailureKind::UnsupportedInput:
    return "E_UI_UNSUPPORTED_INPUT";
  case GraphViewFailureKind::UnknownIdentity:
    return "E_UI_UNKNOWN_IDENTITY";
  case GraphViewFailureKind::AmbiguousIdentity:
    return "E_UI_AMBIGUOUS_IDENTITY";
  case GraphViewFailureKind::Oversized:
    return "E_UI_OVERSIZED";
  }
  return "E_UI_FAILURE";
}

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

std::string portable_file_id(Storage &db, int64_t file_id,
                             std::string_view fallback_path = {}) {
  const std::string source =
      fallback_path.empty()
          ? db.portable_source_identity_for_file(file_id)
          : db.portable_source_identity_for_path(std::string(fallback_path));
  return "file:v1:" + hex_field(source);
}

Value file_node_value(Storage &db, int64_t file_id, const std::string &path,
                      bool resolved, const std::string &freshness,
                      bool truncated) {
  Object out;
  out.emplace_back("id", Value::of(portable_file_id(db, file_id, path)));
  out.emplace_back("kind", Value::of(std::string("file")));
  out.emplace_back("path", Value::of(path));
  out.emplace_back("name", Value::of(pathutil::basename(path)));
  out.emplace_back("status", [&] {
    Object status_value;
    status_value.emplace_back(
        "completeness",
        Value::of(std::string(resolved ? "complete" : "partial")));
    status_value.emplace_back("freshness", Value::of(freshness));
    status_value.emplace_back("resolved", Value::of(resolved));
    status_value.emplace_back("truncated", Value::of(truncated));
    return Value::obj(std::move(status_value));
  }());
  out.emplace_back("evidence", Value::obj({
                                   {"bounded", Value::of(true)},
                                   {"path", Value::of(path)},
                               }));
  return Value::obj(std::move(out));
}

Value include_edge_value(Storage &db, const IncludeEdge &edge,
                         const std::string &source_path,
                         const std::string &freshness, bool truncated,
                         bool sites_truncated,
                         const std::vector<IncludeSite> &sites) {
  const std::string target_path =
      edge.dst_file_id
          ? db.file_abs_path(*edge.dst_file_id).value_or(edge.dst_path)
          : edge.dst_path;
  const std::string source_id =
      portable_file_id(db, edge.src_file_id, source_path);
  const std::string target_id =
      edge.dst_file_id ? portable_file_id(db, *edge.dst_file_id, target_path)
                       : portable_file_id(db, edge.src_file_id, target_path);
  const auto config = db.include_config_by_id(edge.config_id);
  Object out;
  out.emplace_back("id",
                   Value::of(std::string("include-edge:v1:") +
                             length_field(source_id) + length_field(target_id) +
                             length_field(config ? config->digest : "")));
  out.emplace_back("source", Value::of(source_id));
  out.emplace_back("target", Value::of(target_id));
  out.emplace_back("kind", Value::of(std::string("include")));
  out.emplace_back("path", Value::of(target_path));
  out.emplace_back("count", Value::of(edge.count));
  out.emplace_back("configuration", [&] {
    Object value;
    value.emplace_back("digest", Value::of(config ? config->digest : ""));
    value.emplace_back("driver",
                       optional_string(config ? config->driver : std::nullopt));
    value.emplace_back(
        "working_dir",
        optional_string(config ? config->working_dir : std::nullopt));
    Array arguments;
    if (config) {
      for (const auto &argument : config->arguments) {
        arguments.push_back(Value::of(argument));
      }
    }
    value.emplace_back("arguments", Value::arr(std::move(arguments)));
    return Value::obj(std::move(value));
  }());
  out.emplace_back("status", [&] {
    Object value;
    value.emplace_back("completeness", Value::of(std::string("complete")));
    value.emplace_back("freshness", Value::of(freshness));
    value.emplace_back("resolved", Value::of(edge.dst_file_id.has_value()));
    value.emplace_back("truncated", Value::of(truncated));
    value.emplace_back("evidence_truncated", Value::of(sites_truncated));
    return Value::obj(std::move(value));
  }());
  Array evidence_sites;
  for (const auto &site : sites) {
    Object value;
    value.emplace_back("line", Value::of(site.line));
    value.emplace_back("col", Value::of(site.col));
    value.emplace_back("spelling", Value::of(site.spelling));
    value.emplace_back("angled", Value::of(site.is_angled));
    value.emplace_back("directive", Value::of(site.directive));
    value.emplace_back("conditional",
                       Value::of(!site.cond_fingerprint.empty()));
    value.emplace_back("resolved", Value::of(site.resolved));
    evidence_sites.push_back(Value::obj(std::move(value)));
  }
  out.emplace_back("sites", Value::arr(std::move(evidence_sites)));
  out.emplace_back("evidence",
                   Value::obj({
                       {"bounded", Value::of(true)},
                       {"sites_truncated", Value::of(sites_truncated)},
                   }));
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

const Value &required_member(const Value &value, std::string_view key,
                             Value::T type) {
  const Value *child = member(value, key);
  if (child == nullptr || child->t != type) {
    throw CidxError("canonical QueryPlan: missing or invalid '" +
                    std::string(key) + "'");
  }
  return *child;
}

std::string string_member(const Value &value, std::string_view key) {
  return required_member(value, key, Value::T::Str).s;
}

int64_t int_member(const Value &value, std::string_view key) {
  return required_member(value, key, Value::T::Int).i;
}

std::optional<std::string> optional_string_member(const Value &value,
                                                  std::string_view key) {
  if (const Value *child = member(value, key)) {
    if (child->t != Value::T::Str) {
      throw CidxError("canonical QueryPlan: invalid '" + std::string(key) +
                      "'");
    }
    return child->s;
  }
  return std::nullopt;
}

query::View parse_plan_view(std::string_view name) {
  for (const query::View view :
       {query::View::Symbol, query::View::Entity, query::View::Parameter,
        query::View::TemplateParameter, query::View::TemplateArgument,
        query::View::CallArgument, query::View::Edge, query::View::Evidence,
        query::View::Type}) {
    if (name == query::view_name(view)) {
      return view;
    }
  }
  throw CidxError("canonical QueryPlan: unsupported view '" +
                  std::string(name) + "'");
}

query::UnknownPolicy parse_unknown_policy(std::string_view name) {
  if (name == "exclude") {
    return query::UnknownPolicy::Exclude;
  }
  if (name == "include") {
    return query::UnknownPolicy::Include;
  }
  if (name == "error") {
    return query::UnknownPolicy::Error;
  }
  throw CidxError("canonical QueryPlan: unsupported unknown policy '" +
                  std::string(name) + "'");
}

query::Pred parse_plan_pred(const Value &value) {
  const std::string op = string_member(value, "op");
  query::Pred result;
  if (op == "all_of" || op == "any_of") {
    result.op = op == "all_of" ? query::PredOp::AllOf : query::PredOp::AnyOf;
    const Value &preds = required_member(value, "preds", Value::T::Arr);
    for (const Value &pred : preds.a) {
      if (pred.t != Value::T::Obj) {
        throw CidxError("canonical QueryPlan: predicate must be an object");
      }
      result.kids.push_back(parse_plan_pred(pred));
    }
    return result;
  }
  if (op == "not") {
    result.op = query::PredOp::Not;
    result.kids.push_back(
        parse_plan_pred(required_member(value, "pred", Value::T::Obj)));
    return result;
  }
  if (op == "eq" || op == "ne" || op == "glob") {
    if (op == "eq") {
      result.op = query::PredOp::Eq;
    } else if (op == "ne") {
      result.op = query::PredOp::Ne;
    } else {
      result.op = query::PredOp::Glob;
    }
    result.field = string_member(value, "field");
    const Value *raw = member(value, "value");
    if (raw == nullptr ||
        (raw->t != Value::T::Str && raw->t != Value::T::Int)) {
      throw CidxError(
          "canonical QueryPlan: comparison value has an invalid type");
    }
    if (raw->t == Value::T::Int) {
      result.int_value = raw->i;
    } else {
      result.str_values.push_back(raw->s);
    }
    return result;
  }
  if (op == "in") {
    result.op = query::PredOp::In;
    result.field = string_member(value, "field");
    const Value &values = required_member(value, "values", Value::T::Arr);
    for (const Value &item : values.a) {
      if (item.t != Value::T::Str) {
        throw CidxError("canonical QueryPlan: in values must be strings");
      }
      result.str_values.push_back(item.s);
    }
    return result;
  }
  query::PredOp quantifier = query::PredOp::Exists;
  if (op == "none") {
    quantifier = query::PredOp::None;
  } else if (op == "all") {
    quantifier = query::PredOp::All;
  } else if (op == "at_least") {
    quantifier = query::PredOp::AtLeast;
  } else if (op == "exactly") {
    quantifier = query::PredOp::Exactly;
  } else {
    throw CidxError("canonical QueryPlan: unsupported predicate op '" + op +
                    "'");
  }
  result.op = quantifier;
  result.relation = string_member(value, "relation");
  result.min_depth = int_member(value, "min_depth");
  result.max_depth = int_member(value, "max_depth");
  if (const auto direction = optional_string_member(value, "direction")) {
    if (*direction != "in") {
      throw CidxError("canonical QueryPlan: unsupported predicate direction");
    }
    result.inbound = true;
  }
  if (quantifier == query::PredOp::AtLeast ||
      quantifier == query::PredOp::Exactly) {
    result.threshold = int_member(value, "threshold");
  }
  if (const Value *pred = member(value, "pred")) {
    if (pred->t != Value::T::Obj) {
      throw CidxError("canonical QueryPlan: invalid quantifier predicate");
    }
    result.target = std::make_shared<query::Pred>(parse_plan_pred(*pred));
  }
  return result;
}

query::Plan parse_canonical_plan_value(const Value &root) {
  if (root.t != Value::T::Obj || int_member(root, "cxq") != 1) {
    throw CidxError("canonical QueryPlan: expected cxq version 1");
  }
  const Value &source = required_member(root, "source", Value::T::Obj);
  const std::string source_kind = string_member(source, "kind");
  query::Plan plan;
  if (source_kind == "codebase") {
    plan.source = query::codebase();
  } else if (source_kind == "symbol") {
    plan.source = query::symbol(string_member(source, "ref"));
  } else if (source_kind == "entity") {
    plan.source = query::entity(string_member(source, "ref"));
  } else {
    throw CidxError("canonical QueryPlan: unsupported source kind '" +
                    source_kind + "'");
  }
  const Value &stages = required_member(root, "stages", Value::T::Arr);
  for (const Value &value : stages.a) {
    if (value.t != Value::T::Obj) {
      throw CidxError("canonical QueryPlan: stage must be an object");
    }
    const std::string op = string_member(value, "op");
    query::Stage stage;
    if (op == "nodes") {
      stage.op = query::StageOp::Nodes;
      if (const Value *pred = member(value, "pred")) {
        stage.pred = parse_plan_pred(*pred);
      }
    } else if (op == "view") {
      stage.op = query::StageOp::ChangeView;
      stage.level = parse_plan_view(string_member(value, "level"));
    } else if (op == "where") {
      stage.op = query::StageOp::Where;
      stage.pred =
          parse_plan_pred(required_member(value, "pred", Value::T::Obj));
    } else if (op == "out" || op == "in") {
      stage.op = op == "out" ? query::StageOp::Out : query::StageOp::In;
      stage.relation = string_member(value, "relation");
      stage.min_depth = int_member(value, "min_depth");
      stage.max_depth = int_member(value, "max_depth");
      if (const auto mode = optional_string_member(value, "mode")) {
        if (*mode != "static" && *mode != "devirtualized") {
          throw CidxError("canonical QueryPlan: unsupported traversal mode");
        }
        stage.mode = *mode == "devirtualized"
                         ? query::TraversalMode::Devirtualized
                         : query::TraversalMode::Static;
      }
    } else if (op == "union" || op == "intersect" || op == "except") {
      if (op == "union") {
        stage.op = query::StageOp::Union;
      } else if (op == "intersect") {
        stage.op = query::StageOp::Intersect;
      } else {
        stage.op = query::StageOp::Except;
      }
      stage.operand = std::make_shared<query::Plan>(parse_canonical_plan_value(
          required_member(value, "plan", Value::T::Obj)));
    } else if (op == "select" || op == "order_by") {
      stage.op =
          op == "select" ? query::StageOp::Select : query::StageOp::OrderBy;
      const Value &fields = required_member(value, "fields", Value::T::Arr);
      for (const Value &field : fields.a) {
        if (field.t != Value::T::Str) {
          throw CidxError("canonical QueryPlan: fields must be strings");
        }
        stage.fields.push_back(field.s);
      }
    } else if (op == "count") {
      stage.op = query::StageOp::Count;
    } else if (op == "distinct") {
      stage.op = query::StageOp::Distinct;
    } else if (op == "limit") {
      stage.op = query::StageOp::Limit;
      stage.n = int_member(value, "n");
    } else {
      throw CidxError("canonical QueryPlan: unsupported stage op '" + op + "'");
    }
    if (const auto unknown = optional_string_member(value, "unknown")) {
      stage.unknown = parse_unknown_policy(*unknown);
    }
    plan.stages.push_back(std::move(stage));
  }
  return plan;
}

query::Plan parse_canonical_plan(const std::string &text) {
  return parse_canonical_plan_value(json_read::parse(text));
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

std::string graph_query_identity(const GraphViewRequest &request,
                                 std::string_view normalized_input,
                                 const IndexIdentity &identity,
                                 const std::optional<std::string> &plan_json) {
  Object material;
  material.emplace_back("version", Value::of(kGraphViewVersion));
  material.emplace_back("input", Value::of(std::string(normalized_input)));
  material.emplace_back("plan", optional_string(plan_json));
  material.emplace_back("direction", Value::of(request.direction));
  material.emplace_back("depth", Value::of(request.depth));
  material.emplace_back("node_budget", Value::of(request.node_budget));
  material.emplace_back("edge_budget", Value::of(request.edge_budget));
  material.emplace_back("site_budget", Value::of(request.site_budget));
  material.emplace_back("byte_budget", Value::of(request.byte_budget));
  material.emplace_back("workspace", request.workspace
                                         ? Value::of(*request.workspace)
                                         : Value::null());
  Array edge_kinds;
  if (request.edge_kinds) {
    std::vector<std::string> sorted = *request.edge_kinds;
    std::ranges::sort(sorted);
    for (const auto &kind : sorted) {
      edge_kinds.push_back(Value::of(kind));
    }
  }
  material.emplace_back("edge_kinds", Value::arr(std::move(edge_kinds)));
  material.emplace_back("workspace_identity", Value::of(identity.workspace));
  material.emplace_back("schema_version", Value::of(identity.schema_version));
  material.emplace_back("source_revision",
                        optional_string(identity.source_revision));
  material.emplace_back("source_fingerprint",
                        optional_string(identity.source_fingerprint));
  material.emplace_back("index_config_fingerprint",
                        optional_string(identity.index_config_fingerprint));
  std::string hash_material = "cidx.graph-view.query.v1";
  hash_material.push_back('\0');
  hash_material += json_out::dumps_indent2(Value::obj(std::move(material)));
  return sha256_hex(hash_material);
}

std::vector<std::string> fact_sets_for(const GraphInputKind kind,
                                       bool has_edges, bool has_sites,
                                       bool has_includes = false) {
  std::set<std::string> facts;
  switch (kind) {
  case GraphInputKind::Symbol:
    facts.insert("symbols");
    break;
  case GraphInputKind::File:
    facts.insert("files");
    facts.insert("symbols");
    break;
  case GraphInputKind::Entity:
    facts.insert("entities");
    facts.insert("entity_edges");
    break;
  case GraphInputKind::Type:
    facts.insert("types");
    facts.insert("symbols");
    break;
  case GraphInputKind::Cxq:
  case GraphInputKind::QueryPlan:
    facts.insert("query-plan");
    facts.insert("symbols");
    break;
  case GraphInputKind::Path:
    facts.insert("symbols");
    facts.insert("witness-path");
    break;
  }
  if (has_edges) {
    facts.insert("edges");
  }
  if (has_sites) {
    facts.insert("sites");
  }
  if (has_includes) {
    facts.insert("includes");
  }
  return {facts.begin(), facts.end()};
}

Value identity_value(const IndexIdentity &identity,
                     const std::vector<std::string> &fact_sets) {
  Object out;
  out.emplace_back("workspace", Value::of(identity.workspace));
  out.emplace_back("index", Value::of("semantic-index/schema/" +
                                      std::to_string(identity.schema_version)));
  Array facts;
  for (const auto &fact : fact_sets) {
    facts.push_back(Value::of(fact));
  }
  out.emplace_back("fact_sets", Value::arr(std::move(facts)));
  out.emplace_back("catalog_version", Value::of(catalog::kCatalogVersion));
  out.emplace_back("catalog_hash",
                   Value::of(std::string(catalog::kCatalogHash)));
  out.emplace_back("freshness", Value::of(identity.freshness));
  out.emplace_back("source_revision",
                   optional_string(identity.source_revision));
  out.emplace_back("source_fingerprint",
                   optional_string(identity.source_fingerprint));
  return Value::obj(std::move(out));
}

} // namespace

Value build_graph_view(Storage &db, const GraphViewRequest &request) {
  query::SqliteQueryReadAdapter graph_read(db);
  graph::GraphQuery graph(graph_read, "<ui>");
  IndexIdentity identity = db.index_identity();
  if (identity.freshness == "unverifiable" &&
      std::ranges::any_of(db.list_files(), [](const auto &entry) {
        return !entry.first.indexed;
      })) {
    identity.freshness = "stale";
  }
  const std::string freshness = identity.freshness;
  const int node_budget = std::clamp(request.node_budget, 1, 10000);
  const int edge_budget = std::clamp(request.edge_budget, 1, 20000);
  const int site_budget = std::clamp(request.site_budget, 0, 20000);
  const int byte_budget =
      std::clamp(request.byte_budget, 1024, 64 * 1024 * 1024);

  GraphViewInput input = request.input.value_or(GraphViewInput{
      .kind = GraphInputKind::Symbol,
      .value = request.root ? *request.root : request.query.value_or("")});
  if (request.root && request.query && *request.root != *request.query) {
    throw GraphViewError(GraphViewFailureKind::InvalidInput,
                         "--root and --query must identify the same symbol",
                         "provide one symbol reference");
  }
  if (input.value.empty() && request.strict) {
    throw GraphViewError(GraphViewFailureKind::InvalidInput,
                         "a typed GraphView input is required",
                         "provide --input-kind and --input");
  }

  RootResolution resolution;
  std::vector<graph::Sym> input_symbols;
  std::optional<query::Plan> normalized_plan;
  std::optional<std::string> normalized_plan_json;
  std::optional<query::Result> normalized_result;
  std::optional<File> input_file;
  std::vector<IncludeEdge> include_edges;
  bool has_include_facts = false;
  const auto add_symbol_by_id = [&](int64_t id) {
    if (const auto symbol = graph.get_by_id(id)) {
      input_symbols.push_back(*symbol);
    }
  };
  const auto collect_plan = [&](const query::Plan &plan) {
    normalized_plan = query::validate(plan);
    normalized_plan_json = query::canonical_json(*normalized_plan);
    query::SqliteQueryReadAdapter read(db);
    normalized_result = query::Executor(read).run(*normalized_plan);
    const query::Result &result = *normalized_result;
    if (result.shape != query::Shape::Nodes ||
        (result.view != query::View::Symbol &&
         result.view != query::View::Entity) ||
        result.fields.empty() || result.fields.front() != "id") {
      throw GraphViewError(
          GraphViewFailureKind::UnsupportedInput,
          "QueryPlan result must be a bounded symbol/entity node view with an "
          "id field",
          "remove select/count/view stages or select the supported id field");
    }
    for (const auto &row : result.rows) {
      if (row.size() != result.fields.size() || row.empty() ||
          !std::holds_alternative<int64_t>(row.front())) {
        throw GraphViewError(
            GraphViewFailureKind::UnsupportedInput,
            "QueryPlan result has an unsupported row shape or id field",
            "return the validated node/entity view with its id field first");
      }
      add_symbol_by_id(std::get<int64_t>(row.front()));
    }
    if (result.truncated) {
      throw GraphViewError(
          GraphViewFailureKind::Oversized,
          "QueryPlan result exceeded the bounded export result limit",
          "add a smaller limit stage to the QueryPlan");
    }
  };
  const auto resolved_symbol_identity = [&] {
    std::vector<std::string> ids;
    ids.reserve(input_symbols.size());
    for (const auto &symbol : input_symbols) {
      ids.push_back(portable_id(symbol));
    }
    std::ranges::sort(ids);
    ids.erase(std::ranges::unique(ids).begin(), ids.end());
    std::string out = "symbols:v1:";
    for (const auto &id : ids) {
      out += length_field(id);
    }
    return out;
  };
  std::string normalized_input = input.canonical();

  const auto include_file_path = [&](int64_t file_id,
                                     const std::string &fallback) {
    return db.file_abs_path(file_id).value_or(fallback);
  };

  const auto record_file_identity = [&](const File &file) {
    normalized_input = "file:" + db.portable_source_identity_for_file(file.id);
  };

  const auto record_symbol_identity = [&] {
    normalized_input = resolved_symbol_identity();
  };

  const auto record_plan_identity = [&] {
    normalized_input = "plan:v1:" + normalized_plan_json.value_or("");
  };

  Array nodes;
  Array edges;
  bool truncated = false;
  bool evidence_truncated = false;
  int sites_used = 0;

  const auto collect_include_edges = [&](const File &file) {
    include_edges = db.include_edges_from(file.id, false);
    std::ranges::sort(include_edges, [&](const IncludeEdge &left,
                                         const IncludeEdge &right) {
      const std::string left_target =
          left.dst_file_id ? include_file_path(*left.dst_file_id, left.dst_path)
                           : left.dst_path;
      const std::string right_target =
          right.dst_file_id
              ? include_file_path(*right.dst_file_id, right.dst_path)
              : right.dst_path;
      return std::tie(left_target, left.config_id, left.id) <
             std::tie(right_target, right.config_id, right.id);
    });
    has_include_facts =
        !include_edges.empty() || !db.include_configs_for_tu(file.id).empty();
  };

  const auto append_include_facts = [&] {
    if (!input_file || !has_include_facts) {
      return;
    }
    const std::string source_path =
        include_file_path(input_file->id, input_file->name);
    std::set<std::string> emitted_files;
    const auto append_file = [&](int64_t file_id, const std::string &path,
                                 bool resolved) {
      const std::string id = portable_file_id(db, file_id, path);
      if (!emitted_files.insert(id).second) {
        return;
      }
      if (nodes.size() >= static_cast<std::size_t>(node_budget)) {
        truncated = true;
        return;
      }
      nodes.push_back(
          file_node_value(db, file_id, path, resolved, freshness, truncated));
    };
    append_file(input_file->id, source_path, true);
    int emitted_edges = 0;
    int sites_remaining = site_budget - sites_used;
    for (const auto &include : include_edges) {
      const std::string target_path =
          include.dst_file_id
              ? include_file_path(*include.dst_file_id, include.dst_path)
              : include.dst_path;
      append_file(include.dst_file_id.value_or(input_file->id), target_path,
                  include.dst_file_id.has_value());
      if (emitted_edges++ == edge_budget) {
        truncated = true;
        break;
      }
      const int fetch_limit = std::max(0, sites_remaining) + 1;
      auto sites = db.include_sites_for(include.id);
      std::ranges::sort(
          sites, [](const IncludeSite &left, const IncludeSite &right) {
            return std::tie(left.line, left.col, left.begin_offset) <
                   std::tie(right.line, right.col, right.begin_offset);
          });
      const bool sites_truncated =
          sites.size() > static_cast<std::size_t>(fetch_limit);
      if (sites.size() >
          static_cast<std::size_t>(std::max(0, sites_remaining))) {
        sites.resize(static_cast<std::size_t>(std::max(0, sites_remaining)));
      }
      sites_used += static_cast<int>(sites.size());
      sites_remaining -= static_cast<int>(sites.size());
      evidence_truncated = evidence_truncated || sites_truncated;
      edges.push_back(include_edge_value(db, include, source_path, freshness,
                                         truncated, sites_truncated, sites));
    }
  };

  const auto normalize_after_symbols = [&] {
    if (normalized_input == input.canonical()) {
      record_symbol_identity();
    }
  };

  switch (input.kind) {
  case GraphInputKind::Symbol:
    resolution = resolve_root(graph, db, input.value);
    if (resolution.symbol) {
      input_symbols.push_back(*resolution.symbol);
    }
    break;
  case GraphInputKind::File: {
    const auto file = db.get_file(input.value);
    if (!file) {
      resolution.status = "unknown";
      break;
    }
    input_file = file;
    record_file_identity(*input_file);
    for (const auto &symbol : db.symbols_in_file(file->id)) {
      add_symbol_by_id(symbol.id);
    }
    collect_include_edges(*input_file);
    resolution.status = "exact_file";
    if (!input_symbols.empty()) {
      resolution.symbol = input_symbols.front();
    }
    break;
  }
  case GraphInputKind::Entity:
    collect_plan(query::start(query::entity(input.value)).plan());
    record_plan_identity();
    resolution.status = input_symbols.empty() ? "unknown" : "exact_entity";
    if (!input_symbols.empty()) {
      resolution.symbol = input_symbols.front();
    }
    break;
  case GraphInputKind::Type: {
    const auto type_ids = db.type_ids_reaching(input.value);
    for (const int64_t id : db.symbols_named_by_types(type_ids)) {
      add_symbol_by_id(id);
    }
    resolution.status = input_symbols.empty() ? "unknown" : "exact_type";
    if (!input_symbols.empty()) {
      resolution.symbol = input_symbols.front();
    }
    record_symbol_identity();
    break;
  }
  case GraphInputKind::Cxq:
    try {
      collect_plan(query::parse_cxq(input.value));
    } catch (const query::PlanError &error) {
      throw GraphViewError(GraphViewFailureKind::InvalidInput, error.what(),
                           "provide a valid bounded CXQ QueryPlan");
    }
    record_plan_identity();
    resolution.status = input_symbols.empty() ? "unknown" : "exact_plan";
    if (!input_symbols.empty()) {
      resolution.symbol = input_symbols.front();
    }
    break;
  case GraphInputKind::QueryPlan:
    try {
      collect_plan(parse_canonical_plan(input.value));
    } catch (const GraphViewError &) {
      throw;
    } catch (const std::exception &error) {
      throw GraphViewError(
          GraphViewFailureKind::InvalidInput, error.what(),
          "provide canonical QueryPlan JSON from the plan API");
    }
    record_plan_identity();
    resolution.status = input_symbols.empty() ? "unknown" : "exact_plan";
    if (!input_symbols.empty()) {
      resolution.symbol = input_symbols.front();
    }
    break;
  case GraphInputKind::Path: {
    const std::size_t separator = input.value.find("->");
    if (separator == std::string::npos || separator == 0 ||
        separator + 2 >= input.value.size()) {
      throw GraphViewError(GraphViewFailureKind::InvalidInput,
                           "bounded path must use SOURCE->TARGET",
                           "provide two symbol identities separated by ->");
    }
    const auto source =
        resolve_root(graph, db, input.value.substr(0, separator));
    const auto target =
        resolve_root(graph, db, input.value.substr(separator + 2));
    if (source.status == "ambiguous" || target.status == "ambiguous") {
      resolution.status = "ambiguous";
    } else if (!source.symbol || !target.symbol) {
      resolution.status = "unknown";
    } else {
      const auto path =
          graph.reaches(source.symbol->id, target.symbol->id,
                        request.edge_kinds, request.direction, request.depth);
      if (!path) {
        resolution.status = "unknown";
      } else {
        resolution.status = "exact_path";
        resolution.symbol = source.symbol;
        input_symbols = *path;
      }
    }
    resolution.candidates = source.candidates;
    resolution.candidates.insert(resolution.candidates.end(),
                                 target.candidates.begin(),
                                 target.candidates.end());
    break;
  }
  }

  if (request.strict && resolution.status == "ambiguous") {
    throw GraphViewError(
        GraphViewFailureKind::AmbiguousIdentity,
        "typed GraphView input resolves to multiple identities",
        "use a portable identity or a narrower QueryPlan");
  }
  if (request.strict && resolution.status == "unknown") {
    throw GraphViewError(
        GraphViewFailureKind::UnknownIdentity,
        "typed GraphView input does not resolve to an indexed identity",
        "check the file/path or query the index for a canonical identity");
  }
  normalize_after_symbols();
  const std::string query_identity = graph_query_identity(
      request, normalized_input, identity, normalized_plan_json);
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
  metadata.emplace_back("query_identity", Value::of(query_identity));
  metadata.emplace_back("query", Value::null());
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
  metadata.emplace_back(
      "identity",
      identity_value(identity, fact_sets_for(input.kind, false, false)));

  if (!input_symbols.empty() || input_file) {
    std::optional<query::Result> query_result = normalized_result;
    if (normalized_plan) {
      if (!query_result) {
        query::SqliteQueryReadAdapter read(db);
        query_result = query::Executor(read).run(*normalized_plan);
      }
      metadata.emplace_back("query_plan", Value::of(*normalized_plan_json));
    } else if (resolution.symbol && input.kind == GraphInputKind::Symbol) {
      const query::Query plan =
          make_query_plan(*resolution.symbol, request, node_budget);
      query::SqliteQueryReadAdapter read(db);
      query_result = query::Executor(read).run(plan.plan());
      metadata.emplace_back("query_plan",
                            Value::of(query::canonical_json(plan.plan())));
    } else {
      metadata.emplace_back("query_plan", Value::null());
    }
    append_include_facts();
    if (query_result) {
      truncated =
          query_result->truncated ||
          query_result->rows.size() > static_cast<std::size_t>(node_budget);
    }

    std::map<int64_t, graph::Sym> symbols_by_id;
    for (const auto &symbol : input_symbols) {
      symbols_by_id.emplace(symbol.id, symbol);
    }
    if (query_result) {
      for (const auto &row : query_result->rows) {
        if (row.empty() || !std::holds_alternative<int64_t>(row.front())) {
          continue;
        }
        const auto symbol = graph.get_by_id(std::get<int64_t>(row.front()));
        if (symbol) {
          symbols_by_id.emplace(symbol->id, *symbol);
        }
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
    const std::size_t remaining_node_budget =
        nodes.size() >= static_cast<std::size_t>(node_budget)
            ? 0
            : static_cast<std::size_t>(node_budget) - nodes.size();
    if (ordered_nodes.size() > remaining_node_budget) {
      ordered_nodes.resize(remaining_node_budget);
      truncated = true;
    }
    std::set<int64_t> selected_ids;
    std::map<int64_t, int> depths;
    for (const auto &symbol : ordered_nodes) {
      selected_ids.insert(symbol.id);
      depths.emplace(
          symbol.id,
          resolution.symbol && symbol.id == resolution.symbol->id ? 0 : 1);
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

  const bool input_resolved =
      resolution.symbol.has_value() || resolution.status == "exact_file";
  const bool initial_unknown = identity.freshness != "current" ||
                               !db.graph_resolved() || !input_resolved;
  std::string graph_status = "complete";
  if (initial_unknown) {
    graph_status = "unknown";
  } else if (truncated) {
    graph_status = "partial";
  }
  Array markers;
  if (identity.freshness == "stale") {
    markers.push_back(Value::of(std::string("stale")));
  }
  if (identity.freshness != "current" || !db.graph_resolved()) {
    markers.push_back(Value::of(std::string("unknown")));
  }
  if (!input_resolved) {
    markers.push_back(Value::of(std::string("unresolved")));
  }
  if (truncated) {
    markers.push_back(Value::of(std::string("truncated")));
  }
  metadata.emplace_back("status", Value::of(graph_status));
  metadata.emplace_back("markers", Value::arr(markers));

  const auto fact_sets = fact_sets_for(input.kind, !edges.empty(),
                                       sites_used > 0, has_include_facts);
  Value exact_identity = identity_value(identity, fact_sets);
  for (auto &[name, value] : metadata) {
    if (name == "identity") {
      value = exact_identity;
    }
  }

  Object out;
  out.emplace_back("schema", Value::of(std::string("cidx.graph-view.v1")));
  out.emplace_back("version", Value::of(kGraphViewVersion));
  out.emplace_back("status", Value::of(graph_status));
  out.emplace_back("markers", Value::arr(markers));
  out.emplace_back("query_identity", Value::of(query_identity));
  out.emplace_back("result_id", Value::of(std::string(64, '0')));
  out.emplace_back("identity", exact_identity);
  out.emplace_back("request", [&] {
    Object r;
    r.emplace_back("input_kind",
                   Value::of(std::string(graph_input_kind_name(input.kind))));
    r.emplace_back("input", Value::of(normalized_input));
    r.emplace_back("raw_input", Value::of(input.canonical()));
    r.emplace_back("root", Value::null());
    r.emplace_back("query", Value::null());
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
  bool byte_truncated = false;

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
    byte_truncated = true;
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

  const bool final_truncated = truncated || byte_truncated;
  const bool unknown = identity.freshness != "current" ||
                       !db.graph_resolved() || !input_resolved;
  std::string final_status = "complete";
  if (unknown) {
    final_status = "unknown";
  } else if (final_truncated) {
    final_status = "partial";
  }
  Array final_markers;
  if (identity.freshness == "stale") {
    final_markers.push_back(Value::of(std::string("stale")));
  }
  if (identity.freshness != "current" || !db.graph_resolved()) {
    final_markers.push_back(Value::of(std::string("unknown")));
  }
  if (!input_resolved) {
    final_markers.push_back(Value::of(std::string("unresolved")));
  }
  if (final_truncated) {
    final_markers.push_back(Value::of(std::string("truncated")));
  }
  const auto set_status = [&](Value &value) {
    if (Value *status_value = member(value, "status")) {
      for (auto &[name, child] : status_value->o) {
        if (name == "truncated") {
          child = Value::of(final_truncated);
        }
        if (name == "evidence_truncated") {
          child = Value::of(evidence_truncated || byte_truncated);
        }
      }
    }
  };
  if (Value *metadata_value = member(result, "metadata")) {
    if (Value *status_value = member(*metadata_value, "status")) {
      *status_value = Value::of(final_status);
    }
    if (Value *markers_value = member(*metadata_value, "markers")) {
      *markers_value = Value::arr(final_markers);
    }
    set_bool_member(*metadata_value, "truncated", final_truncated);
    set_bool_member(*metadata_value, "evidence_truncated",
                    evidence_truncated || byte_truncated);
    if (Value *continuation = member(*metadata_value, "continuation")) {
      set_bool_member(*continuation, "available", final_truncated);
      for (auto &[name, child] : continuation->o) {
        if (name == "reason") {
          std::string reason = "complete";
          if (byte_truncated) {
            reason = "byte_budget";
          } else if (final_truncated) {
            reason = "budget";
          }
          child = Value::of(std::move(reason));
        }
      }
    }
  }
  for (Value &node : member(result, "nodes")->a) {
    set_status(node);
  }
  for (Value &edge : member(result, "edges")->a) {
    set_status(edge);
  }
  if (Value *status_value = member(result, "status")) {
    *status_value = Value::of(final_status);
  }
  if (Value *markers_value = member(result, "markers")) {
    *markers_value = Value::arr(final_markers);
  }

  const auto final_facts = fact_sets_for(
      input.kind, !member(result, "edges")->a.empty(),
      [&] {
        return std::ranges::any_of(
            member(result, "edges")->a, [](const Value &edge) {
              if (const Value *sites = member(edge, "sites")) {
                return !sites->a.empty();
              }
              return false;
            });
      }(),
      has_include_facts);
  exact_identity = identity_value(identity, final_facts);
  if (Value *identity_value_member = member(result, "identity")) {
    *identity_value_member = exact_identity;
  }
  if (Value *metadata_value = member(result, "metadata")) {
    if (Value *identity_member = member(*metadata_value, "identity")) {
      *identity_member = exact_identity;
    }
  }
  Value canonical = result;
  if (Value *result_id = member(canonical, "result_id")) {
    *result_id = Value::of(std::string(64, '0'));
  }
  if (Value *result_id = member(result, "result_id")) {
    std::string result_material = "cidx.graph-view.result.v2";
    result_material.push_back('\0');
    result_material += json_out::dumps_indent2(canonical);
    *result_id = Value::of(sha256_hex(result_material));
  }
  if (script_safe_size(json_out::dumps_indent2(result)) >
      static_cast<std::size_t>(byte_budget)) {
    throw GraphViewError(
        GraphViewFailureKind::Oversized,
        "byte budget is too small for the finalized GraphView metadata",
        "increase --byte-limit or reduce the input and evidence budgets");
  }
  return result;
}

} // namespace cidx::ui
