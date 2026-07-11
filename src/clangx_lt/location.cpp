#include "clangx_lt/location.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"

namespace cidx::lt {

namespace {

ExpansionLoc to_expansion(const clang::SourceManager &sm,
                          clang::SourceLocation loc) {
  ExpansionLoc out;
  if (loc.isInvalid())
    return out;
  const clang::SourceLocation exp = sm.getExpansionLoc(loc);
  // Canonical absolute path: the invocation may spell the file relatively
  // (compile-db "file" entries), but consumers compare absolute paths.
  if (auto fe = sm.getFileEntryRefForID(sm.getFileID(exp))) {
    llvm::StringRef real = fe->getFileEntry().tryGetRealPathName();
    out.file = real.empty() ? fe->getName().str() : real.str();
  } else {
    out.file = sm.getFilename(exp).str();
  }
  out.line = sm.getExpansionLineNumber(exp);
  out.col = sm.getExpansionColumnNumber(exp);
  return out;
}

} // namespace

ExpansionLoc expansion_loc(const clang::ASTContext &context,
                           clang::SourceLocation loc) {
  return to_expansion(context.getSourceManager(), loc);
}

ExpansionLoc extent_start(const clang::ASTContext &context,
                          clang::SourceRange range) {
  return to_expansion(context.getSourceManager(), range.getBegin());
}

ExpansionLoc extent_end(const clang::ASTContext &context,
                        clang::SourceRange range) {
  const clang::SourceManager &sm = context.getSourceManager();
  clang::SourceLocation end = range.getEnd();
  if (end.isInvalid())
    return {};
  end = sm.getExpansionLoc(end);
  // libclang extent end = start of last token + token length.
  const clang::SourceLocation past = clang::Lexer::getLocForEndOfToken(
      end, /*Offset=*/0, sm, context.getLangOpts());
  return to_expansion(sm, past.isValid() ? past : end);
}

} // namespace cidx::lt
