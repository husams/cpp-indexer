// Source-location helpers that reproduce libclang's location conventions.
//
// libclang reports expansion locations, and its cursor-extent END is adjusted
// one past the last token (CIndex translateSourceRange measures the token
// length). The C++ API's getSourceRange().getEnd() is the START of the last
// token, so extent_end applies Lexer::getLocForEndOfToken to match.
#pragma once

#include <cstdint>
#include <string>

namespace clang {
class ASTContext;
class SourceLocation;
class SourceRange;
} // namespace clang

namespace cidx::lt {

struct ExpansionLoc {
  std::string file; // empty = no file (built-in / invalid)
  int64_t line = 0;
  int64_t col = 0;
};

// Expansion location of `loc` (libclang cursor.location convention).
ExpansionLoc expansion_loc(const clang::ASTContext &context,
                           clang::SourceLocation loc);

// Expansion location of the range start (libclang extent.start).
ExpansionLoc extent_start(const clang::ASTContext &context,
                          clang::SourceRange range);

// Expansion location one past the last token of the range (libclang
// extent.end).
ExpansionLoc extent_end(const clang::ASTContext &context,
                        clang::SourceRange range);

} // namespace cidx::lt
