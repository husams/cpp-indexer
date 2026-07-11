#include "clangx/ast.hpp"
#include "clangx/ast_internal.hpp"

#include <optional>
#include <string>
#include <utility>

#include "clangx/clang_raii.hpp"

namespace cidx {
using namespace ast_detail;

std::optional<Symbol> AstIndexer::to_symbol(CXCursor cursor, int64_t file_id) {
  const char *kind = kind_name(::clang_getCursorKind(cursor));
  if (kind == nullptr) {
    return std::nullopt;
  }
  std::string usr = CxString(::clang_getCursorUSR(cursor)).str();
  if (usr.empty()) {
    return std::nullopt; // no USR -> not indexable (ast.py:99-101)
  }

  // Parent USR: the semantic parent unless it is the TU (ast.py:102-105).
  std::optional<std::string> parent_usr;
  const CXCursor parent = ::clang_getCursorSemanticParent(cursor);
  const CXCursorKind parent_kind = ::clang_getCursorKind(parent);
  if (!is_invalid_kind(parent_kind) &&
      parent_kind != CXCursor_TranslationUnit) {
    std::string pu = CxString(::clang_getCursorUSR(parent)).str();
    if (!pu.empty()) {
      parent_usr = std::move(pu);
    }
  }

  const bool is_def = ::clang_isCursorDefinition(cursor) != 0;
  const ExpansionLoc loc = cursor_location(cursor);

  Symbol sym;
  sym.usr = std::move(usr);
  sym.spelling = CxString(::clang_getCursorSpelling(cursor)).str();
  sym.kind = kind;
  std::string qual = qualified_name(cursor);
  if (!qual.empty()) {
    sym.qual_name = std::move(qual);
  }
  std::string display =
      CxString(::clang_getCursorDisplayName(cursor)).str();
  if (!display.empty()) {
    sym.display_name = std::move(display);
  }
  const CXCursorKind ck = ::clang_getCursorKind(cursor);
  const CXType info_type =
      (ck == CXCursor_TypedefDecl || ck == CXCursor_TypeAliasDecl)
          ? ::clang_getTypedefDeclUnderlyingType(cursor)
          : ::clang_getCursorType(cursor);
  std::string type_info = CxString(::clang_getTypeSpelling(info_type)).str();
  if (!type_info.empty()) {
    sym.type_info = std::move(type_info);
  }
  sym.file_id = file_id;
  // Start of this cursor's own extent -- NOT `loc` (cursor.location, the
  // identifying spelling location) -- so (line, col)..(end_line, end_col)
  // slices the WHOLE declaration (ast.py:_to_symbol).
  const ExpansionLoc start = cursor_extent_start(cursor);
  sym.line = static_cast<int64_t>(start.line);
  sym.col = static_cast<int64_t>(start.col);
  // End of this cursor's own extent, paired with (line, col) so
  // (line..end_line) slices the whole entity (ast.py:_to_symbol). The upsert
  // moves end_line/end_col in lockstep with line/col.
  const ExpansionLoc end = cursor_extent_end(cursor);
  sym.end_line = static_cast<int64_t>(end.line);
  sym.end_col = static_cast<int64_t>(end.col);
  // A declaration cursor records itself as the decl site too; the upsert
  // keeps it when the definition later takes file/line/col (ast.py:117-121).
  if (!is_def) {
    sym.decl_file_id = file_id;
    sym.decl_line = static_cast<int64_t>(loc.line);
    sym.decl_col = static_cast<int64_t>(loc.col);
  }
  sym.is_definition = is_def;
  sym.is_pure = ::clang_CXXMethod_isPureVirtual(cursor) != 0;
  // C++ static member function. False for free functions and non-methods; a
  // file-scope `static` free function is captured by linkage='internal'.
  sym.is_static = ::clang_CXXMethod_isStatic(cursor) != 0;
  sym.linkage = linkage_name(::clang_getCursorLinkage(cursor));
  sym.access = access_name(::clang_getCXXAccessSpecifier(cursor));
  sym.parent_usr = std::move(parent_usr);
  // A definition resolves the symbol; a bare declaration leaves it
  // unresolved until some TU provides the definition (ast.py:127-129).
  sym.resolved = is_def;
  return sym;
}

bool AstIndexer::store(const Symbol &sym) {
  // Always upsert (ast.py mirror): add_symbol's own CASE-WHEN/COALESCE logic
  // never lets a lesser declaration cursor downgrade an already-stored
  // definition's location/extent — it only fills gaps (e.g. the decl site,
  // G15) and refreshes fields the row didn't carry yet (e.g. end_line/
  // end_col backfilled by a schema migration). A prior version skipped the
  // write entirely for an already-resolved symbol, so add_symbol (the only
  // place that writes end_line/end_col) was never reached again on reindex.
  const std::optional<Symbol> existing = db_.lookup_symbol(sym.usr);
  db_.add_symbol(sym);
  return !(existing && existing->resolved); // true = counted as "stored"
}

// M4: txn-free inner work — caller MUST own an open transaction.
std::pair<int, int> AstIndexer::index_file_notxn(const ParsedTu &tu,
                                                 const std::string &filename,
                                                 int64_t file_id) {
  int stored = 0;
  int skipped = 0;
  for_file_cursors(tu, filename, [&](CXCursor cursor) {
    const std::optional<Symbol> sym = to_symbol(cursor, file_id);
    if (sym) {
      if (store(*sym)) {
        ++stored;
      } else {
        ++skipped;
      }
    }
    // for_file_cursors stops at function bodies, so body-scoped named type
    // declarations (local using/typedef/enum/record + local-record members)
    // are unreachable above. Descend into each function/method DEFINITION to
    // pick them up (ast.py:_index_file_notxn). Definitions only -- a bare
    // prototype has no body. Local variables are NOT emitted (they feed
    // reference sites via index_edges, not the symbol table).
    const CXCursorKind ck = ::clang_getCursorKind(cursor);
    if (is_function_like(ck) && ::clang_isCursorDefinition(cursor) != 0) {
      for_body_local_symbols(cursor, filename, [&](CXCursor local) {
        const std::optional<Symbol> lsym = to_symbol(local, file_id);
        if (!lsym) {
          return;
        }
        if (store(*lsym)) {
          ++stored;
        } else {
          ++skipped;
        }
      });
    }
  });
  return {stored, skipped};
}

std::pair<int, int> AstIndexer::index_file(const ParsedTu &tu,
                                           const std::string &filename,
                                           int64_t file_id) {
  Transaction txn = db_.transaction(); // one txn per file (ast.py:142)
  const auto result = index_file_notxn(tu, filename, file_id);
  txn.commit(); // R2: explicit commit so a COMMIT failure is not swallowed
  return result;
}

int AstIndexer::index_symbols(const ParsedTu &tu, const std::string &filename,
                              int64_t file_id) {
  // ast.py:163-168 — the main file is tu.spelling (the path exactly as
  // passed to parse, G24); callers pass that same path.
  return index_file(tu, filename, file_id).first;
}

} // namespace cidx
