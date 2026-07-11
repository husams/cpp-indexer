// AST indexer — see ast.hpp. Line-level behavior is pinned to
// project/indexer/clang/ast.py (cited per function).
#include "clangx/ast.hpp"
#include "clangx/ast_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <exception>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include <sys/stat.h>

#include "clangx/clang_raii.hpp"
#include "util/env.hpp"
#include "util/hashing.hpp"
#include "util/pathutil.hpp"

namespace cidx::ast_detail {

// _KIND_MAP (ast.py:25-43): exactly 17 CursorKinds -> storage kinds; cursors
// of any other kind are ignored. The `macro` entry is unreachable today —
// parse() passes options=0, so no DETAILED_PREPROCESSING_RECORD, so
// MACRO_DEFINITION cursors never appear (G22/D19) — but it stays mapped for
// DB compatibility.
const char *kind_name(CXCursorKind kind) {
  switch (kind) {
  case CXCursor_ClassDecl:
    return "class";
  case CXCursor_StructDecl:
    return "struct";
  case CXCursor_UnionDecl:
    return "union";
  case CXCursor_FunctionDecl:
    return "function";
  case CXCursor_CXXMethod:
    return "method";
  case CXCursor_FieldDecl:
    return "member";
  case CXCursor_Constructor:
    return "constructor";
  case CXCursor_Destructor:
    return "destructor";
  case CXCursor_EnumDecl:
    return "enum";
  case CXCursor_EnumConstantDecl:
    return "enum-constant";
  case CXCursor_TypedefDecl:
    return "typedef";
  case CXCursor_TypeAliasDecl:
    return "type-alias";
  case CXCursor_ClassTemplate:
    return "class-template";
  case CXCursor_FunctionTemplate:
    return "function-template";
  case CXCursor_VarDecl:
    return "variable";
  case CXCursor_Namespace:
    return "namespace";
  case CXCursor_MacroDefinition:
    return "macro";
  default:
    return nullptr;
  }
}

// Symbol kind for a minted stub, taken from its reference cursor so a defaulted
// ctor stub is 'constructor', not the bare 'function' fallback (used when the
// cursor maps to no storage kind). Mirrors ast.py's _KIND_MAP.get(k,"function").
std::string stub_kind(CXCursor c) {
  const char *k = kind_name(::clang_getCursorKind(c));
  return k != nullptr ? std::string(k) : std::string("function");
}

// _FUNCTION_KINDS (ast.py:53-59): indexed themselves, but their bodies are
// NOT walked (locals, body-scoped types, and statements are not file-scope
// symbols).
bool is_function_like(CXCursorKind kind) {
  return kind == CXCursor_FunctionDecl || kind == CXCursor_CXXMethod ||
         kind == CXCursor_Constructor || kind == CXCursor_Destructor ||
         kind == CXCursor_FunctionTemplate;
}

// Body-scoped named type declarations that ARE indexed as symbols even though
// they live inside a function/method body (or a local record within one) --
// mirrors ast.py:_LOCAL_SYMBOL_KINDS. Local VARIABLES are absent: they are
// reference-site sources only (index_edges body descent), not symbols. C++
// forbids block-scope templates and local-class static data members, so no
// template/variable kinds belong here. Every kind here is also a kind_name()
// key, so to_symbol maps it without special-casing.
bool is_local_symbol_kind(CXCursorKind kind) {
  return kind == CXCursor_TypedefDecl || kind == CXCursor_TypeAliasDecl ||
         kind == CXCursor_EnumDecl || kind == CXCursor_EnumConstantDecl ||
         kind == CXCursor_StructDecl || kind == CXCursor_ClassDecl ||
         kind == CXCursor_UnionDecl || kind == CXCursor_FieldDecl ||
         kind == CXCursor_CXXMethod || kind == CXCursor_Constructor ||
         kind == CXCursor_Destructor;
}

// Python's Cursor.from_result turns the null/invalid cursor into None; the
// invalid-kind range is the C-API equivalent (cursor walks stop there).
bool is_invalid_kind(CXCursorKind kind) {
  return kind >= CXCursor_FirstInvalid && kind <= CXCursor_LastInvalid;
}

// cursor.location expansion site: file handle + 1-based line/column.
ExpansionLoc cursor_location(CXCursor cursor) {
  ExpansionLoc loc;
  unsigned offset = 0;
  ::clang_getExpansionLocation(::clang_getCursorLocation(cursor), &loc.file,
                                 &loc.line, &loc.col, &offset);
  return loc;
}

// End of cursor.extent as an expansion site -- the closing '}' of a function or
// method definition, or the full extent of a class/struct/union/typedef decl.
// Uses clang_getExpansionLocation to match clang.cindex's extent.end.line/column
// (cindex resolves SourceLocation line/column via the expansion location).
ExpansionLoc cursor_extent_end(CXCursor cursor) {
  ExpansionLoc loc;
  unsigned offset = 0;
  ::clang_getExpansionLocation(
      ::clang_getRangeEnd(::clang_getCursorExtent(cursor)), &loc.file,
      &loc.line, &loc.col, &offset);
  return loc;
}

// Start of cursor.extent as an expansion site -- NOT cursor.location (which is
// the identifying spelling location, e.g. the class/function NAME). extent.start
// includes the leading class/struct/union/enum keyword and, for a function/
// method, its return type (and out-of-line qualifier, e.g. `Circle::`), so
// (line, col)..(end_line, end_col) slices the WHOLE declaration (ast.py mirror).
ExpansionLoc cursor_extent_start(CXCursor cursor) {
  ExpansionLoc loc;
  unsigned offset = 0;
  ::clang_getExpansionLocation(
      ::clang_getRangeStart(::clang_getCursorExtent(cursor)), &loc.file,
      &loc.line, &loc.col, &offset);
  return loc;
}

// Declaration location of a mint target's reference cursor. The target of a
// mint (callee/base/override/primary) carries a real source location even when
// its definition body is never separately indexed -- e.g. an implicit/defaulted
// ctor is anchored to its `struct` line. Recording it here is what lets
// chain::D::D resolve to chain.hpp:25 instead of `@<no-location>`.
//
// Lookup-only for the registered file id (db.get_file, never add_file_path). A
// target in a file no registered component owns -- system/stdlib headers -- has
// no file row, so `file_id` is nullopt; but the AST still knows where it is, so
// `path` carries the raw file path (with line/col). The stub then keeps that
// location instead of going `@<no-location>` (e.g. libstdc++
// __normal_iterator::operator* shows stl_iterator.h:NNNN). Only a cursor with no
// source location at all (implicit/builtin) yields all nullopt. Mirrors
// ast.py:_ref_decl_loc.
RefDeclLoc ref_decl_loc(Storage &db, CXCursor ref) {
  RefDeclLoc out;
  const ExpansionLoc loc = cursor_location(ref);
  if (loc.file == nullptr) {
    return out;
  }
  const std::string fname = CxString(::clang_getFileName(loc.file)).str();
  if (fname.empty()) {
    return out;
  }
  out.line = static_cast<int64_t>(loc.line);
  out.col = static_cast<int64_t>(loc.col);
  const auto row = db.get_file(fname);
  if (!row) {
    out.path = fname; // unregistered (system/stdlib) header: keep the raw path
    return out;
  }
  out.file_id = row->id;
  return out;
}

// Strip pointer/reference/array layers off `t` and return the declaration
// cursor of the named type it spells, or the null cursor when the type has no
// user declaration (builtins like int, function pointers, …). Single-level by
// design: resolves the type as WRITTEN (a typedef alias stays the alias),
// mirroring ast.py:_named_type_decl.
CXCursor named_type_decl(CXType t) {
  for (int i = 0; i < 32; ++i) {            // guard against pathological nesting
    const CXTypeKind tk = t.kind;
    if (tk == CXType_Pointer || tk == CXType_LValueReference ||
        tk == CXType_RValueReference) {
      t = ::clang_getPointeeType(t);
    } else if (tk == CXType_ConstantArray || tk == CXType_IncompleteArray ||
               tk == CXType_VariableArray || tk == CXType_DependentSizedArray) {
      t = ::clang_getArrayElementType(t);
    } else {
      break;
    }
  }
  const CXCursor decl = ::clang_getTypeDeclaration(t);
  if (::clang_Cursor_isNull(decl) ||
      is_invalid_kind(::clang_getCursorKind(decl))) {
    return ::clang_getNullCursor();
  }
  return decl;
}

// Emit a `uses` edge (kind=7) src -> the record/enum/typedef named by `ctype`
// (parameter, return, field, variable, or typedef-underlying type), grounded
// at `loc_cursor`'s location. Lookup-only like body descent (the type's symbol
// must already be indexed, so builtins/unindexed stdlib types create neither
// edges nor stubs); no self-edge. Mirrors ast.py:_emit_type_use.
void emit_type_use(Storage &db, int64_t src_id, CXType ctype,
                   int64_t file_id, CXCursor loc_cursor, int conditional) {
  const CXCursor decl = named_type_decl(ctype);
  if (::clang_Cursor_isNull(decl)) {
    return;
  }
  const std::string usr = CxString(::clang_getCursorUSR(decl)).str();
  if (usr.empty()) {
    return;
  }
  const auto dst = db.lookup_symbol(usr);
  if (!dst || dst->id == src_id) {
    return;
  }
  Edge e;
  e.src_id = src_id;
  e.dst_id = dst->id;
  e.kind = 7; // uses
  e.count = 1;
  const int64_t edge_id = db.add_edge(e);

  const ExpansionLoc loc = cursor_location(loc_cursor);
  if (loc.line != 0) {
    EdgeSite site;
    site.edge_id = edge_id;
    site.file_id = file_id;
    site.line = static_cast<int64_t>(loc.line);
    site.col = static_cast<int64_t>(loc.col);
    site.conditional = conditional;
    db.add_edge_site(site);
  }
}

// v26: cursor kinds that establish an enclosing SYMBOL for a namespace
// reference -- the nearest such ancestor of a NAMESPACE_REF is the uses-edge
// source. Mirrors ast.py _SCOPE_KINDS.
bool is_scope_kind(CXCursorKind ck) {
  switch (ck) {
  case CXCursor_FunctionDecl:
  case CXCursor_CXXMethod:
  case CXCursor_Constructor:
  case CXCursor_Destructor:
  case CXCursor_FunctionTemplate:
  case CXCursor_ClassDecl:
  case CXCursor_StructDecl:
  case CXCursor_UnionDecl:
  case CXCursor_ClassTemplate:
  case CXCursor_Namespace:
  case CXCursor_VarDecl:
  case CXCursor_FieldDecl:
  case CXCursor_EnumDecl:
  case CXCursor_TypedefDecl:
  case CXCursor_TypeAliasDecl:
    return true;
  default:
    return false;
  }
}

// Collect a cursor's IMMEDIATE children (CXChildVisit_Continue = no recursion),
// so ns_uses_descend can recurse manually with a per-node enclosing id.
struct NsChildCollector {
  std::vector<CXCursor> *out = nullptr;
};
CXChildVisitResult ns_collect_visitor(CXCursor c, CXCursor /*parent*/,
                                      CXClientData data) noexcept {
  static_cast<NsChildCollector *>(data)->out->push_back(c);
  return CXChildVisit_Continue;
}

// Recursive descent for namespace `uses` edges -- mirrors ast.py
// _emit_namespace_uses' inner descend(). Tracks the nearest enclosing indexed
// symbol id (-1 = none) and, for each main-file NAMESPACE_REF to an indexed
// namespace, emits a uses(7) edge enclosing->namespace + one edge_site. DESCENDS
// INTO BODIES (unlike for_file_cursors), so a `geo::` qualifier inside a
// function body is attributed to that function. Only main-file refs to an
// INDEXED namespace are recorded (bare `std::` -> unindexed c:@N@std -> lookup
// miss -> skipped), mirroring the lookup-only discipline of the other passes.
void ns_uses_descend(Storage &db, CXCursor cursor,
                     int64_t enclosing_id, const std::string &filename,
                     int64_t file_id) {
  std::vector<CXCursor> children;
  NsChildCollector cc;
  cc.out = &children;
  ::clang_visitChildren(cursor, &ns_collect_visitor, &cc);
  for (const CXCursor &child : children) {
    const ExpansionLoc loc = cursor_location(child);
    if (loc.file == nullptr) {
      continue; // from no file: skip subtree
    }
    CXString fname_cx = ::clang_getFileName(loc.file);
    CxString fname_raii(fname_cx);
    const char *raw = ::clang_getCString(fname_cx);
    if (raw == nullptr || std::strcmp(raw, filename.c_str()) != 0) {
      continue; // from another file: skip subtree
    }
    const CXCursorKind ck = ::clang_getCursorKind(child);
    if (ck == CXCursor_NamespaceRef) {
      if (enclosing_id >= 0) {
        const CXCursor ref = ::clang_getCursorReferenced(child);
        if (!::clang_Cursor_isNull(ref)) {
          const std::string nusr =
              CxString(::clang_getCursorUSR(ref)).str();
          if (!nusr.empty()) {
            const auto nsym = db.lookup_symbol(nusr);
            if (nsym && nsym->id != enclosing_id) {
              Edge e;
              e.src_id = enclosing_id;
              e.dst_id = nsym->id;
              e.kind = 7; // uses
              e.count = 1;
              const int64_t edge_id = db.add_edge(e);
              if (loc.line != 0) {
                EdgeSite site;
                site.edge_id = edge_id;
                site.file_id = file_id;
                site.line = static_cast<int64_t>(loc.line);
                site.col = static_cast<int64_t>(loc.col);
                site.conditional = 0;
                db.add_edge_site(site);
              }
            }
          }
        }
      }
      continue; // NAMESPACE_REF has no children worth walking
    }
    int64_t new_enclosing = enclosing_id;
    if (is_scope_kind(ck)) {
      const std::string usr = CxString(::clang_getCursorUSR(child)).str();
      if (!usr.empty()) {
        const auto s = db.lookup_symbol(usr);
        if (s) {
          new_enclosing = s->id;
        }
      }
    }
    ns_uses_descend(db, child, new_enclosing, filename, file_id);
  }
}

// B3 driver: mirror ast.py _emit_namespace_uses(db, tu, filename, file_id).
void emit_namespace_uses(Storage &db, const ParsedTu &tu,
                         const std::string &filename, int64_t file_id) {
  ns_uses_descend(db, ::clang_getTranslationUnitCursor(tu.tu), -1,
                  filename, file_id);
}

// _linkage (ast.py:77-79) via the explicit D13 table — the stored spellings
// are DB content shared with Python-written rows.
std::optional<std::string> linkage_name(CXLinkageKind linkage) {
  switch (linkage) {
  case CXLinkage_NoLinkage:
    return std::string("no-linkage");
  case CXLinkage_Internal:
    return std::string("internal");
  case CXLinkage_UniqueExternal:
    return std::string("unique-external");
  case CXLinkage_External:
    return std::string("external");
  case CXLinkage_Invalid:
  default:
    return std::nullopt; // INVALID -> NULL
  }
}

// _ACCESS (ast.py:45-49) via the D13 table; invalid/none -> NULL.
std::optional<std::string> access_name(CX_CXXAccessSpecifier access) {
  switch (access) {
  case CX_CXXPublic:
    return std::string("public");
  case CX_CXXProtected:
    return std::string("protected");
  case CX_CXXPrivate:
    return std::string("private");
  default:
    return std::nullopt;
  }
}

// '_qualified_name' (ast.py:82-91): 'ns::Class::name' built from SEMANTIC
// parents, so an out-of-line method definition is qualified by its class,
// not the file scope it sits in. Anonymous levels (empty spelling) are
// skipped (G25).
std::string qualified_name(CXCursor cursor) {
  std::vector<std::string> parts;
  CXCursor c = cursor;
  while (true) {
    const CXCursorKind kind = ::clang_getCursorKind(c);
    if (is_invalid_kind(kind) || kind == CXCursor_TranslationUnit) {
      break;
    }
    std::string spelling = CxString(::clang_getCursorSpelling(c)).str();
    if (!spelling.empty()) {
      parts.push_back(std::move(spelling));
    }
    c = ::clang_getCursorSemanticParent(c);
  }
  std::string out;
  for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
    if (!out.empty()) {
      out += "::";
    }
    out += *it;
  }
  return out;
}

// Visitor context for for_file_cursors. Callbacks into libclang are noexcept
// (D23): a C++ exception thrown by fn is stashed here, the walk is Break-ed,
// and the exception is rethrown after clang_visitChildren returns.
struct WalkCtx {
  const std::string *filename = nullptr;
  const std::function<void(CXCursor)> *fn = nullptr;
  std::exception_ptr error;
};

CXChildVisitResult walk_visitor(CXCursor cursor, CXCursor /*parent*/,
                                CXClientData data) noexcept {
  auto *ctx = static_cast<WalkCtx *>(data);
  try {
    const ExpansionLoc loc = cursor_location(cursor);
    if (loc.file == nullptr) {
      return CXChildVisit_Continue; // cursor from no file: skip subtree
    }
    // R11: compare the raw C string BEFORE constructing any std::string to
    // avoid a heap allocation per visited cursor (including pruned ones).
    // CxString RAII ensures clang_disposeString is called in all paths.
    CXString fname_cx = ::clang_getFileName(loc.file);
    CxString fname_raii(fname_cx);
    const char *raw = ::clang_getCString(fname_cx);
    if (raw == nullptr || std::strcmp(raw, ctx->filename->c_str()) != 0) {
      return CXChildVisit_Continue; // cursor from another file: skip subtree
    }
    (*ctx->fn)(cursor);
    return is_function_like(::clang_getCursorKind(cursor))
               ? CXChildVisit_Continue // body not walked
               : CXChildVisit_Recurse;
  } catch (...) {
    ctx->error = std::current_exception();
    return CXChildVisit_Break;
  }
}

// Parent-aware variant of WalkCtx: passes (cursor, parent) to fn.
// Used by index_edges so CXX_BASE_SPECIFIER handlers can get the enclosing
// record from the walk parent (spec §1.4 gotcha: semantic_parent is NULL).
struct WalkCtxP {
  const std::string *filename = nullptr;
  const std::function<void(CXCursor, CXCursor)> *fn = nullptr;
  std::exception_ptr error;
};

CXChildVisitResult walk_visitor_p(CXCursor cursor, CXCursor parent,
                                  CXClientData data) noexcept {
  auto *ctx = static_cast<WalkCtxP *>(data);
  try {
    const ExpansionLoc loc = cursor_location(cursor);
    if (loc.file == nullptr) {
      return CXChildVisit_Continue;
    }
    CXString fname_cx = ::clang_getFileName(loc.file);
    CxString fname_raii(fname_cx);
    const char *raw = ::clang_getCString(fname_cx);
    if (raw == nullptr || std::strcmp(raw, ctx->filename->c_str()) != 0) {
      return CXChildVisit_Continue;
    }
    (*ctx->fn)(cursor, parent);
    return is_function_like(::clang_getCursorKind(cursor))
               ? CXChildVisit_Continue
               : CXChildVisit_Recurse;
  } catch (...) {
    ctx->error = std::current_exception();
    return CXChildVisit_Break;
  }
}

// Visitor context for for_body_local_symbols. Mirrors ast.py:_body_local_symbols
// -- a depth-first walk of a function-like DEFINITION's subtree that streams
// only body-scoped named type declarations. Callbacks into libclang are
// noexcept (D23); the exception is stashed and rethrown by the caller.
struct BodyLocalCtx {
  const std::string *filename = nullptr;
  const std::function<void(CXCursor)> *fn = nullptr;
  std::exception_ptr error;
};

CXChildVisitResult body_local_visitor(CXCursor cursor, CXCursor /*parent*/,
                                      CXClientData data) noexcept {
  auto *ctx = static_cast<BodyLocalCtx *>(data);
  try {
    const ExpansionLoc loc = cursor_location(cursor);
    if (loc.file == nullptr) {
      return CXChildVisit_Continue;
    }
    CXString fname_cx = ::clang_getFileName(loc.file);
    CxString fname_raii(fname_cx);
    const char *raw = ::clang_getCString(fname_cx);
    if (raw == nullptr || std::strcmp(raw, ctx->filename->c_str()) != 0) {
      return CXChildVisit_Continue; // cursor from another file: skip subtree
    }
    if (is_local_symbol_kind(::clang_getCursorKind(cursor))) {
      (*ctx->fn)(cursor);
    }
    // Descend through EVERYTHING (mirrors ast.py's unconditional recursion), so
    // a local class in the body and the local declarations inside ITS methods'
    // bodies are all reached from one walk of the enclosing function.
    return CXChildVisit_Recurse;
  } catch (...) {
    ctx->error = std::current_exception();
    return CXChildVisit_Break;
  }
}

// Walk a function-like DEFINITION's body, streaming its body-scoped named type
// declarations to fn (ast.py:_body_local_symbols). Rethrows any error stashed
// by the noexcept visitor.
void for_body_local_symbols(CXCursor fn_cursor, const std::string &filename,
                            const std::function<void(CXCursor)> &fn) {
  BodyLocalCtx ctx;
  ctx.filename = &filename;
  ctx.fn = &fn;
  ::clang_visitChildren(fn_cursor, &body_local_visitor, &ctx);
  if (ctx.error) {
    std::rethrow_exception(ctx.error);
  }
}

// One transitive inclusion of the TU, copied out as plain data inside the
// (noexcept) inclusion visitor. The CXFile handle stays valid for the TU's
// lifetime and is only consulted while the ParsedTu is alive.
struct InclusionRec {
  CXFile file = nullptr;
  std::string name; // libclang's spelling of the included file (G23)
};

struct InclusionCtx {
  std::vector<InclusionRec> inclusions;
  std::exception_ptr error;
};

void inclusion_visitor(CXFile included_file, CXSourceLocation * /*stack*/,
                       unsigned include_len, CXClientData data) noexcept {
  auto *ctx = static_cast<InclusionCtx *>(data);
  if (include_len == 0) {
    return; // the main file itself (cindex `depth > 0` parity)
  }
  try {
    InclusionRec rec;
    rec.file = included_file;
    rec.name = CxString(::clang_getFileName(included_file)).str();
    ctx->inclusions.push_back(std::move(rec));
  } catch (...) {
    ctx->error = std::current_exception();
  }
}

// _ignore_system_headers (ast.py:171-174): default true; the exact falsy set
// {0,false,no,off} (env.hpp) turns system-header indexing on.
bool default_ignore_system_headers() {
  const std::optional<std::string> val = get_env(kIgnoreSystemHeadersEnv);
  return !env_flag_false_headers(val ? val->c_str() : nullptr);
}

// _is_system_header (ast.py:177-180): per-TU via clang_getLocation(tu, file,
// 1, 1) — honors the -isystem/sysroot of THIS parse (G26).
bool is_system_header(CXTranslationUnit tu, CXFile file) {
  const CXSourceLocation loc = ::clang_getLocation(tu, file, 1, 1);
  return ::clang_Location_isInSystemHeader(loc) != 0;
}

// os.path.getmtime(path) if os.path.exists(path) else None (ast.py:220):
// float seconds = sec + nsec * 1e-9.
std::optional<double> file_mtime(const std::string &path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) {
    return std::nullopt;
  }
#ifdef __APPLE__
  return static_cast<double>(st.st_mtimespec.tv_sec) +
         static_cast<double>(st.st_mtimespec.tv_nsec) * 1e-9;
#else
  return static_cast<double>(st.st_mtim.tv_sec) +
         static_cast<double>(st.st_mtim.tv_nsec) * 1e-9;
#endif
}

} // namespace cidx::ast_detail

namespace cidx {
using namespace ast_detail;

void AstIndexer::for_file_cursors(const ParsedTu &tu,
                                  const std::string &filename,
                                  const std::function<void(CXCursor)> &fn) {
  WalkCtx ctx;
  ctx.filename = &filename;
  ctx.fn = &fn;
  ::clang_visitChildren(::clang_getTranslationUnitCursor(tu.tu),
                          &walk_visitor, &ctx);
  if (ctx.error) {
    std::rethrow_exception(ctx.error);
  }
}

void AstIndexer::for_file_cursors_p(
    const ParsedTu &tu, const std::string &filename,
    const std::function<void(CXCursor, CXCursor)> &fn) {
  WalkCtxP ctx;
  ctx.filename = &filename;
  ctx.fn = &fn;
  ::clang_visitChildren(::clang_getTranslationUnitCursor(tu.tu),
                          &walk_visitor_p, &ctx);
  if (ctx.error) {
    std::rethrow_exception(ctx.error);
  }
}

HeaderStats AstIndexer::index_headers(
    const ParsedTu &tu, const std::optional<bool> &ignore_system,
    const std::optional<std::vector<std::string>> &header_options,
    const std::optional<std::string> &header_driver) {
  const bool ignore =
      ignore_system ? *ignore_system : default_ignore_system_headers();

  // tu.get_includes() parity: collect the transitive inclusion list as plain
  // data first (cindex does the same), then do all DB work outside the C
  // callback. A header included twice appears twice; dedupe below.
  InclusionCtx ctx;
  ::clang_getInclusions(tu.tu, &inclusion_visitor, &ctx);
  if (ctx.error) {
    std::rethrow_exception(ctx.error);
  }

  // Two passes over this TU's headers. A header may reference a symbol
  // declared in a header it includes (which appears LATER in include order)
  // -- e.g. a function template whose body calls a member function template in
  // a deeper header. That call is dependent/recovered and only LINKS to an
  // already-indexed target (no stub is minted), so the target symbol must
  // already exist. Pass 1 mints symbols for every not-yet-indexed header;
  // pass 2 then extracts edges with all header symbols present.
  struct PendingHeader {
    std::string inc_name;   // inclusion spelling (for cursor matching, G23)
    int64_t file_id = -1;
    std::optional<double> mtime;
    int stored = 0;
  };

  HeaderStats counts;
  std::unordered_set<std::string> seen;
  std::vector<PendingHeader> pending;

  // Pass 1: mint symbols for every not-yet-indexed header.
  for (const InclusionRec &inc : ctx.inclusions) {
    const std::string path = pathutil::abspath(inc.name);
    if (!seen.insert(path).second) {
      continue;
    }
    if (ignore && is_system_header(tu.tu, inc.file)) {
      ++counts.system;
      continue;
    }
    if (!db_.component_for_path(path)) {
      ++counts.unowned;
      continue;
    }
    const std::optional<std::string> md5 = md5_of(path);
    if (db_.is_file_indexed(path, std::nullopt, md5)) {
      ++counts.already;
      continue;
    }
    const std::optional<double> mtime = file_mtime(path);
    // Stamp the header with the including TU's (encoded) options + driver so
    // it is standalone-reparseable with full -I/-std/-D context, mirroring TU
    // rows (decoded at parse time).
    const int64_t file_id =
        db_.add_file_path(path, mtime, md5, header_options, header_driver);
    // Extract this header's symbols out of THIS TU's AST (no separate
    // parse), matching cursors against the include SPELLING, not the
    // abspath (G23: cursors' location-file names agree with the spelling).
    std::pair<int, int> result;
    {
      Transaction txn = db_.transaction();
      result = index_file_notxn(tu, inc.name, file_id);
      txn.commit();
    }
    pending.push_back({inc.name, file_id, mtime, result.first});
  }

  // Pass 2: extract edges for those headers, now that every header symbol is
  // in the DB (QD-1). Symbol rows must all exist before edge extraction begins.
  for (const PendingHeader &ph : pending) {
    {
      Transaction txn = db_.transaction();
      if (graph_enabled_) {
        db_.delete_edges_for_file(ph.file_id);
        db_.delete_definitions_for_file(ph.file_id); // v27: cascades def_edge
        index_edges_notxn(tu, ph.inc_name, ph.file_id);
      }
      txn.commit();
    }
    db_.mark_file_indexed(ph.file_id, ph.mtime);
    ++counts.indexed;
    counts.symbols += ph.stored;
  }
  return counts;
}

} // namespace cidx
