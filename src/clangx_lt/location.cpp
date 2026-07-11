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
  // libclang reports the file SPELLING (symlinks like MacOSX.sdk stay
  // unresolved). Only a RELATIVE spelling (compile-db "file" entries) is
  // canonicalized so absolute-path consumers can match it.
  out.file = sm.getFilename(exp).str();
  if (!out.file.empty() && out.file[0] != '/') {
    if (auto fe = sm.getFileEntryRefForID(sm.getFileID(exp))) {
      llvm::StringRef real = fe->getFileEntry().tryGetRealPathName();
      if (!real.empty())
        out.file = real.str();
    }
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
