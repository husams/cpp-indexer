// query/cxq.cpp -- small dependency-free CXQ parser.

#include "query/cxq.hpp"

#include <cctype>
#include <charconv>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace cidx::query {
namespace {

[[noreturn]] void parse_fail(const std::string &message) {
  throw PlanError("E_PARSE: " + message);
}

std::string trim(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.remove_suffix(1);
  }
  return std::string(value);
}

bool quoted(std::string_view value) {
  return value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                               (value.front() == '\'' && value.back() == '\''));
}

std::string atom(std::string_view value) {
  std::string result = trim(value);
  if (quoted(result)) {
    return result.substr(1, result.size() - 2);
  }
  return result;
}

std::vector<std::string> split_top_level(std::string_view value,
                                         char delimiter) {
  std::vector<std::string> parts;
  size_t begin = 0;
  int parens = 0;
  int brackets = 0;
  char quote = '\0';
  for (size_t i = 0; i < value.size(); ++i) {
    const char ch = value[i];
    if (quote != '\0') {
      if (ch == quote && (i == 0 || value[i - 1] != '\\')) {
        quote = '\0';
      }
      continue;
    }
    if (ch == '\'' || ch == '"') {
      quote = ch;
    } else if (ch == '(') {
      ++parens;
    } else if (ch == ')') {
      --parens;
    } else if (ch == '[') {
      ++brackets;
    } else if (ch == ']') {
      --brackets;
    } else if (ch == delimiter && parens == 0 && brackets == 0) {
      parts.push_back(trim(value.substr(begin, i - begin)));
      begin = i + 1;
    }
  }
  if (quote != '\0' || parens != 0 || brackets != 0) {
    parse_fail("unbalanced quotes or delimiters");
  }
  parts.push_back(trim(value.substr(begin)));
  return parts;
}

std::vector<std::string> call_parts(std::string_view token,
                                    std::string_view expected) {
  const std::string value = trim(token);
  const std::string prefix(expected);
  if (!value.starts_with(prefix + "(") || !value.ends_with(")") ||
      value.size() <= prefix.size() + 1) {
    parse_fail("expected " + prefix + "(...)");
  }
  const std::string_view inside(value.data() + prefix.size() + 1,
                                value.size() - prefix.size() - 2);
  if (trim(inside).empty()) {
    return {};
  }
  return split_top_level(inside, ',');
}

size_t find_top_level_keyword(std::string_view value,
                              std::string_view keyword) {
  int parens = 0;
  int brackets = 0;
  char quote = '\0';
  for (size_t i = 0; i + keyword.size() <= value.size(); ++i) {
    const char ch = value[i];
    if (quote != '\0') {
      if (ch == quote && (i == 0 || value[i - 1] != '\\')) {
        quote = '\0';
      }
      continue;
    }
    if (ch == '\'' || ch == '"') {
      quote = ch;
      continue;
    }
    if (ch == '(') {
      ++parens;
      continue;
    }
    if (ch == ')') {
      --parens;
      continue;
    }
    if (ch == '[') {
      ++brackets;
      continue;
    }
    if (ch == ']') {
      --brackets;
      continue;
    }
    const bool left_boundary =
        i == 0 || std::isspace(static_cast<unsigned char>(value[i - 1])) != 0;
    const size_t end = i + keyword.size();
    const bool right_boundary =
        end == value.size() ||
        std::isspace(static_cast<unsigned char>(value[end])) != 0;
    if (parens == 0 && brackets == 0 && left_boundary && right_boundary &&
        value.substr(i, keyword.size()) == keyword) {
      return i;
    }
  }
  return std::string_view::npos;
}

bool integer(std::string_view value, int64_t &out) {
  const std::string text = trim(value);
  if (text.empty()) {
    return false;
  }
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), out);
  return error == std::errc{} && end == text.data() + text.size();
}

Pred predicate(std::string_view expression);

std::vector<std::string> list_values(std::string_view value) {
  const std::string text = trim(value);
  if (text.size() < 2 || text.front() != '[' || text.back() != ']') {
    parse_fail("expected a bracketed value list");
  }
  const std::string_view inside(text.data() + 1, text.size() - 2);
  if (trim(inside).empty()) {
    parse_fail("value list must not be empty");
  }
  std::vector<std::string> values;
  for (const auto &part : split_top_level(inside, ',')) {
    values.push_back(atom(part));
  }
  return values;
}

Pred comparison(std::string_view expression) {
  const std::string text = trim(expression);
  for (const std::string_view op : {"!=", "~=", "=", " in "}) {
    const size_t at = text.find(op);
    if (at == std::string::npos) {
      continue;
    }
    const std::string field_name = trim(std::string_view(text).substr(0, at));
    const std::string_view rhs = std::string_view(text).substr(at + op.size());
    if (field_name.empty()) {
      parse_fail("missing predicate field");
    }
    if (op == " in ") {
      return in_list(field_name, list_values(rhs));
    }
    const std::string value = atom(rhs);
    if (op == "~=") {
      return glob(field_name, value);
    }
    if (value == "true" || value == "false") {
      return op == "=" ? eq(field_name, value == "true")
                       : not_(eq(field_name, value == "true"));
    }
    int64_t number = 0;
    if (integer(value, number)) {
      return op == "=" ? eq(field_name, number) : not_(eq(field_name, number));
    }
    return op == "=" ? eq(field_name, value) : ne(field_name, value);
  }
  parse_fail("unsupported predicate '" + text + "'");
}

Pred predicate(std::string_view expression) {
  std::string text = trim(expression);
  while (text.size() >= 2 && text.front() == '(' && text.back() == ')') {
    const auto parts =
        split_top_level(std::string_view(text).substr(1, text.size() - 2), ',');
    if (parts.size() != 1) {
      break;
    }
    text = trim(std::string_view(text).substr(1, text.size() - 2));
  }
  if (text.starts_with("not ")) {
    return not_(predicate(std::string_view(text).substr(4)));
  }
  for (const std::string_view op : {"or", "and"}) {
    const size_t at = find_top_level_keyword(text, op);
    if (at != std::string::npos) {
      std::vector<Pred> parts;
      parts.push_back(predicate(std::string_view(text).substr(0, at)));
      parts.push_back(predicate(std::string_view(text).substr(at + op.size())));
      return op == "or" ? any_of(std::move(parts)) : all_of(std::move(parts));
    }
  }
  for (const std::string_view name : {"eq", "ne", "glob", "in"}) {
    const std::string prefix(name);
    if (text.starts_with(prefix + "(")) {
      const auto args = call_parts(text, name);
      if (name == "in") {
        if (args.size() != 2) {
          parse_fail("in() requires a field and list");
        }
        return in_list(atom(args[0]), list_values(args[1]));
      }
      if (args.size() != 2) {
        parse_fail(prefix + "() requires a field and value");
      }
      const std::string field_name = atom(args[0]);
      const std::string value = atom(args[1]);
      if (name == "glob") {
        return glob(field_name, value);
      }
      if (value == "true" || value == "false") {
        return name == "eq" ? eq(field_name, value == "true")
                            : not_(eq(field_name, value == "true"));
      }
      int64_t number = 0;
      if (integer(value, number)) {
        return name == "eq" ? eq(field_name, number)
                            : not_(eq(field_name, number));
      }
      return name == "eq" ? eq(field_name, value) : ne(field_name, value);
    }
  }
  return comparison(text);
}

std::pair<int64_t, int64_t> depth_args(const std::vector<std::string> &args) {
  if (args.size() == 1) {
    return {1, 1};
  }
  if (args.size() == 2 && args[1].starts_with("depth=")) {
    const std::string value = args[1].substr(6);
    const size_t dots = value.find("..");
    int64_t min_depth = 0;
    int64_t max_depth = 0;
    if (dots == std::string::npos ||
        !integer(value.substr(0, dots), min_depth) ||
        !integer(value.substr(dots + 2), max_depth)) {
      parse_fail("depth must be written as depth=min..max");
    }
    return {min_depth, max_depth};
  }
  if (args.size() == 2) {
    int64_t depth = 0;
    if (!integer(args[1], depth)) {
      parse_fail("depth must be an integer or depth=min..max");
    }
    return {depth, depth};
  }
  if (args.size() == 3) {
    int64_t min_depth = 0;
    int64_t max_depth = 0;
    if (!integer(args[1], min_depth) || !integer(args[2], max_depth)) {
      parse_fail("depth must be written as depth=min..max");
    }
    return {min_depth, max_depth};
  }
  parse_fail("traversal accepts relation and at most two depth arguments");
}

Stage stage(std::string_view token) {
  const std::string value = trim(token);
  if (value.starts_with("rank(")) {
    parse_fail("rank() is not available in v1");
  }
  for (const std::string_view name :
       {"nodes", "view", "where", "out", "in", "sites", "union", "intersect", "except",
        "select", "count", "distinct", "order_by", "limit"}) {
    const std::string prefix(name);
    if (!value.starts_with(prefix + "(")) {
      continue;
    }
    const auto args = call_parts(value, name);
    if (name == "nodes") {
      if (args.size() > 1) {
        parse_fail("nodes() takes zero or one predicate");
      }
      return args.empty() ? nodes() : nodes(predicate(args.front()));
    }
    if (name == "view") {
      if (args.size() != 1) {
        parse_fail("view() requires one level");
      }
      const std::string level = atom(args.front());
      if (level == "symbol") {
        return view(View::Symbol);
      }
      if (level == "entity") {
        return view(View::Entity);
      }
      parse_fail("unknown view '" + level + "'");
    }
    if (name == "where") {
      if (args.size() != 1) {
        parse_fail("where() requires one expression");
      }
      return where(predicate(args.front()));
    }
    if (name == "sites") {
      if (!args.empty()) {
        parse_fail("sites() takes no arguments");
      }
      return sites();
    }
    if (name == "out" || name == "in") {
      if (args.empty()) {
        parse_fail(std::string(name) + "() requires a relation");
      }
      const auto [min_depth, max_depth] = depth_args(args);
      if (name == "out") {
        return out(atom(args.front()), min_depth, max_depth);
      }
      return in_(atom(args.front()), min_depth, max_depth);
    }
    if (name == "select" || name == "order_by") {
      std::vector<std::string> fields;
      fields.reserve(args.size());
      for (const auto &arg : args) {
        fields.push_back(atom(arg));
      }
      if (fields.empty()) {
        parse_fail(std::string(name) + "() requires fields");
      }
      return name == "select" ? select(std::move(fields))
                              : order_by(std::move(fields));
    }
    if (name == "count") {
      if (!args.empty()) {
        parse_fail("count() takes no arguments");
      }
      return count();
    }
    if (name == "distinct") {
      if (!args.empty()) {
        parse_fail("distinct() takes no arguments");
      }
      return distinct();
    }
    if (name == "limit") {
      int64_t n = 0;
      if (args.size() != 1 || !integer(args.front(), n)) {
        parse_fail("limit() requires one integer");
      }
      return limit(n);
    }
    if (name == "union" || name == "intersect" || name == "except") {
      if (args.size() != 1) {
        parse_fail(std::string(name) + "() requires one query");
      }
      Plan parsed = parse_cxq(args.front());
      Query query(parsed.source);
      for (const auto &nested : parsed.stages) {
        query = query | nested;
      }
      if (name == "union") {
        return union_(query);
      }
      if (name == "intersect") {
        return intersect(query);
      }
      return except_(query);
    }
  }
  parse_fail("unknown stage '" + value + "'");
}

} // namespace

Plan parse_cxq(const std::string &text) {
  const auto parts = split_top_level(text, '|');
  if (parts.empty() || parts.front().empty()) {
    parse_fail("query is empty");
  }
  const std::string first = trim(parts.front());
  Source source;
  if (first.starts_with("codebase(")) {
    const auto source_args = call_parts(first, "codebase");
    if (!source_args.empty()) {
      parse_fail("codebase() takes no arguments");
    }
    source = codebase();
  } else if (first.starts_with("symbol(")) {
    const auto source_args = call_parts(first, "symbol");
    if (source_args.size() != 1) {
      parse_fail("symbol() requires one reference");
    }
    source = symbol(atom(source_args.front()));
  } else if (first.starts_with("entity(")) {
    const auto source_args = call_parts(first, "entity");
    if (source_args.size() != 1) {
      parse_fail("entity() requires one reference");
    }
    source = entity(atom(source_args.front()));
  } else {
    parse_fail("query must start with codebase(), symbol(), or entity()");
  }
  Query query(source);
  for (size_t i = 1; i < parts.size(); ++i) {
    query = query | stage(parts[i]);
  }
  return query.plan();
}

} // namespace cidx::query
