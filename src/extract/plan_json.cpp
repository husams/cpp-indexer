#include "extract/plan_json.hpp"

#include <cctype>
#include <map>
#include <memory>
#include <sstream>
#include <utility>
#include <variant>
#include <vector>

namespace cidx::extract {

PlanParseError::PlanParseError(const std::string &message, std::size_t offset)
    : std::runtime_error(message + " (offset " + std::to_string(offset) + ")"),
      offset_(offset) {}

namespace {

// ---------------------------------------------------------------------
// A minimal, self-contained JSON value tree scoped to this file. Objects
// keep field insertion order (a vector of pairs, not a map) so encoding is
// deterministic without a separate sort step, and so "unknown field" checks
// during decode can report the exact offending key.
// ---------------------------------------------------------------------
struct JVal;
using JArray = std::vector<JVal>;
using JObject = std::vector<std::pair<std::string, JVal>>;

struct JVal {
  std::variant<std::monostate, bool, std::int64_t, std::string, JArray, JObject>
      v;

  JVal() = default;
  JVal(bool b) : v(b) {}
  JVal(std::int64_t n) : v(n) {}
  JVal(std::string s) : v(std::move(s)) {}
  JVal(JArray a) : v(std::move(a)) {}
  JVal(JObject o) : v(std::move(o)) {}

  [[nodiscard]] bool is_object() const {
    return std::holds_alternative<JObject>(v);
  }
  [[nodiscard]] bool is_array() const {
    return std::holds_alternative<JArray>(v);
  }
  [[nodiscard]] bool is_string() const {
    return std::holds_alternative<std::string>(v);
  }
  [[nodiscard]] bool is_int() const {
    return std::holds_alternative<std::int64_t>(v);
  }
  [[nodiscard]] bool is_bool() const { return std::holds_alternative<bool>(v); }

  [[nodiscard]] const JObject &as_object() const {
    return std::get<JObject>(v);
  }
  [[nodiscard]] const JArray &as_array() const { return std::get<JArray>(v); }
  [[nodiscard]] const std::string &as_string() const {
    return std::get<std::string>(v);
  }
  [[nodiscard]] std::int64_t as_int() const {
    return std::get<std::int64_t>(v);
  }
  [[nodiscard]] bool as_bool() const { return std::get<bool>(v); }
};

void set_field(JObject &obj, std::string key, JVal value) {
  obj.emplace_back(std::move(key), std::move(value));
}

const JVal *find_field(const JObject &obj, std::string_view key) {
  for (const auto &[k, val] : obj) {
    if (k == key) {
      return &val;
    }
  }
  return nullptr;
}

void escape_into(const std::string &s, std::string &out) {
  out.push_back('"');
  for (unsigned char c : s) {
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
    default:
      if (c < 0x20) {
        std::ostringstream hex;
        hex << "\\u" << std::hex << std::uppercase;
        hex.width(4);
        hex.fill('0');
        hex << static_cast<int>(c);
        out += hex.str();
      } else {
        out.push_back(static_cast<char>(c));
      }
    }
  }
  out.push_back('"');
}

void dump_into(const JVal &val, std::string &out) {
  if (std::holds_alternative<std::monostate>(val.v)) {
    out += "null";
  } else if (val.is_bool()) {
    out += val.as_bool() ? "true" : "false";
  } else if (val.is_int()) {
    out += std::to_string(val.as_int());
  } else if (val.is_string()) {
    escape_into(val.as_string(), out);
  } else if (val.is_array()) {
    out.push_back('[');
    bool first = true;
    for (const auto &item : val.as_array()) {
      if (!first) {
        out.push_back(',');
      }
      first = false;
      dump_into(item, out);
    }
    out.push_back(']');
  } else if (val.is_object()) {
    out.push_back('{');
    bool first = true;
    for (const auto &[key, value] : val.as_object()) {
      if (!first) {
        out.push_back(',');
      }
      first = false;
      escape_into(key, out);
      out.push_back(':');
      dump_into(value, out);
    }
    out.push_back('}');
  }
}

std::string dump(const JVal &val) {
  std::string out;
  dump_into(val, out);
  return out;
}

// --- Parsing -----------------------------------------------------------

class JsonParser {
public:
  explicit JsonParser(std::string_view text) : text_(text) {}

  JVal parse_document() {
    JVal value = parse_value(0);
    skip_ws();
    if (pos_ != text_.size()) {
      fail("trailing content after JSON document");
    }
    return value;
  }

private:
  std::string_view text_;
  std::size_t pos_ = 0;

  [[noreturn]] void fail(const std::string &message) const {
    throw PlanParseError(message, pos_);
  }

  void skip_ws() {
    while (pos_ < text_.size()) {
      char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  char peek() {
    if (pos_ >= text_.size()) {
      fail("unexpected end of JSON document");
    }
    return text_[pos_];
  }

  void expect(char c) {
    if (pos_ >= text_.size() || text_[pos_] != c) {
      fail(std::string("expected '") + c + "'");
    }
    ++pos_;
  }

  bool consume_literal(std::string_view lit) {
    if (text_.substr(pos_, lit.size()) == lit) {
      pos_ += lit.size();
      return true;
    }
    return false;
  }

  JVal parse_value(int depth) {
    if (depth > kExtractionPlanMaxJsonDepth) {
      fail("JSON document exceeds the maximum allowed nesting");
    }
    skip_ws();
    char c = peek();
    if (c == '{') {
      return parse_object(depth);
    }
    if (c == '[') {
      return parse_array(depth);
    }
    if (c == '"') {
      return {parse_string()};
    }
    if (c == 't' && consume_literal("true")) {
      return {true};
    }
    if (c == 'f' && consume_literal("false")) {
      return {false};
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      return {parse_int()};
    }
    fail("unexpected character in JSON document");
  }

  JVal parse_object(int depth) {
    expect('{');
    JObject obj;
    skip_ws();
    if (peek() == '}') {
      ++pos_;
      return {std::move(obj)};
    }
    while (true) {
      skip_ws();
      if (peek() != '"') {
        fail("expected an object key");
      }
      std::string key = parse_string();
      for (const auto &existing : obj) {
        if (existing.first == key) {
          fail("duplicate object key: " + key);
        }
      }
      skip_ws();
      expect(':');
      JVal value = parse_value(depth + 1);
      obj.emplace_back(std::move(key), std::move(value));
      skip_ws();
      char c = peek();
      if (c == ',') {
        ++pos_;
        continue;
      }
      if (c == '}') {
        ++pos_;
        break;
      }
      fail("expected ',' or '}' in object");
    }
    return {std::move(obj)};
  }

  JVal parse_array(int depth) {
    expect('[');
    JArray arr;
    skip_ws();
    if (peek() == ']') {
      ++pos_;
      return {std::move(arr)};
    }
    while (true) {
      arr.push_back(parse_value(depth + 1));
      skip_ws();
      char c = peek();
      if (c == ',') {
        ++pos_;
        continue;
      }
      if (c == ']') {
        ++pos_;
        break;
      }
      fail("expected ',' or ']' in array");
    }
    return {std::move(arr)};
  }

  std::string parse_string() {
    expect('"');
    std::string out;
    while (true) {
      if (pos_ >= text_.size()) {
        fail("unterminated string literal");
      }
      char c = text_[pos_++];
      if (c == '"') {
        break;
      }
      if (c == '\\') {
        if (pos_ >= text_.size()) {
          fail("unterminated escape sequence");
        }
        char esc = text_[pos_++];
        switch (esc) {
        case '"':
          out.push_back('"');
          break;
        case '\\':
          out.push_back('\\');
          break;
        case '/':
          out.push_back('/');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'u': {
          if (pos_ + 4 > text_.size()) {
            fail("truncated \\u escape");
          }
          unsigned code = 0;
          for (int i = 0; i < 4; ++i) {
            char hc = text_[pos_++];
            code <<= 4;
            if (hc >= '0' && hc <= '9') {
              code |= static_cast<unsigned>(hc - '0');
            } else if (hc >= 'a' && hc <= 'f') {
              code |= static_cast<unsigned>(hc - 'a' + 10);
            } else if (hc >= 'A' && hc <= 'F') {
              code |= static_cast<unsigned>(hc - 'A' + 10);
            } else {
              fail("invalid \\u escape digit");
            }
          }
          // Encode as UTF-8. Surrogate pairs are rejected (out of scope for
          // plan text, which is expected to be ASCII-safe identifiers and
          // source snippets); a lone code point covers every case CIDX
          // itself ever emits.
          if (code <= 0x7F) {
            out.push_back(static_cast<char>(code));
          } else if (code <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
          } else {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
          }
          break;
        }
        default:
          fail("invalid escape sequence");
        }
      } else if (static_cast<unsigned char>(c) < 0x20) {
        fail("control character in string literal");
      } else {
        out.push_back(c);
      }
    }
    return out;
  }

  std::int64_t parse_int() {
    std::size_t start = pos_;
    if (peek() == '-') {
      ++pos_;
    }
    if (pos_ >= text_.size() ||
        std::isdigit(static_cast<unsigned char>(text_[pos_])) == 0) {
      fail("invalid number literal");
    }
    while (pos_ < text_.size() &&
           std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0) {
      ++pos_;
    }
    if (pos_ < text_.size() &&
        (text_[pos_] == '.' || text_[pos_] == 'e' || text_[pos_] == 'E')) {
      fail("floating-point numbers are not part of the ExtractionPlan grammar");
    }
    std::string token(text_.substr(start, pos_ - start));
    try {
      return std::stoll(token);
    } catch (const std::exception &) {
      fail("number literal out of range");
    }
  }
};

// --- ExtractionPlan <-> JVal --------------------------------------------

std::string require_string(const JObject &obj, std::string_view key,
                           std::string_view where) {
  const JVal *field = find_field(obj, key);
  if (field == nullptr || !field->is_string()) {
    throw PlanParseError("missing or non-string field '" + std::string(key) +
                             "' in " + std::string(where),
                         0);
  }
  return field->as_string();
}

std::int64_t require_int(const JObject &obj, std::string_view key,
                         std::string_view where) {
  const JVal *field = find_field(obj, key);
  if (field == nullptr || !field->is_int()) {
    throw PlanParseError("missing or non-integer field '" + std::string(key) +
                             "' in " + std::string(where),
                         0);
  }
  return field->as_int();
}

bool optional_bool(const JObject &obj, std::string_view key, bool fallback) {
  const JVal *field = find_field(obj, key);
  if (field == nullptr) {
    return fallback;
  }
  if (!field->is_bool()) {
    throw PlanParseError("field '" + std::string(key) + "' must be a boolean",
                         0);
  }
  return field->as_bool();
}

const JArray &require_array(const JObject &obj, std::string_view key,
                            std::string_view where) {
  const JVal *field = find_field(obj, key);
  if (field == nullptr || !field->is_array()) {
    throw PlanParseError("missing or non-array field '" + std::string(key) +
                             "' in " + std::string(where),
                         0);
  }
  return field->as_array();
}

void reject_unknown_fields(const JObject &obj,
                           const std::vector<std::string> &allowed,
                           std::string_view where) {
  for (const auto &[key, value] : obj) {
    (void)value;
    bool found = false;
    for (const auto &name : allowed) {
      if (name == key) {
        found = true;
        break;
      }
    }
    if (!found) {
      throw PlanParseError(
          "unknown field '" + key + "' in " + std::string(where), 0);
    }
  }
}

TraversalMode parse_traversal(const std::string &text) {
  if (text == "as_is") {
    return TraversalMode::as_is;
  }
  if (text == "ignore_unless_spelled") {
    return TraversalMode::ignore_unless_spelled;
  }
  throw PlanParseError("unknown traversal mode: " + text, 0);
}

PlanScope parse_scope(const std::string &text) {
  if (text == "main_file") {
    return PlanScope::main_file;
  }
  if (text == "translation_unit") {
    return PlanScope::translation_unit;
  }
  if (text == "workspace") {
    return PlanScope::workspace;
  }
  throw PlanParseError("unknown scope: " + text, 0);
}

DeclaredCompleteness parse_completeness(const std::string &text) {
  if (text == "complete") {
    return DeclaredCompleteness::complete;
  }
  if (text == "partial") {
    return DeclaredCompleteness::partial;
  }
  if (text == "unknown_capable") {
    return DeclaredCompleteness::unknown_capable;
  }
  throw PlanParseError("unknown declared completeness: " + text, 0);
}

EndpointDomain parse_domain(const std::string &text) {
  if (text == "declaration") {
    return EndpointDomain::declaration;
  }
  if (text == "expression") {
    return EndpointDomain::expression;
  }
  if (text == "type") {
    return EndpointDomain::type;
  }
  if (text == "custom_node") {
    return EndpointDomain::custom_node;
  }
  throw PlanParseError("unknown endpoint domain: " + text, 0);
}

IdentityKind parse_identity_kind(const std::string &text) {
  if (text == "usr") {
    return IdentityKind::usr;
  }
  if (text == "source_anchor") {
    return IdentityKind::source_anchor;
  }
  if (text == "owner_position") {
    return IdentityKind::owner_position;
  }
  if (text == "type_key") {
    return IdentityKind::type_key;
  }
  if (text == "composed") {
    return IdentityKind::composed;
  }
  throw PlanParseError("unknown identity kind: " + text, 0);
}

JVal identity_to_json(const IdentityRecipe &identity) {
  JObject obj;
  set_field(obj, "kind", JVal(to_string(identity.kind)));
  JArray components;
  for (const auto &component : identity.components) {
    components.emplace_back(component);
  }
  set_field(obj, "components", JVal(std::move(components)));
  return {std::move(obj)};
}

IdentityRecipe identity_from_json(const JVal &value) {
  if (!value.is_object()) {
    throw PlanParseError("identity must be an object", 0);
  }
  const JObject &obj = value.as_object();
  reject_unknown_fields(obj, {"kind", "components"}, "identity");
  IdentityRecipe identity;
  identity.kind = parse_identity_kind(require_string(obj, "kind", "identity"));
  for (const auto &item : require_array(obj, "components", "identity")) {
    if (!item.is_string()) {
      throw PlanParseError("identity components must be strings", 0);
    }
    identity.components.push_back(item.as_string());
  }
  return identity;
}

JVal emit_to_json(const EmitOperation &emit) {
  JObject obj;
  if (emit.node) {
    JObject node;
    set_field(node, "namespace", JVal(emit.node->namespace_name));
    set_field(node, "node_kind", JVal(emit.node->node_kind));
    set_field(node, "binding", JVal(emit.node->binding));
    set_field(node, "identity", identity_to_json(emit.node->identity));
    set_field(obj, "node", JVal(std::move(node)));
  }
  if (emit.relation) {
    JObject relation;
    set_field(relation, "namespace", JVal(emit.relation->namespace_name));
    set_field(relation, "relation_kind", JVal(emit.relation->relation_kind));
    set_field(relation, "from_binding", JVal(emit.relation->from_binding));
    set_field(relation, "to_binding", JVal(emit.relation->to_binding));
    set_field(relation, "with_evidence", JVal(emit.relation->with_evidence));
    set_field(obj, "relation", JVal(std::move(relation)));
  }
  if (emit.attribute) {
    JObject attribute;
    set_field(attribute, "namespace", JVal(emit.attribute->namespace_name));
    set_field(attribute, "attribute_name",
              JVal(emit.attribute->attribute_name));
    set_field(attribute, "binding", JVal(emit.attribute->binding));
    set_field(attribute, "ast_property", JVal(emit.attribute->ast_property));
    set_field(obj, "attribute", JVal(std::move(attribute)));
  }
  if (emit.unknown) {
    JObject unknown;
    set_field(unknown, "namespace", JVal(emit.unknown->namespace_name));
    set_field(unknown, "reason_code", JVal(emit.unknown->reason_code));
    set_field(unknown, "binding", JVal(emit.unknown->binding));
    set_field(obj, "unknown", JVal(std::move(unknown)));
  }
  return {std::move(obj)};
}

EmitOperation emit_from_json(const JVal &value) {
  if (!value.is_object()) {
    throw PlanParseError("emit operation must be an object", 0);
  }
  const JObject &obj = value.as_object();
  reject_unknown_fields(obj, {"node", "relation", "attribute", "unknown"},
                        "emit operation");
  EmitOperation emit;
  int payloads = 0;
  if (const JVal *node = find_field(obj, "node")) {
    if (!node->is_object()) {
      throw PlanParseError("emit node must be an object", 0);
    }
    const JObject &nobj = node->as_object();
    reject_unknown_fields(
        nobj, {"namespace", "node_kind", "binding", "identity"}, "emit node");
    EmitNode built;
    built.namespace_name = require_string(nobj, "namespace", "emit node");
    built.node_kind = require_string(nobj, "node_kind", "emit node");
    built.binding = require_string(nobj, "binding", "emit node");
    const JVal *identity = find_field(nobj, "identity");
    if (identity == nullptr) {
      throw PlanParseError("emit node missing identity", 0);
    }
    built.identity = identity_from_json(*identity);
    emit.node = std::move(built);
    ++payloads;
  }
  if (const JVal *relation = find_field(obj, "relation")) {
    if (!relation->is_object()) {
      throw PlanParseError("emit relation must be an object", 0);
    }
    const JObject &robj = relation->as_object();
    reject_unknown_fields(robj,
                          {"namespace", "relation_kind", "from_binding",
                           "to_binding", "with_evidence"},
                          "emit relation");
    EmitRelation built;
    built.namespace_name = require_string(robj, "namespace", "emit relation");
    built.relation_kind =
        require_string(robj, "relation_kind", "emit relation");
    built.from_binding = require_string(robj, "from_binding", "emit relation");
    built.to_binding = require_string(robj, "to_binding", "emit relation");
    built.with_evidence = optional_bool(robj, "with_evidence", true);
    emit.relation = std::move(built);
    ++payloads;
  }
  if (const JVal *attribute = find_field(obj, "attribute")) {
    if (!attribute->is_object()) {
      throw PlanParseError("emit attribute must be an object", 0);
    }
    const JObject &aobj = attribute->as_object();
    reject_unknown_fields(
        aobj, {"namespace", "attribute_name", "binding", "ast_property"},
        "emit attribute");
    EmitAttribute built;
    built.namespace_name = require_string(aobj, "namespace", "emit attribute");
    built.attribute_name =
        require_string(aobj, "attribute_name", "emit attribute");
    built.binding = require_string(aobj, "binding", "emit attribute");
    built.ast_property = require_string(aobj, "ast_property", "emit attribute");
    emit.attribute = std::move(built);
    ++payloads;
  }
  if (const JVal *unknown = find_field(obj, "unknown")) {
    if (!unknown->is_object()) {
      throw PlanParseError("emit unknown must be an object", 0);
    }
    const JObject &uobj = unknown->as_object();
    reject_unknown_fields(uobj, {"namespace", "reason_code", "binding"},
                          "emit unknown");
    EmitUnknown built;
    built.namespace_name = require_string(uobj, "namespace", "emit unknown");
    built.reason_code = require_string(uobj, "reason_code", "emit unknown");
    built.binding = require_string(uobj, "binding", "emit unknown");
    emit.unknown = std::move(built);
    ++payloads;
  }
  if (payloads != 1) {
    throw PlanParseError(
        "an emit operation must set exactly one of node/relation/attribute/"
        "unknown",
        0);
  }
  return emit;
}

JVal rule_to_json(const ExtractionRule &rule) {
  JObject obj;
  set_field(obj, "id", JVal(rule.id));
  set_field(obj, "version", JVal(static_cast<std::int64_t>(rule.version)));
  set_field(obj, "matcher_expression", JVal(rule.matcher_expression));
  JArray bindings;
  for (const auto &binding : rule.bindings) {
    JObject bobj;
    set_field(bobj, "name", JVal(binding.name));
    set_field(bobj, "domain", JVal(to_string(binding.domain)));
    bindings.emplace_back(std::move(bobj));
  }
  set_field(obj, "bindings", JVal(std::move(bindings)));
  JArray emits;
  for (const auto &emit : rule.emits) {
    emits.push_back(emit_to_json(emit));
  }
  set_field(obj, "emits", JVal(std::move(emits)));
  set_field(obj, "scope", JVal(to_string(rule.scope)));
  set_field(obj, "traversal", JVal(to_string(rule.traversal)));
  set_field(obj, "completeness", JVal(to_string(rule.completeness)));
  JObject budget;
  set_field(budget, "max_matches", JVal(rule.budget.max_matches));
  set_field(budget, "max_emitted_facts", JVal(rule.budget.max_emitted_facts));
  set_field(budget, "max_visited_nodes", JVal(rule.budget.max_visited_nodes));
  set_field(budget, "declared", JVal(rule.budget.declared));
  set_field(obj, "budget", JVal(std::move(budget)));
  set_field(obj, "producer_package", JVal(rule.producer_package));
  set_field(obj, "producer_version",
            JVal(static_cast<std::int64_t>(rule.producer_version)));
  return {std::move(obj)};
}

ExtractionRule rule_from_json(const JVal &value) {
  if (!value.is_object()) {
    throw PlanParseError("rule must be an object", 0);
  }
  const JObject &obj = value.as_object();
  reject_unknown_fields(obj,
                        {"id", "version", "matcher_expression", "bindings",
                         "emits", "scope", "traversal", "completeness",
                         "budget", "producer_package", "producer_version"},
                        "rule");
  ExtractionRule rule;
  rule.id = require_string(obj, "id", "rule");
  rule.version =
      static_cast<std::uint32_t>(require_int(obj, "version", "rule"));
  rule.matcher_expression = require_string(obj, "matcher_expression", "rule");
  for (const auto &item : require_array(obj, "bindings", "rule")) {
    if (!item.is_object()) {
      throw PlanParseError("binding must be an object", 0);
    }
    const JObject &bobj = item.as_object();
    reject_unknown_fields(bobj, {"name", "domain"}, "binding");
    Binding binding;
    binding.name = require_string(bobj, "name", "binding");
    binding.domain = parse_domain(require_string(bobj, "domain", "binding"));
    rule.bindings.push_back(std::move(binding));
  }
  for (const auto &item : require_array(obj, "emits", "rule")) {
    rule.emits.push_back(emit_from_json(item));
  }
  rule.scope = parse_scope(require_string(obj, "scope", "rule"));
  rule.traversal = parse_traversal(require_string(obj, "traversal", "rule"));
  rule.completeness =
      parse_completeness(require_string(obj, "completeness", "rule"));
  const JVal *budget_val = find_field(obj, "budget");
  if (budget_val == nullptr || !budget_val->is_object()) {
    throw PlanParseError("rule missing budget object", 0);
  }
  const JObject &budget_obj = budget_val->as_object();
  reject_unknown_fields(
      budget_obj,
      {"max_matches", "max_emitted_facts", "max_visited_nodes", "declared"},
      "budget");
  rule.budget.max_matches = require_int(budget_obj, "max_matches", "budget");
  rule.budget.max_emitted_facts =
      require_int(budget_obj, "max_emitted_facts", "budget");
  rule.budget.max_visited_nodes =
      require_int(budget_obj, "max_visited_nodes", "budget");
  rule.budget.declared = optional_bool(budget_obj, "declared", false);
  rule.producer_package = require_string(obj, "producer_package", "rule");
  rule.producer_version =
      static_cast<std::uint32_t>(require_int(obj, "producer_version", "rule"));
  return rule;
}

} // namespace

std::string canonical_json(const ExtractionPlan &plan) {
  JObject obj;
  set_field(obj, "schema_version",
            JVal(static_cast<std::int64_t>(plan.schema_version)));
  set_field(obj, "plan_id", JVal(plan.plan_id));
  set_field(obj, "plan_version",
            JVal(static_cast<std::int64_t>(plan.plan_version)));
  JArray catalog_versions;
  for (auto version : plan.catalog_versions) {
    catalog_versions.emplace_back(static_cast<std::int64_t>(version));
  }
  set_field(obj, "catalog_versions", JVal(std::move(catalog_versions)));
  JArray rules;
  for (const auto &rule : plan.rules) {
    rules.push_back(rule_to_json(rule));
  }
  set_field(obj, "rules", JVal(std::move(rules)));
  return dump(JVal(std::move(obj)));
}

ExtractionPlan parse_plan_json(std::string_view text) {
  JsonParser parser(text);
  JVal document = parser.parse_document();
  if (!document.is_object()) {
    throw PlanParseError("an ExtractionPlan document must be a JSON object", 0);
  }
  const JObject &obj = document.as_object();
  reject_unknown_fields(obj,
                        {"schema_version", "plan_id", "plan_version",
                         "catalog_versions", "rules"},
                        "ExtractionPlan");
  ExtractionPlan plan;
  plan.schema_version = static_cast<std::uint32_t>(
      require_int(obj, "schema_version", "ExtractionPlan"));
  plan.plan_id = require_string(obj, "plan_id", "ExtractionPlan");
  plan.plan_version = static_cast<std::uint32_t>(
      require_int(obj, "plan_version", "ExtractionPlan"));
  for (const auto &item :
       require_array(obj, "catalog_versions", "ExtractionPlan")) {
    if (!item.is_int()) {
      throw PlanParseError("catalog_versions must be integers", 0);
    }
    plan.catalog_versions.push_back(static_cast<std::uint32_t>(item.as_int()));
  }
  for (const auto &item : require_array(obj, "rules", "ExtractionPlan")) {
    plan.rules.push_back(rule_from_json(item));
  }
  return plan;
}

} // namespace cidx::extract
