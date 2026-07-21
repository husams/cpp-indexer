#include "util/json_read.hpp"

#include "util/errors.hpp"

#include <cstdlib>
#include <string>

namespace cidx::json_read {

namespace {

using cidx::json_out::Value;

[[noreturn]] void fail(const std::string &msg, std::size_t at) {
  throw CidxError("json: " + msg + " at offset " + std::to_string(at));
}

class Parser {
public:
  explicit Parser(const std::string &s) : s_(s) {}

  Value parse_document() {
    skip_ws();
    Value v = parse_value(0);
    skip_ws();
    if (i_ != s_.size()) {
      fail("trailing data after top-level value", i_);
    }
    return v;
  }

private:
  void skip_ws() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' ||
                              s_[i_] == '\n' || s_[i_] == '\r')) {
      ++i_;
    }
  }

  [[nodiscard]] char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }

  void expect(char c) {
    if (i_ >= s_.size() || s_[i_] != c) {
      fail(std::string("expected '") + c + "'", i_);
    }
    ++i_;
  }

  bool literal(const char *word) {
    const std::size_t n = std::char_traits<char>::length(word);
    if (s_.compare(i_, n, word) != 0) {
      return false;
    }
    i_ += n;
    return true;
  }

  Value parse_value(int depth) {
    if (depth > kMaxDepth) {
      fail("nesting deeper than " + std::to_string(kMaxDepth), i_);
    }
    switch (peek()) {
    case '{':
      return parse_object(depth);
    case '[':
      return parse_array(depth);
    case '"':
      return Value::of(parse_string());
    case 't':
      if (literal("true")) {
        return Value::of(true);
      }
      fail("bad literal", i_);
    case 'f':
      if (literal("false")) {
        return Value::of(false);
      }
      fail("bad literal", i_);
    case 'n':
      if (literal("null")) {
        return Value::null();
      }
      fail("bad literal", i_);
    default:
      return parse_number();
    }
  }

  Value parse_object(int depth) {
    expect('{');
    json_out::Object members;
    skip_ws();
    if (peek() == '}') {
      ++i_;
      return Value::obj(std::move(members));
    }
    while (true) {
      skip_ws();
      if (peek() != '"') {
        fail("object key must be a string", i_);
      }
      std::string key = parse_string();
      skip_ws();
      expect(':');
      skip_ws();
      Value v = parse_value(depth + 1);
      // Last wins, matching CPython. Callers look keys up by name, so a
      // duplicate must not produce two members with the same key.
      bool replaced = false;
      for (auto &m : members) {
        if (m.first == key) {
          m.second = std::move(v);
          replaced = true;
          break;
        }
      }
      if (!replaced) {
        members.emplace_back(std::move(key), std::move(v));
      }
      skip_ws();
      if (peek() == ',') {
        ++i_;
        continue;
      }
      expect('}');
      return Value::obj(std::move(members));
    }
  }

  Value parse_array(int depth) {
    expect('[');
    json_out::Array items;
    skip_ws();
    if (peek() == ']') {
      ++i_;
      return Value::arr(std::move(items));
    }
    while (true) {
      skip_ws();
      items.push_back(parse_value(depth + 1));
      skip_ws();
      if (peek() == ',') {
        ++i_;
        continue;
      }
      expect(']');
      return Value::arr(std::move(items));
    }
  }

  unsigned hex4() {
    if (i_ + 4 > s_.size()) {
      fail("truncated \\u escape", i_);
    }
    unsigned cp = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = s_[i_++];
      cp <<= 4;
      if (c >= '0' && c <= '9') {
        cp |= static_cast<unsigned>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        cp |= static_cast<unsigned>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        cp |= static_cast<unsigned>(c - 'A' + 10);
      } else {
        fail("bad hex digit in \\u escape", i_ - 1);
      }
    }
    return cp;
  }

  static void append_utf8(std::string &out, unsigned cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  std::string parse_string() {
    expect('"');
    std::string out;
    while (true) {
      if (i_ >= s_.size()) {
        fail("unterminated string", i_);
      }
      const char c = s_[i_++];
      if (c == '"') {
        return out;
      }
      if (c != '\\') {
        // Raw control characters are invalid JSON; a plan carrying one is
        // corrupt, not merely unusual.
        if (static_cast<unsigned char>(c) < 0x20) {
          fail("raw control character in string", i_ - 1);
        }
        out += c;
        continue;
      }
      if (i_ >= s_.size()) {
        fail("trailing backslash", i_);
      }
      switch (s_[i_++]) {
      case '"': out += '"'; break;
      case '\\': out += '\\'; break;
      case '/': out += '/'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'u': {
        unsigned cp = hex4();
        if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 1 < s_.size() &&
            s_[i_] == '\\' && s_[i_ + 1] == 'u') {
          const std::size_t save = i_;
          i_ += 2;
          const unsigned lo = hex4();
          if (lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          } else {
            i_ = save; // unpaired high surrogate: emit it alone (json_min parity)
          }
        }
        append_utf8(out, cp);
        break;
      }
      default:
        fail("unknown escape", i_ - 1);
      }
    }
  }

  Value parse_number() {
    const std::size_t start = i_;
    if (peek() == '-') {
      ++i_;
    }
    while (i_ < s_.size() && ((s_[i_] >= '0' && s_[i_] <= '9') ||
                              s_[i_] == '.' || s_[i_] == 'e' ||
                              s_[i_] == 'E' || s_[i_] == '+' ||
                              s_[i_] == '-')) {
      ++i_;
    }
    if (i_ == start) {
      fail("expected a value", start);
    }
    const std::string text = s_.substr(start, i_ - start);
    // The value tree has no float node, and every number a plan carries is an
    // offset, a count, or a version -- all integral. A fractional number is a
    // corrupt plan, not something to silently truncate.
    if (text.find_first_of(".eE") != std::string::npos) {
      fail("non-integer number '" + text + "'", start);
    }
    errno = 0;
    char *end = nullptr;
    const long long v = std::strtoll(text.c_str(), &end, 10);
    if (errno != 0 || end != text.c_str() + text.size()) {
      fail("bad number '" + text + "'", start);
    }
    return Value::of(v);
  }

  const std::string &s_;
  std::size_t i_ = 0;
};

} // namespace

Value parse(const std::string &text) { return Parser(text).parse_document(); }

} // namespace cidx::json_read
