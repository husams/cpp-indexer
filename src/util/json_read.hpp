// json_read -- a general JSON parser producing the json_out::Value tree.
//
// json_min decodes arrays of strings (the compile_options column) and json_out
// writes a full value tree. Reading a full tree back had no home until the
// include-hygiene cleanup plan needed it: `cidx include apply` must parse an
// artifact `cidx include plan` wrote. Parsing into json_out::Value keeps ONE
// value representation for both directions.
//
// Deliberately strict. A cleanup plan is an untrusted input that describes
// edits to source files, so this rejects rather than guesses: no trailing
// commas, no comments, no NaN/Infinity, no single quotes, no duplicate-key
// merging (last wins, as in CPython), and a hard nesting cap so a hostile
// artifact cannot exhaust the stack.
#pragma once

#include <string>

#include "cli/json_out.hpp"

namespace cidx {
namespace json_read {

// Maximum array/object nesting. Far above any plan cidx emits; low enough that
// the recursive-descent parser cannot be driven into a stack overflow.
inline constexpr int kMaxDepth = 100;

// Parse `text` as JSON. Throws CidxError with a byte offset on malformed
// input, unbalanced structure, or nesting beyond kMaxDepth.
cidx::json_out::Value parse(const std::string &text);

} // namespace json_read
} // namespace cidx
