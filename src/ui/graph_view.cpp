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

struct PortableEdgeRef {
  std::string source;
  std::string kind;
  std::string target;
};

// Reverse of portable_edge_id() (declared below): recovers the endpoint
// identities and relation kind embedded in a previously-emitted edge id, for
// HSE-92's live evidence-loading operation. Never trusts the input beyond
// this bounded, fully-validated decode.
std::optional<PortableEdgeRef> parse_portable_edge_id(std::string_view value) {
  constexpr std::string_view prefix = "edge:v1:";
  if (!value.starts_with(prefix)) {
    return std::nullopt;
  }
  std::size_t offset = prefix.size();
  const auto source = length_value(value, offset);
  const auto kind = length_value(value, offset);
  const auto target = length_value(value, offset);
  if (!source || !kind || !target || offset != value.size()) {
    return std::nullopt;
  }
  return PortableEdgeRef{.source = std::string(*source),
                         .kind = std::string(*kind),
                         .target = std::string(*target)};
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
  out.emplace_back("component", sym.component ? Value::of(*sym.component)
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
  out.emplace_back("args_redacted", Value::of(true));
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
  // Exported file identities are deliberately non-reversible. The storage
  // source identity is useful internally but may contain absolute checkout or
  // external paths, so never expose it through reversible encoding.
  const std::string source =
      fallback_path.empty()
          ? db.portable_source_identity_for_file(file_id)
          : db.portable_source_identity_for_path(std::string(fallback_path));
  std::string material = "cidx.graph-view.file.v2";
  material.push_back('\0');
  material += source;
  return "file:v2:" + sha256_hex(material);
}

Value file_node_value(Storage &db, int64_t file_id, const std::string &path,
                      bool resolved, const std::string &freshness,
                      bool truncated,
                      const std::optional<std::string> &workspace) {
  const std::string portable_path = redacted_path(path, workspace);
  Object out;
  out.emplace_back("id", Value::of(portable_file_id(db, file_id, path)));
  out.emplace_back("kind", Value::of(std::string("file")));
  out.emplace_back("path", Value::of(portable_path));
  out.emplace_back("name", Value::of(pathutil::basename(portable_path)));
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
                                   {"path", Value::of(portable_path)},
                               }));
  return Value::obj(std::move(out));
}

Value include_edge_value(Storage &db, const IncludeEdge &edge,
                         const std::string &source_path,
                         const std::string &freshness, bool truncated,
                         bool sites_truncated,
                         const std::vector<IncludeSite> &sites,
                         const std::optional<std::string> &workspace) {
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
  const std::string portable_target_path =
      redacted_path(target_path, workspace);
  Object out;
  out.emplace_back("id",
                   Value::of(std::string("include-edge:v1:") +
                             length_field(source_id) + length_field(target_id) +
                             length_field(config ? config->digest : "")));
  out.emplace_back("source", Value::of(source_id));
  out.emplace_back("target", Value::of(target_id));
  out.emplace_back("kind", Value::of(std::string("include")));
  out.emplace_back("path", Value::of(portable_target_path));
  out.emplace_back("count", Value::of(edge.count));
  out.emplace_back("configuration", [&] {
    Object value;
    value.emplace_back("digest", Value::of(config ? config->digest : ""));
    value.emplace_back("driver", [&] {
      if (!config || !config->driver) {
        return Value::null();
      }
      return Value::of(redacted_path(*config->driver, workspace));
    }());
    value.emplace_back("working_dir", [&] {
      if (!config || !config->working_dir) {
        return Value::null();
      }
      return Value::of(redacted_path(*config->working_dir, workspace));
    }());
    value.emplace_back(
        "argument_count",
        Value::of(static_cast<int64_t>(config ? config->arguments.size() : 0)));
    value.emplace_back("arguments_redacted", Value::of(true));
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
    value.emplace_back("spelling",
                       Value::of(redacted_path(site.spelling, workspace)));
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

std::string entity_id(const graph::Sym &symbol, const EntityNode &entity) {
  return "entity:v1:" + length_field(portable_id(symbol)) +
         length_field(entity.kind_name);
}

std::string type_id(const TypeNode &type) {
  return "type:v1:" + hex_field(type.type_key) +
         length_field(std::to_string(type.kind));
}

std::string entity_edge_id(std::string_view source, const EntityEdge &edge,
                           std::string_view target) {
  return "entity-edge:v1:" + length_field(source) +
         length_field(edge.kind_name) + length_field(target) +
         length_field(std::to_string(edge.via_member_id.value_or(-1))) +
         length_field(std::to_string(edge.create_form.value_or(-1)));
}

std::string type_edge_kind_name(int64_t kind) {
  switch (kind) {
  case 1:
    return "pointee";
  case 2:
    return "element_type";
  case 3:
    return "alias_of";
  case 4:
    return "return_type";
  case 5:
    return "param_type";
  case 6:
    return "template_argument_type";
  case 7:
    return "member_owner";
  case 8:
    return "member_component";
  default:
    return "unknown";
  }
}

Value entity_node_value(const graph::Sym &symbol, const EntityNode &entity,
                        const std::string &freshness, bool truncated,
                        const std::optional<std::string> &workspace) {
  Object out;
  out.emplace_back("id", Value::of(entity_id(symbol, entity)));
  out.emplace_back("kind", Value::of(std::string("entity")));
  out.emplace_back("entity_kind", Value::of(entity.kind_name));
  out.emplace_back("name",
                   Value::of(symbol.name.empty() ? symbol.usr : symbol.name));
  out.emplace_back("identity", Value::of(portable_id(symbol)));
  out.emplace_back("location", Value::of(location(symbol, workspace)));
  out.emplace_back("status", [&] {
    Object value;
    value.emplace_back(
        "completeness",
        Value::of(std::string(symbol.resolved ? "complete" : "partial")));
    value.emplace_back("freshness", Value::of(freshness));
    value.emplace_back("resolved", Value::of(symbol.resolved));
    value.emplace_back("truncated", Value::of(truncated));
    return Value::obj(std::move(value));
  }());
  out.emplace_back("evidence",
                   Value::obj({
                       {"bounded", Value::of(true)},
                       {"location", Value::of(location(symbol, workspace))},
                   }));
  return Value::obj(std::move(out));
}

Value entity_edge_value(const EntityEdge &edge, std::string_view source,
                        std::string_view target, const std::string &freshness,
                        bool truncated) {
  Object out;
  out.emplace_back("id", Value::of(entity_edge_id(source, edge, target)));
  out.emplace_back("source", Value::of(std::string(source)));
  out.emplace_back("target", Value::of(std::string(target)));
  out.emplace_back("kind", Value::of(edge.kind_name));
  out.emplace_back("count", Value::of(edge.count));
  out.emplace_back("multiplicity", Value::of(edge.multiplicity));
  out.emplace_back("access", Value::of(edge.access));
  out.emplace_back("is_virtual", Value::of(edge.is_virtual));
  out.emplace_back("partial", Value::of(edge.partial));
  out.emplace_back("via_member_id", optional_int(edge.via_member_id));
  out.emplace_back("create_form", optional_int(edge.create_form));
  out.emplace_back(
      "status",
      Value::obj({
          {"completeness",
           Value::of(std::string(edge.partial != 0 ? "partial" : "complete"))},
          {"freshness", Value::of(freshness)},
          {"resolved", Value::of(true)},
          {"truncated", Value::of(truncated)},
      }));
  out.emplace_back("evidence", Value::obj({
                                   {"bounded", Value::of(true)},
                                   {"derived", Value::of(true)},
                               }));
  return Value::obj(std::move(out));
}

Value type_node_value(const TypeNode &type, const std::string &freshness,
                      bool truncated) {
  Object out;
  out.emplace_back("id", Value::of(type_id(type)));
  out.emplace_back("kind", Value::of(std::string("type")));
  out.emplace_back("type_kind", Value::of(std::to_string(type.kind)));
  out.emplace_back("spelling", Value::of(type.spelling));
  out.emplace_back("const", Value::of(type.is_const));
  out.emplace_back("volatile", Value::of(type.is_volatile));
  out.emplace_back("restrict", Value::of(type.is_restrict));
  out.emplace_back("status",
                   Value::obj({
                       {"completeness", Value::of(std::string("complete"))},
                       {"freshness", Value::of(freshness)},
                       {"resolved", Value::of(true)},
                       {"truncated", Value::of(truncated)},
                   }));
  out.emplace_back("evidence", Value::obj({
                                   {"bounded", Value::of(true)},
                                   {"type_key_hashed", Value::of(true)},
                               }));
  return Value::obj(std::move(out));
}

Value type_edge_value(const TypeEdge &edge, std::string_view source,
                      std::string_view target, const std::string &freshness,
                      bool truncated) {
  const std::string kind = type_edge_kind_name(edge.kind);
  Object out;
  out.emplace_back("id", Value::of("type-edge:v1:" + length_field(source) +
                                   length_field(kind) +
                                   length_field(std::to_string(edge.position)) +
                                   length_field(target)));
  out.emplace_back("source", Value::of(std::string(source)));
  out.emplace_back("target", Value::of(std::string(target)));
  out.emplace_back("kind", Value::of(kind));
  out.emplace_back("position", Value::of(edge.position));
  out.emplace_back("status",
                   Value::obj({
                       {"completeness", Value::of(std::string("complete"))},
                       {"freshness", Value::of(freshness)},
                       {"resolved", Value::of(true)},
                       {"truncated", Value::of(truncated)},
                   }));
  out.emplace_back("evidence", Value::obj({
                                   {"bounded", Value::of(true)},
                                   {"derived", Value::of(true)},
                               }));
  return Value::obj(std::move(out));
}

struct RootResolution {
  std::optional<graph::Sym> symbol;
  std::string status = "none";
  std::vector<graph::Sym> candidates;
};

bool matches_any(const std::vector<std::string> &allowed,
                 const std::string &value) {
  return std::ranges::find(allowed, value) != allowed.end();
}

// Progressive continuation (HSE-92): drop the first `offset` deterministically
// ordered candidates so a repeat request with a larger offset picks up
// exactly where the previous bounded slice left off.
template <typename T>
void skip_offset(std::vector<T> &ordered, int offset) {
  if (offset <= 0 || ordered.empty()) {
    return;
  }
  const auto count = std::min(static_cast<std::size_t>(offset), ordered.size());
  ordered.erase(ordered.begin(),
               ordered.begin() + static_cast<std::ptrdiff_t>(count));
}

void append_typed_facts(Storage &db, graph::GraphQuery &graph,
                        const std::vector<int64_t> &entity_ids,
                        const std::vector<int64_t> &type_ids, bool entity_view,
                        int node_budget, int edge_budget,
                        const std::string &freshness, bool &truncated,
                        bool &partial_facts,
                        const std::optional<std::string> &workspace,
                        const std::optional<std::vector<std::string>> &node_kinds,
                        int node_offset, bool &more_nodes_available,
                        Array &nodes, Array &edges) {
  if (entity_view) {
    std::map<int64_t, std::pair<graph::Sym, EntityNode>> available;
    std::vector<EntityEdge> candidate_edges;
    for (const int64_t id : entity_ids) {
      const auto entity = db.entity_node_by_id(id);
      const auto symbol = graph.get_by_id(id);
      if (!entity || !symbol) {
        continue;
      }
      available.emplace(id, std::make_pair(*symbol, *entity));
      for (const auto &edge : db.entity_edges_from(id)) {
        candidate_edges.push_back(edge);
        const auto target_entity = db.entity_node_by_id(edge.dst_id);
        const auto target_symbol = graph.get_by_id(edge.dst_id);
        if (target_entity && target_symbol) {
          available.emplace(edge.dst_id,
                            std::make_pair(*target_symbol, *target_entity));
        }
      }
    }
    std::vector<int64_t> ordered_ids;
    for (const auto &[id, value] : available) {
      if (node_kinds && !node_kinds->empty() &&
          !matches_any(*node_kinds, value.second.kind_name)) {
        continue;
      }
      ordered_ids.push_back(id);
    }
    std::ranges::sort(ordered_ids, [&](int64_t left, int64_t right) {
      return entity_id(available.at(left).first, available.at(left).second) <
             entity_id(available.at(right).first, available.at(right).second);
    });
    skip_offset(ordered_ids, node_offset);
    const std::size_t remaining =
        nodes.size() >= static_cast<std::size_t>(node_budget)
            ? 0
            : static_cast<std::size_t>(node_budget) - nodes.size();
    if (ordered_ids.size() > remaining) {
      ordered_ids.resize(remaining);
      truncated = true;
      more_nodes_available = true;
    }
    std::set<int64_t> selected;
    for (const int64_t id : ordered_ids) {
      selected.insert(id);
      nodes.push_back(entity_node_value(available.at(id).first,
                                        available.at(id).second, freshness,
                                        truncated, workspace));
    }
    std::ranges::sort(
        candidate_edges, [](const EntityEdge &left, const EntityEdge &right) {
          return std::tie(left.kind_name, left.src_id, left.dst_id,
                          left.via_member_id, left.create_form) <
                 std::tie(right.kind_name, right.src_id, right.dst_id,
                          right.via_member_id, right.create_form);
        });
    for (const auto &edge : candidate_edges) {
      if (!selected.contains(edge.src_id) || !selected.contains(edge.dst_id)) {
        continue;
      }
      if (edges.size() >= static_cast<std::size_t>(edge_budget)) {
        truncated = true;
        break;
      }
      const std::string source = entity_id(available.at(edge.src_id).first,
                                           available.at(edge.src_id).second);
      const std::string target = entity_id(available.at(edge.dst_id).first,
                                           available.at(edge.dst_id).second);
      partial_facts = partial_facts || edge.partial != 0;
      edges.push_back(
          entity_edge_value(edge, source, target, freshness, truncated));
    }
    return;
  }

  std::map<int64_t, TypeNode> available;
  std::vector<TypeEdge> candidate_edges;
  for (const int64_t id : type_ids) {
    if (const auto type = db.type_node_by_id(id)) {
      available.emplace(id, *type);
    }
    for (const auto &edge : db.type_edges_from(id)) {
      candidate_edges.push_back(edge);
      if (const auto type = db.type_node_by_id(edge.dst_id)) {
        available.emplace(edge.dst_id, *type);
      }
    }
  }
  std::vector<int64_t> ordered_ids;
  for (const auto &[id, type] : available) {
    if (node_kinds && !node_kinds->empty() &&
        !matches_any(*node_kinds, std::to_string(type.kind))) {
      continue;
    }
    ordered_ids.push_back(id);
  }
  std::ranges::sort(ordered_ids, [&](int64_t left, int64_t right) {
    return type_id(available.at(left)) < type_id(available.at(right));
  });
  skip_offset(ordered_ids, node_offset);
  const std::size_t remaining =
      nodes.size() >= static_cast<std::size_t>(node_budget)
          ? 0
          : static_cast<std::size_t>(node_budget) - nodes.size();
  if (ordered_ids.size() > remaining) {
    ordered_ids.resize(remaining);
    truncated = true;
    more_nodes_available = true;
  }
  std::set<int64_t> selected;
  for (const int64_t id : ordered_ids) {
    selected.insert(id);
    nodes.push_back(type_node_value(available.at(id), freshness, truncated));
  }
  std::ranges::sort(
      candidate_edges, [](const TypeEdge &left, const TypeEdge &right) {
        return std::tie(left.src_id, left.kind, left.position, left.dst_id) <
               std::tie(right.src_id, right.kind, right.position, right.dst_id);
      });
  for (const auto &edge : candidate_edges) {
    if (!selected.contains(edge.src_id) || !selected.contains(edge.dst_id)) {
      continue;
    }
    if (edges.size() >= static_cast<std::size_t>(edge_budget)) {
      truncated = true;
      break;
    }
    edges.push_back(type_edge_value(edge, type_id(available.at(edge.src_id)),
                                    type_id(available.at(edge.dst_id)),
                                    freshness, truncated));
  }
}

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
    // Auto-discovery must only pick genuine symbol<->symbol relations
    // (e.g. calls/uses), regardless of traversal direction: a relation whose
    // TARGET view is not Symbol (e.g. "of_type" -> Type) cannot be traversed
    // through query::in_()/out() in a plain symbol-view QueryPlan and fails
    // validation. Direction only selects in_() vs out(); it must not widen
    // which relations are eligible.
    for (const auto &relation : query::relation_catalog()) {
      if (relation.layer == query::View::Symbol &&
          relation.target_view == query::View::Symbol) {
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
                                       bool has_includes = false,
                                       bool entity_view = false,
                                       bool type_view = false) {
  std::set<std::string> facts;
  if (entity_view) {
    facts.insert("entities");
    facts.insert("entity_edges");
  } else if (type_view) {
    facts.insert("types");
    facts.insert("type_edges");
  } else {
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
  out.emplace_back("workspace",
                   Value::of(pathutil::basename(identity.workspace)));
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

// ---- HSE-92 live-explorer filters and continuation -------------------------

// Node-level filters that apply uniformly to the symbol/CXQ/QueryPlan/File
// path. Entity and type views only honor node_kinds (see append_typed_facts);
// file/component/repository/status facts do not have an equivalent meaning
// for structural entity/type facts.
bool passes_node_filters(Storage &db, const graph::Sym &sym,
                         const GraphViewRequest &request) {
  if (request.node_kinds && !request.node_kinds->empty() &&
      !matches_any(*request.node_kinds, sym.kind)) {
    return false;
  }
  if (request.files && !request.files->empty()) {
    const std::string path =
        sym.file ? redacted_path(*sym.file, request.workspace) : std::string();
    if (!matches_any(*request.files, path)) {
      return false;
    }
  }
  if (request.components && !request.components->empty()) {
    if (!sym.component || !matches_any(*request.components, *sym.component)) {
      return false;
    }
  }
  if (request.status_filter) {
    const std::string &status = *request.status_filter;
    if (status == "resolved" && !sym.resolved) {
      return false;
    }
    if (status == "unresolved" && sym.resolved) {
      return false;
    }
    if (status == "external" && !sym.external) {
      return false;
    }
    if (status == "internal" && sym.external) {
      return false;
    }
    if (status == "stub" && !sym.is_stub()) {
      return false;
    }
  }
  if (request.repositories && !request.repositories->empty()) {
    if (!sym.component) {
      return false;
    }
    const auto component = db.get_component_by_name(*sym.component);
    if (!component || !component->repository_id) {
      return false;
    }
    const auto repository = db.get_repository_by_id(*component->repository_id);
    if (!repository || !matches_any(*request.repositories, repository->name)) {
      return false;
    }
  }
  return true;
}

// Whether a bounded set of evidence sites is uniformly present (no site is
// config-conditional) versus config-varying. Mirrors the site.conditional
// fact already carried on every emitted symbol/include edge; introduces no
// new semantic claim beyond what is already indexed.
bool sites_are_conditional(const std::vector<graph::Site> &sites) {
  return std::ranges::any_of(
      sites, [](const graph::Site &site) { return site.conditional; });
}

bool include_sites_are_conditional(const std::vector<IncludeSite> &sites) {
  return std::ranges::any_of(sites, [](const IncludeSite &site) {
    return !site.cond_fingerprint.empty();
  });
}

bool passes_applicability_filter(const GraphViewRequest &request,
                                 bool conditional) {
  if (!request.applicability_filter) {
    return true;
  }
  if (*request.applicability_filter == "conditional") {
    return conditional;
  }
  if (*request.applicability_filter == "universal") {
    return !conditional;
  }
  return true;
}

// Continuation tokens page a strictly-repeated request over its own
// deterministically-ordered node/edge candidates. The token embeds the
// query identity of the request that produced it so a token from one
// normalized query can never be replayed against a different one -- paging
// is deterministic and request-scoped, never a hidden server-side cursor.
std::string encode_continuation(std::string_view query_identity,
                                int node_offset, int edge_offset) {
  return "cont:v1:" + length_field(query_identity) +
        std::to_string(node_offset) + "," + std::to_string(edge_offset);
}

struct ContinuationToken {
  std::string query_identity;
  int node_offset = 0;
  int edge_offset = 0;
};

std::optional<ContinuationToken> decode_continuation(std::string_view token) {
  constexpr std::string_view prefix = "cont:v1:";
  if (!token.starts_with(prefix)) {
    return std::nullopt;
  }
  std::size_t offset = prefix.size();
  const auto identity = length_value(token, offset);
  if (!identity) {
    return std::nullopt;
  }
  const std::size_t comma = token.find(',', offset);
  if (comma == std::string_view::npos) {
    return std::nullopt;
  }
  int node_offset_value = 0;
  int edge_offset_value = 0;
  const auto node_result = std::from_chars(
      token.data() + offset, token.data() + comma, node_offset_value);
  const auto edge_result =
      std::from_chars(token.data() + comma + 1, token.data() + token.size(),
                      edge_offset_value);
  if (node_result.ec != std::errc{} || node_result.ptr != token.data() + comma ||
      edge_result.ec != std::errc{} ||
      edge_result.ptr != token.data() + token.size() || node_offset_value < 0 ||
      edge_offset_value < 0) {
    return std::nullopt;
  }
  return ContinuationToken{.query_identity = std::string(*identity),
                           .node_offset = node_offset_value,
                           .edge_offset = edge_offset_value};
}

// Computes and embeds (or, if it would blow the byte budget, omits) the
// HSE-92 progressive-paging continuation token on an already-finalized
// GraphView `continuation` metadata object. Split out of build_graph_view()
// to keep that function within its complexity budget; see the call site for
// the invariants each parameter carries.
void apply_continuation_token(Value &continuation, Value &result,
                              const std::string &query_identity,
                              bool witness_view, bool more_nodes_available,
                              bool more_edges_available,
                              bool byte_trim_dropped_content, int node_offset,
                              int edge_offset, int delivered_node_count,
                              int delivered_edge_count, int byte_budget) {
  // Deterministic progressive paging: a token is only emitted when this
  // exact normalized query still has more nodes/edges beyond what was just
  // delivered. Bounded witness paths never page.
  const bool continuation_possible =
      !witness_view && (more_nodes_available || more_edges_available ||
                        byte_trim_dropped_content);
  Value token = Value::null();
  if (continuation_possible) {
    int next_node_offset = node_offset;
    int next_edge_offset = 0;
    if (more_nodes_available) {
      next_node_offset = node_offset + delivered_node_count;
      next_edge_offset = 0;
    } else {
      next_edge_offset = edge_offset + delivered_edge_count;
    }
    token = Value::of(encode_continuation(query_identity, next_node_offset,
                                          next_edge_offset));
  }
  const auto set_token = [&](const Value &value) {
    bool set = false;
    for (auto &[name, child] : continuation.o) {
      if (name == "token") {
        child = value;
        set = true;
      }
    }
    if (!set) {
      continuation.o.emplace_back("token", value);
    }
  };
  set_token(token);
  // A continuation token is a "nice to have" atop an already-truncated
  // response, never a reason to fail the whole request: if embedding it
  // would itself blow the byte budget, drop the token (the client can still
  // see `available: true` and retry with a larger byte limit) rather than
  // throwing GraphViewFailureKind::Oversized over a response that was
  // otherwise successfully trimmed to fit.
  if (token.t != Value::T::Null &&
      script_safe_size(json_out::dumps_indent2(result)) >
          static_cast<std::size_t>(byte_budget)) {
    set_token(Value::null());
  }
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
  std::vector<int64_t> entity_ids;
  std::vector<int64_t> type_ids;
  std::vector<graph::Sym> witness_nodes;
  std::vector<graph::Edge> witness_edges;
  bool entity_view = false;
  bool type_view = false;
  bool witness_view = false;
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
          "QueryPlan result must be a bounded symbol/entity node view with "
          "an "
          "id field",
          "remove select/count/view stages or select the supported id field");
    }
    entity_view = result.view == query::View::Entity;
    for (const auto &row : result.rows) {
      if (row.size() != result.fields.size() || row.empty() ||
          !std::holds_alternative<int64_t>(row.front())) {
        throw GraphViewError(
            GraphViewFailureKind::UnsupportedInput,
            "QueryPlan result has an unsupported row shape or id field",
            "return the validated node/entity view with its id field first");
      }
      const int64_t id = std::get<int64_t>(row.front());
      if (result.view == query::View::Entity) {
        if (!db.entity_node_by_id(id)) {
          throw GraphViewError(
              GraphViewFailureKind::UnsupportedInput,
              "QueryPlan entity result contains a non-entity identity",
              "return entity node ids from the validated entity view");
        }
        entity_ids.push_back(id);
      } else {
        add_symbol_by_id(id);
      }
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
    normalized_input = portable_file_id(db, file.id);
  };

  const auto record_symbol_identity = [&] {
    normalized_input = resolved_symbol_identity();
  };

  const auto record_plan_identity = [&] {
    std::vector<std::string> resolved_ids;
    if (entity_view) {
      for (const int64_t id : entity_ids) {
        const auto entity = db.entity_node_by_id(id);
        const auto symbol = graph.get_by_id(id);
        if (entity && symbol) {
          resolved_ids.push_back(entity_id(*symbol, *entity));
        }
      }
    } else {
      for (const auto &symbol : input_symbols) {
        resolved_ids.push_back(portable_id(symbol));
      }
    }
    std::ranges::sort(resolved_ids);
    normalized_input =
        "plan:v1:" + normalized_plan_json.value_or("") + ":resolved:";
    for (const auto &id : resolved_ids) {
      normalized_input += length_field(id);
    }
  };

  Array nodes;
  Array edges;
  bool truncated = false;
  bool evidence_truncated = false;
  bool partial_facts = false;
  int sites_used = 0;
  // HSE-92: whether more nodes/edges of THIS SAME normalized query exist
  // beyond the page just delivered (independent of `truncated`, which also
  // covers byte-budget overflow that a node/edge continuation token cannot
  // resolve).
  bool more_nodes_available = false;
  bool more_edges_available = false;
  int delivered_node_count = 0;
  int delivered_edge_count = 0;

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
      nodes.push_back(file_node_value(db, file_id, path, resolved, freshness,
                                      truncated, request.workspace));
    };
    append_file(input_file->id, source_path, true);
    int sites_remaining = site_budget - sites_used;
    for (const auto &include : include_edges) {
      const std::string target_path =
          include.dst_file_id
              ? include_file_path(*include.dst_file_id, include.dst_path)
              : include.dst_path;
      const std::string target_id = portable_file_id(
          db, include.dst_file_id.value_or(input_file->id), target_path);
      append_file(include.dst_file_id.value_or(input_file->id), target_path,
                  include.dst_file_id.has_value());
      const bool target_emitted = emitted_files.contains(target_id);
      if (edges.size() >= static_cast<std::size_t>(edge_budget)) {
        truncated = true;
        break;
      }
      auto sites = db.include_sites_for(include.id);
      std::ranges::sort(
          sites, [](const IncludeSite &left, const IncludeSite &right) {
            return std::tie(left.line, left.col, left.begin_offset) <
                   std::tie(right.line, right.col, right.begin_offset);
          });
      if (!passes_applicability_filter(request,
                                       include_sites_are_conditional(sites))) {
        continue;
      }
      const std::size_t retained_limit =
          static_cast<std::size_t>(std::max(0, sites_remaining));
      const bool sites_truncated = sites.size() > retained_limit;
      if (sites.size() > retained_limit) {
        sites.resize(retained_limit);
      }
      sites_used += static_cast<int>(sites.size());
      sites_remaining -= static_cast<int>(sites.size());
      evidence_truncated = evidence_truncated || sites_truncated;
      if (target_emitted) {
        edges.push_back(include_edge_value(db, include, source_path, freshness,
                                           truncated, sites_truncated, sites,
                                           request.workspace));
      }
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
    entity_view = true;
    resolution.status = entity_ids.empty() ? "unknown" : "exact_entity";
    if (!entity_ids.empty()) {
      resolution.symbol = graph.get_by_id(entity_ids.front());
    }
    break;
  case GraphInputKind::Type: {
    type_ids = db.type_ids_reaching(input.value);
    type_view = true;
    resolution.status = type_ids.empty() ? "unknown" : "exact_type";
    normalized_input = "types:v1:";
    for (const int64_t id : type_ids) {
      if (const auto type = db.type_node_by_id(id)) {
        normalized_input += hex_field(type->type_key);
      }
    }
    break;
  }
  case GraphInputKind::Cxq: {
    try {
      collect_plan(query::parse_cxq(input.value));
    } catch (const query::PlanError &error) {
      throw GraphViewError(GraphViewFailureKind::InvalidInput, error.what(),
                           "provide a valid bounded CXQ QueryPlan");
    }
    record_plan_identity();
    const bool plan_empty =
        entity_view ? entity_ids.empty() : input_symbols.empty();
    resolution.status = plan_empty ? "unknown" : "exact_plan";
    if (entity_view && !entity_ids.empty()) {
      resolution.symbol = graph.get_by_id(entity_ids.front());
    } else if (!input_symbols.empty()) {
      resolution.symbol = input_symbols.front();
    }
    break;
  }
  case GraphInputKind::QueryPlan: {
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
    const bool plan_empty =
        entity_view ? entity_ids.empty() : input_symbols.empty();
    resolution.status = plan_empty ? "unknown" : "exact_plan";
    if (entity_view && !entity_ids.empty()) {
      resolution.symbol = graph.get_by_id(entity_ids.front());
    } else if (!input_symbols.empty()) {
      resolution.symbol = input_symbols.front();
    }
    break;
  }
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
        witness_nodes = *path;
        witness_view = true;
        input_symbols = witness_nodes;
        const auto kind_ids = graph::GraphQuery::kind_ids(request.edge_kinds);
        for (std::size_t index = 1; index < witness_nodes.size(); ++index) {
          const graph::Sym &from = witness_nodes[index - 1];
          const graph::Sym &to = witness_nodes[index];
          auto adjacent = graph.edges(from.id, request.direction, kind_ids,
                                      edge_budget + 1, false);
          std::vector<graph::Edge> matching;
          for (const auto &edge : adjacent) {
            const bool forward = edge.src_id == from.id && edge.dst_id == to.id;
            const bool reverse = edge.src_id == to.id && edge.dst_id == from.id;
            if (forward || reverse) {
              matching.push_back(edge);
            }
          }
          std::ranges::sort(
              matching, [&](const graph::Edge &left, const graph::Edge &right) {
                return portable_edge_id(from, left, to) <
                       portable_edge_id(from, right, to);
              });
          if (matching.empty()) {
            throw GraphViewError(
                GraphViewFailureKind::UnsupportedInput,
                "bounded path has no materialized witness edge",
                "choose a path whose consecutive steps are indexed");
          }
          witness_edges.push_back(matching.front());
        }
        normalized_input = "path:v1:";
        for (std::size_t index = 0; index < witness_nodes.size(); ++index) {
          normalized_input += length_field(portable_id(witness_nodes[index]));
          if (index < witness_edges.size()) {
            const auto &edge = witness_edges[index];
            normalized_input += length_field(portable_edge_id(
                witness_nodes[index], edge, witness_nodes[index + 1]));
          }
        }
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

  int node_offset = 0;
  int edge_offset = 0;
  if (request.continuation) {
    const auto token = decode_continuation(*request.continuation);
    if (!token || token->query_identity != query_identity) {
      throw GraphViewError(
          GraphViewFailureKind::InvalidInput,
          "continuation token does not match the current normalized query",
          "reissue the base request and use the continuation token it "
          "returns");
    }
    node_offset = token->node_offset;
    edge_offset = token->edge_offset;
  }

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
  index.emplace_back("index_config_redacted", Value::of(true));
  index.emplace_back("index_config_fingerprint",
                     optional_string(identity.index_config_fingerprint));
  index.emplace_back("freshness", Value::of(identity.freshness));
  metadata.emplace_back("index", Value::obj(std::move(index)));
  metadata.emplace_back(
      "identity",
      identity_value(identity, fact_sets_for(input.kind, false, false, false,
                                             entity_view, type_view)));

  if (!input_symbols.empty() || input_file || entity_view || type_view) {
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
    if (query_result && !entity_view && !type_view) {
      truncated =
          query_result->truncated ||
          query_result->rows.size() > static_cast<std::size_t>(node_budget);
    }

    if (entity_view || type_view) {
      append_typed_facts(db, graph, entity_ids, type_ids, entity_view,
                         node_budget, edge_budget, freshness, truncated,
                         partial_facts, request.workspace, request.node_kinds,
                         node_offset, more_nodes_available, nodes, edges);
    } else {
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
      if (witness_view) {
        ordered_nodes = witness_nodes;
      } else {
        ordered_nodes.reserve(symbols_by_id.size());
        for (const auto &[id, symbol] : symbols_by_id) {
          (void)id;
          if (passes_node_filters(db, symbol, request)) {
            ordered_nodes.push_back(symbol);
          }
        }
        std::ranges::sort(ordered_nodes,
                          [](const graph::Sym &a, const graph::Sym &b) {
                            return portable_id(a) < portable_id(b);
                          });
        skip_offset(ordered_nodes, node_offset);
      }
      const std::size_t remaining_node_budget =
          nodes.size() >= static_cast<std::size_t>(node_budget)
              ? 0
              : static_cast<std::size_t>(node_budget) - nodes.size();
      if (ordered_nodes.size() > remaining_node_budget) {
        ordered_nodes.resize(remaining_node_budget);
        truncated = true;
        more_nodes_available = !witness_view;
      }
      delivered_node_count = static_cast<int>(ordered_nodes.size());
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

      std::vector<graph::Edge> selected_edges;
      if (witness_view) {
        selected_edges = witness_edges;
        if (selected_edges.size() + 1 > ordered_nodes.size()) {
          selected_edges.resize(
              ordered_nodes.empty() ? 0 : ordered_nodes.size() - 1);
          truncated = true;
        }
        if (selected_edges.size() >
            static_cast<std::size_t>(
                std::max(0, edge_budget - static_cast<int>(edges.size())))) {
          selected_edges.resize(static_cast<std::size_t>(
              std::max(0, edge_budget - static_cast<int>(edges.size()))));
          truncated = true;
        }
      } else {
        std::map<std::string, graph::Edge> by_key;
        const auto kind_ids = graph::GraphQuery::kind_ids(request.edge_kinds);
        for (const auto &symbol : ordered_nodes) {
          const auto adjacent = graph.edges(symbol.id, request.direction,
                                            kind_ids, edge_budget + 1, false);
          for (const auto &edge : adjacent) {
            if (!selected_ids.contains(edge.src_id) ||
                !selected_ids.contains(edge.dst_id)) {
              continue;
            }
            const auto source = symbols_by_id.find(edge.src_id);
            const auto target = symbols_by_id.find(edge.dst_id);
            if (source == symbols_by_id.end() ||
                target == symbols_by_id.end()) {
              continue;
            }
            by_key.emplace(
                portable_edge_id(source->second, edge, target->second), edge);
          }
        }
        std::vector<graph::Edge> ordered_edges;
        ordered_edges.reserve(by_key.size());
        for (auto &[key, edge] : by_key) {
          (void)key;
          ordered_edges.push_back(edge);
        }
        skip_offset(ordered_edges, edge_offset);
        if (ordered_edges.size() >
            static_cast<std::size_t>(
                std::max(0, edge_budget - static_cast<int>(edges.size())))) {
          truncated = true;
          more_edges_available = true;
        }
        for (const auto &edge : ordered_edges) {
          if (edges.size() >= static_cast<std::size_t>(edge_budget)) {
            truncated = true;
            break;
          }
          selected_edges.push_back(edge);
        }
      }
      const std::size_t symbol_edges_before = edges.size();
      int sites_remaining = std::max(0, site_budget - sites_used);
      for (const auto &edge : selected_edges) {
        if (edges.size() >= static_cast<std::size_t>(edge_budget)) {
          truncated = true;
          break;
        }
        const auto source = symbols_by_id.find(edge.src_id);
        const auto target = symbols_by_id.find(edge.dst_id);
        if (source == symbols_by_id.end() || target == symbols_by_id.end()) {
          continue;
        }
        const int fetch_limit = sites_remaining + 1;
        auto sites = graph.sites(edge.edge_id, fetch_limit);
        std::ranges::sort(sites,
                          [&](const graph::Site &a, const graph::Site &b) {
                            return site_sort_key(a, request.workspace) <
                                   site_sort_key(b, request.workspace);
                          });
        if (!passes_applicability_filter(request, sites_are_conditional(sites))) {
          continue;
        }
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
      delivered_edge_count =
          static_cast<int>(edges.size() - symbol_edges_before);
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
      resolution.symbol.has_value() || resolution.status == "exact_file" ||
      (entity_view && !entity_ids.empty()) || (type_view && !type_ids.empty());
  const bool initial_unknown = identity.freshness != "current" ||
                               !db.graph_resolved() || !input_resolved;
  std::string graph_status = "complete";
  if (initial_unknown) {
    graph_status = "unknown";
  } else if (truncated || partial_facts) {
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
  if (partial_facts) {
    markers.push_back(Value::of(std::string("partial")));
  }
  metadata.emplace_back("status", Value::of(graph_status));
  metadata.emplace_back("markers", Value::arr(markers));

  const auto fact_sets =
      fact_sets_for(input.kind, !edges.empty(), sites_used > 0,
                    has_include_facts, entity_view, type_view);
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
    r.emplace_back("input_redacted", Value::of(true));
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
  bool byte_trim_dropped_content = false;

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
  // HSE-92's continuation token (added to metadata further below, once
  // per-page delivery counts are final) costs a small, tightly-bounded
  // number of bytes: a fixed "cont:v1:" prefix plus a 64-hex-char
  // query-identity hash plus two small integers -- comfortably under 256
  // bytes for any request. Trim to `trim_budget` (byte_budget minus that
  // reserve) rather than the full byte_budget so the token almost always
  // has room to land inside the real budget; the unmodified final check
  // below still throws Oversized in the genuine can't-fit case instead of
  // silently violating byte_budget.
  constexpr std::size_t kContinuationReserve = 256;
  const std::size_t trim_budget =
      std::cmp_greater(byte_budget, kContinuationReserve)
          ? static_cast<std::size_t>(byte_budget) - kContinuationReserve
          : 1;
  if (script_safe_size(json_out::dumps_indent2(result)) > trim_budget) {
    byte_truncated = true;
    mark_byte_truncated();
    while (script_safe_size(json_out::dumps_indent2(result)) > trim_budget &&
           (member(result, "edges") != nullptr &&
            !member(result, "edges")->a.empty())) {
      member(result, "edges")->a.pop_back();
      byte_trim_dropped_content = true;
      if (delivered_edge_count > 0) {
        --delivered_edge_count;
      }
    }
    while (script_safe_size(json_out::dumps_indent2(result)) > trim_budget &&
           (member(result, "nodes") != nullptr &&
            !member(result, "nodes")->a.empty())) {
      member(result, "nodes")->a.pop_back();
      byte_trim_dropped_content = true;
      if (delivered_node_count > 0) {
        --delivered_node_count;
      }
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
  const bool final_partial =
      std::ranges::any_of(member(result, "edges")->a, [](const Value &edge) {
        const Value *partial = member(edge, "partial");
        return partial != nullptr && partial->t == Value::T::Int &&
               partial->i != 0;
      });
  const bool unknown = identity.freshness != "current" ||
                       !db.graph_resolved() || !input_resolved;
  std::string final_status = "complete";
  if (unknown) {
    final_status = "unknown";
  } else if (final_truncated || final_partial) {
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
  if (final_partial) {
    final_markers.push_back(Value::of(std::string("partial")));
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
      apply_continuation_token(*continuation, result, query_identity,
                               witness_view, more_nodes_available,
                               more_edges_available, byte_trim_dropped_content,
                               node_offset, edge_offset, delivered_node_count,
                               delivered_edge_count, byte_budget);
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
      has_include_facts, entity_view, type_view);
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

json_out::Value
search_candidates(Storage &db, const std::string &text,
                  const std::optional<std::string> &node_kind,
                  const std::optional<std::string> &workspace, int limit) {
  const int bounded_limit = std::clamp(limit, 1, 500);
  query::SqliteQueryReadAdapter graph_read(db);
  graph::GraphQuery graph(graph_read, "<ui-search>");
  auto matches = graph.find(text, node_kind, bounded_limit + 1);
  const bool truncated =
      matches.size() > static_cast<std::size_t>(bounded_limit);
  if (truncated) {
    matches.resize(static_cast<std::size_t>(bounded_limit));
  }
  std::ranges::sort(matches, [](const graph::Sym &a, const graph::Sym &b) {
    return portable_id(a) < portable_id(b);
  });
  json_out::Array results;
  for (const auto &sym : matches) {
    json_out::Object item;
    item.emplace_back("id", json_out::Value::of(portable_id(sym)));
    item.emplace_back("usr", json_out::Value::of(sym.usr));
    item.emplace_back(
        "name", json_out::Value::of(sym.name.empty() ? sym.usr : sym.name));
    item.emplace_back("kind", json_out::Value::of(sym.kind));
    item.emplace_back("location", json_out::Value::of(location(sym, workspace)));
    item.emplace_back("component", sym.component
                                       ? json_out::Value::of(*sym.component)
                                       : json_out::Value::null());
    item.emplace_back(
        "status",
        json_out::Value::obj({
            {"resolved", json_out::Value::of(sym.resolved)},
            {"external", json_out::Value::of(sym.external)},
            {"stub", json_out::Value::of(sym.is_stub())},
        }));
    results.push_back(json_out::Value::obj(std::move(item)));
  }
  json_out::Object out;
  out.emplace_back("schema",
                   json_out::Value::of(std::string("cidx.graph-view.search.v1")));
  out.emplace_back("query", json_out::Value::of(text));
  out.emplace_back("truncated", json_out::Value::of(truncated));
  out.emplace_back("matches", json_out::Value::arr(std::move(results)));
  return json_out::Value::obj(std::move(out));
}

json_out::Value load_edge_evidence(Storage &db, const std::string &edge_id,
                                   const std::optional<std::string> &workspace,
                                   int site_offset, int site_limit) {
  const auto edge_ref = parse_portable_edge_id(edge_id);
  if (!edge_ref) {
    throw GraphViewError(
        GraphViewFailureKind::InvalidInput,
        "edge id is not a recognized portable identity",
        "pass an edge id copied from a prior GraphView response");
  }
  const auto source_ref = parse_portable_id(edge_ref->source);
  const auto target_ref = parse_portable_id(edge_ref->target);
  if (!source_ref || !target_ref) {
    throw GraphViewError(
        GraphViewFailureKind::InvalidInput,
        "edge id endpoints are not recognized portable identities",
        "pass an edge id copied from a prior GraphView response");
  }
  query::SqliteQueryReadAdapter graph_read(db);
  graph::GraphQuery graph(graph_read, "<ui-evidence>");
  const auto sources = portable_matches(db, graph, *source_ref);
  const auto targets = portable_matches(db, graph, *target_ref);
  if (sources.size() != 1 || targets.size() != 1) {
    throw GraphViewError(
        GraphViewFailureKind::UnknownIdentity,
        "edge endpoints no longer resolve to a unique identity",
        "reissue the originating GraphView request");
  }
  const graph::Sym &source = sources.front();
  const graph::Sym &target = targets.front();
  const auto kind_ids =
      graph::GraphQuery::kind_ids(std::vector<std::string>{edge_ref->kind});
  const auto adjacent = graph.edges(source.id, "out", kind_ids, 2000, false);
  const graph::Edge *matched = nullptr;
  for (const auto &candidate : adjacent) {
    if (candidate.src_id == source.id && candidate.dst_id == target.id &&
        candidate.kind == edge_ref->kind) {
      matched = &candidate;
      break;
    }
  }
  if (matched == nullptr) {
    throw GraphViewError(GraphViewFailureKind::UnknownIdentity,
                        "edge no longer exists between the given endpoints",
                        "reissue the originating GraphView request");
  }
  const int bounded_offset = std::max(0, site_offset);
  const int bounded_limit = std::clamp(site_limit, 1, 5000);
  auto sites = graph.sites(matched->edge_id, bounded_offset + bounded_limit + 1);
  std::ranges::sort(sites, [&](const graph::Site &a, const graph::Site &b) {
    return site_sort_key(a, workspace) < site_sort_key(b, workspace);
  });
  skip_offset(sites, bounded_offset);
  const bool truncated =
      sites.size() > static_cast<std::size_t>(bounded_limit);
  if (truncated) {
    sites.resize(static_cast<std::size_t>(bounded_limit));
  }
  json_out::Array site_values;
  for (const auto &site : sites) {
    site_values.push_back(site_value(site, workspace));
  }
  json_out::Object out;
  out.emplace_back(
      "schema", json_out::Value::of(std::string("cidx.graph-view.evidence.v1")));
  out.emplace_back("edge_id", json_out::Value::of(edge_id));
  out.emplace_back("truncated", json_out::Value::of(truncated));
  out.emplace_back("sites", json_out::Value::arr(std::move(site_values)));
  return json_out::Value::obj(std::move(out));
}

} // namespace cidx::ui
