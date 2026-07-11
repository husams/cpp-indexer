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
#include <vector>

#include "clangx/clang_raii.hpp"

namespace cidx::ast_detail {

// Context for the recursive body descent (calls + uses).
struct BodyDescentCtx {
  Storage *db = nullptr;
  int64_t src_id = -1;
  int64_t file_id = -1;
  int cond_depth = 0;
  std::string owner_usr; // USR of the enclosing method's owning record (empty
                         // for free fns); self-owner skip in TYPE_REF branch.
  std::exception_ptr error;
};

// FirstChildCtx + first_child_visitor are shared by the Phase 2 helpers below
// and by recover_overloaded_callee (dependent-call recovery).
struct FirstChildCtx {
  CXCursor out;
  bool found = false;
};
CXChildVisitResult first_child_visitor(CXCursor c, CXCursor /*parent*/,
                                       CXClientData d) noexcept {
  auto *x = static_cast<FirstChildCtx *>(d);
  x->out = c;
  x->found = true;
  return CXChildVisit_Break; // callee is the FIRST child; args follow
}

// ---------------------------------------------------------------------------
// Phase 2: value-source classification helpers (mirrors ast.py)
// ---------------------------------------------------------------------------

// Peel implicit casts, parentheses, and address-of/dereference from `expr`
// to the underlying named sub-expression. At most 16 layers.
// Mirrors ast.py:_peel_expr.
CXCursor peel_expr(CXCursor expr) {
  for (int i = 0; i < 16; ++i) {
    const CXCursorKind k = ::clang_getCursorKind(expr);
    // PAREN_EXPR (111), UNARY_OPERATOR (112), CSTYLE_CAST_EXPR (117)
    // UNEXPOSED_EXPR (0/1) covers implicit casts
    if (k == CXCursor_ParenExpr || k == CXCursor_UnaryOperator ||
        k == (CXCursorKind)117 /*CStyleCast*/ ||
        k == CXCursor_UnexposedExpr || k == (CXCursorKind)1) {
      FirstChildCtx fc{};
      ::clang_visitChildren(expr, &first_child_visitor, &fc);
      if (fc.found) {
        expr = fc.out;
        continue;
      }
    }
    break;
  }
  return expr;
}

// Strip pointer/reference/cv-qualifiers from `t` and return the USR of the
// record declaration, or "" when there is none (builtins).
// Mirrors ast.py:_record_usr_of_type.
std::string record_usr_of_type(CXType t) {
  // clang_getCanonicalType is not wrapped in LibClang; call the C API directly.
  CXType canonical = ::clang_getCanonicalType(t);
  for (int i = 0; i < 8; ++i) {
    const CXTypeKind tk = canonical.kind;
    if (tk == CXType_Pointer || tk == CXType_LValueReference ||
        tk == CXType_RValueReference) {
      canonical = ::clang_getCanonicalType(::clang_getPointeeType(canonical));
    } else {
      break;
    }
  }
  const CXCursor decl = ::clang_getTypeDeclaration(canonical);
  if (::clang_Cursor_isNull(decl) ||
      is_invalid_kind(::clang_getCursorKind(decl))) {
    return "";
  }
  return CxString(::clang_getCursorUSR(decl)).str();
}

// Mirrors ast.py:_type_is_value. True iff `loc_type` holds `dispatch_record_usr`
// by value (exact, non-erased). Sound: pointer/ref fail the RECORD kind gate;
// a handle's wrapper USR never equals the dispatch USR.
bool type_is_value(CXType loc_type,
                   const std::string &dispatch_record_usr) {
  if (dispatch_record_usr.empty()) return false;
  CXType c = ::clang_getCanonicalType(loc_type);
  if (c.kind != CXType_Record) return false;
  const CXCursor decl = ::clang_getTypeDeclaration(c);
  if (::clang_Cursor_isNull(decl) ||
      is_invalid_kind(::clang_getCursorKind(decl))) {
    return false;
  }
  return CxString(::clang_getCursorUSR(decl)).str() == dispatch_record_usr;
}

// Mirrors ast.py:_decl_type_for_expr.
// Returns the DECLARED type of the value source, not the use-site expression type.
// libclang auto-derefs lvalue-references at the call-site: a field "B& br" presents
// as expression-type B.  For DECL_REF_EXPR / MEMBER_REF_EXPR we read
// getCursorType(getCursorReferenced(peeled)) which preserves the reference.
// For CALL_EXPR (call_result) we use getCursorResultType of the callee.
// Falls back to getCursorType(peeled) for anything else (safe).
static CXType decl_type_for_expr(CXCursor peeled) {
  const CXCursorKind k = ::clang_getCursorKind(peeled);
  if (k == CXCursor_DeclRefExpr || k == CXCursor_MemberRefExpr) {
    const CXCursor ref = ::clang_getCursorReferenced(peeled);
    if (::clang_Cursor_isNull(ref)) {
      return ::clang_getCursorType(peeled);
    }
    return ::clang_getCursorType(ref);
  }
  if (k == CXCursor_CallExpr || k == (CXCursorKind)128 /*CXXFunctionalCast*/) {
    const CXCursor ref = ::clang_getCursorReferenced(peeled);
    if (::clang_Cursor_isNull(ref)) {
      return ::clang_getCursorType(peeled);
    }
    return ::clang_getCursorResultType(ref);
  }
  return ::clang_getCursorType(peeled);
}

// Result of classify_value_source — mirrors the Python tuple.
struct ValueSource {
  std::string src_kind;               // local|construct|member|global|call_result|literal|this|unknown
  std::string type_usr;               // "" = none
  std::string decl_usr;               // "" = none
  std::string callee_usr;             // "" = none (call_result only)
};

// Classify the provenance of a value expression.
// Mirrors ast.py:_classify_value_source.
ValueSource classify_value_source(CXCursor expr) {
  const CXCursor peeled = peel_expr(expr);
  const CXCursorKind k = ::clang_getCursorKind(peeled);

  // CXXThisExpr (132)
  if (k == CXCursor_CXXThisExpr) {
    const std::string tu = record_usr_of_type(::clang_getCursorType(peeled));
    return {"this", tu, tu, ""};
  }

  // DECL_REF_EXPR (101)
  if (k == CXCursor_DeclRefExpr) {
    const CXCursor ref = ::clang_getCursorReferenced(peeled);
    if (::clang_Cursor_isNull(ref)) {
      return {"unknown", "", "", ""};
    }
    const CXCursorKind ref_kind = ::clang_getCursorKind(ref);
    const std::string decl_usr = CxString(::clang_getCursorUSR(ref)).str();
    const std::string type_usr = record_usr_of_type(::clang_getCursorType(peeled));
    if (ref_kind == CXCursor_ParmDecl) {
      return {"local", type_usr, decl_usr, ""};
    }
    if (ref_kind == CXCursor_VarDecl) {
      const CXCursor parent = ::clang_getCursorSemanticParent(ref);
      const CXCursorKind pk = ::clang_getCursorKind(parent);
      if (pk == CXCursor_FunctionDecl || pk == CXCursor_CXXMethod ||
          pk == CXCursor_Constructor || pk == CXCursor_Destructor ||
          pk == (CXCursorKind)144 /*LambdaExpr*/) {
        return {"local", type_usr, decl_usr, ""};
      }
      return {"global", type_usr, decl_usr, ""};
    }
    return {"unknown", type_usr, decl_usr, ""};
  }

  // MEMBER_REF_EXPR (102)
  if (k == CXCursor_MemberRefExpr) {
    const CXCursor ref = ::clang_getCursorReferenced(peeled);
    const std::string decl_usr = ::clang_Cursor_isNull(ref) ? "" :
        CxString(::clang_getCursorUSR(ref)).str();
    const std::string type_usr = record_usr_of_type(::clang_getCursorType(peeled));
    return {"member", type_usr, decl_usr, ""};
  }

  // CALL_EXPR (103) or CXXFunctionalCastExpr (128)
  if (k == CXCursor_CallExpr || k == (CXCursorKind)128 /*CXXFunctionalCast*/) {
    const CXCursor ref = ::clang_getCursorReferenced(peeled);
    if (!::clang_Cursor_isNull(ref)) {
      const CXCursorKind ref_kind = ::clang_getCursorKind(ref);
      if (ref_kind == CXCursor_Constructor ||
          ref_kind == (CXCursorKind)26 /*ConversionFunction*/) {
        const std::string type_usr = record_usr_of_type(::clang_getCursorType(peeled));
        return {"construct", type_usr, "", ""};
      }
    }
    const std::string type_usr = record_usr_of_type(::clang_getCursorType(peeled));
    const std::string callee_usr = ::clang_Cursor_isNull(ref) ? "" :
        CxString(::clang_getCursorUSR(ref)).str();
    return {"call_result", type_usr, "", callee_usr};
  }

  // CXXNewExpr (134)
  if (k == (CXCursorKind)134) {
    const std::string type_usr = record_usr_of_type(::clang_getCursorType(peeled));
    return {"construct", type_usr, "", ""};
  }

  // Literals: INTEGER_LITERAL(106) FLOATING_LITERAL(107) STRING_LITERAL(109)
  //           CHARACTER_LITERAL(110) CXXBoolLiteralExpr(130) CXXNullPtrLiteralExpr(131)
  //           GNUNullExpr(123)
  if (k == CXCursor_IntegerLiteral || k == CXCursor_FloatingLiteral ||
      k == CXCursor_StringLiteral || k == CXCursor_CharacterLiteral ||
      k == (CXCursorKind)130 /*CXXBoolLiteral*/ ||
      k == (CXCursorKind)131 /*CXXNullPtrLiteral*/ ||
      k == (CXCursorKind)123 /*GNUNullExpr*/) {
    return {"literal", "", "", ""};
  }

  return {"unknown", "", "", ""};
}

// Return the Layer-0 edge kind for a constructor call: copy(13)/move(14)/value(10).
// Mirrors ast.py:_ctor_form_kind.
// Inspects the ctor declaration's single parameter type spelling:
//   "&&"  -> move (14); "&" alone -> copy (13); else -> value (10).
static int ctor_form_kind(CXCursor ctor_cursor) {
  // Collect PARM_DECL children of the ctor declaration.
  struct ParmCtx {
    std::string first_param_type;
    int count = 0;
  } pctx;
  ::clang_visitChildren(
      ctor_cursor,
      [](CXCursor c, CXCursor /*parent*/, CXClientData data) {
        auto *ctx = static_cast<ParmCtx *>(data);
        if (::clang_getCursorKind(c) == CXCursor_ParmDecl) {
          ++ctx->count;
          if (ctx->count == 1) {
            ctx->first_param_type =
                CxString(::clang_getTypeSpelling(::clang_getCursorType(c)))
                    .str();
          }
        }
        return CXChildVisit_Continue;
      },
      &pctx);
  if (pctx.count == 1) {
    const std::string &pt = pctx.first_param_type;
    if (pt.find("&&") != std::string::npos) {
      return 14; // construct-move
    }
    if (pt.find('&') != std::string::npos) {
      return 13; // construct-copy
    }
  }
  return 10; // construct-value
}

// Return the receiver sub-expression of a C++ member call (the base object),
// or a null cursor for free-function calls or no-receiver implicit-this calls.
// Mirrors ast.py:_receiver_subexpr.
CXCursor receiver_subexpr(CXCursor call) {
  FirstChildCtx fc{};
  ::clang_visitChildren(call, &first_child_visitor, &fc);
  if (!fc.found) {
    return ::clang_getNullCursor();
  }
  const CXCursor peeled_first = peel_expr(fc.out);
  if (::clang_getCursorKind(peeled_first) == CXCursor_MemberRefExpr) {
    // The receiver is the MEMBER_REF_EXPR's first child
    FirstChildCtx mc{};
    ::clang_visitChildren(peeled_first, &first_child_visitor, &mc);
    if (mc.found) {
      return mc.out;
    }
    // Implicit this — no explicit child
    return ::clang_getNullCursor();
  }
  return ::clang_getNullCursor();
}

// ---------------------------------------------------------------------------

// Emit a calls or uses edge_site for a cursor inside a body descent.
// kind_id: 1=calls, 7=uses. The edge is upserted (ON CONFLICT increments
// count); the site is OR IGNORE (same site visited twice is one row).
// Returns the stable edge.id for further linkage (call_arg rows).
int64_t emit_body_edge(BodyDescentCtx *ctx, CXCursor cursor,
                       int64_t dst_id, int kind_id) {
  Edge e;
  e.src_id = ctx->src_id;
  e.dst_id = dst_id;
  e.kind = kind_id;
  e.count = 1;
  const int64_t edge_id = ctx->db->add_edge(e);

  unsigned line = 0;
  unsigned col = 0;
  unsigned offset = 0;
  CXFile file_handle = nullptr;
  ::clang_getExpansionLocation(::clang_getCursorLocation(cursor),
                                 &file_handle, &line, &col, &offset);
  EdgeSite site;
  site.edge_id = edge_id;
  site.file_id = ctx->file_id;
  site.line = static_cast<int64_t>(line);
  site.col = static_cast<int64_t>(col);
  site.conditional = ctx->cond_depth > 0 ? 1 : 0;
  ctx->db->add_edge_site(site);
  return edge_id;
}

// Emit a calls edge with Phase 2/3 receiver provenance on the edge_site.
// Returns the edge.id (needed for add_call_arg).
int64_t emit_call_edge(BodyDescentCtx *ctx, CXCursor cursor,
                       int64_t dst_id,
                       const std::string &recv_src_kind,
                       const std::string &recv_type_usr,
                       const std::string &recv_decl_usr,
                       std::optional<int64_t> recv_param_pos = std::nullopt,
                       std::optional<int64_t> recv_type_is_value = std::nullopt) {
  Edge e;
  e.src_id = ctx->src_id;
  e.dst_id = dst_id;
  e.kind = 1; // calls
  e.count = 1;
  const int64_t edge_id = ctx->db->add_edge(e);

  unsigned line = 0;
  unsigned col = 0;
  unsigned offset = 0;
  CXFile file_handle = nullptr;
  ::clang_getExpansionLocation(::clang_getCursorLocation(cursor),
                                 &file_handle, &line, &col, &offset);
  EdgeSite site;
  site.edge_id = edge_id;
  site.file_id = ctx->file_id;
  site.line = static_cast<int64_t>(line);
  site.col = static_cast<int64_t>(col);
  site.conditional = ctx->cond_depth > 0 ? 1 : 0;
  if (!recv_src_kind.empty()) {
    site.recv_src_kind = recv_src_kind;
  }
  if (!recv_type_usr.empty()) {
    site.recv_type_usr = recv_type_usr;
  }
  if (!recv_decl_usr.empty()) {
    site.recv_decl_usr = recv_decl_usr;
  }
  if (recv_param_pos.has_value()) {
    site.recv_param_pos = recv_param_pos;
  }
  if (recv_type_is_value.has_value()) {
    site.recv_type_is_value = recv_type_is_value;
  }
  ctx->db->add_edge_site(site);
  return edge_id;
}

// Emit call_arg rows for all non-literal positional args of a CALL_EXPR.
// Mirrors the Phase 2 loop in ast.py:_body_descent.
void emit_call_args(BodyDescentCtx *ctx, CXCursor call,
                    int64_t edge_id, unsigned line, unsigned col) {
  const int nargs = ::clang_Cursor_getNumArguments(call);
  for (int pos = 0; pos < nargs; ++pos) {
    const CXCursor arg = ::clang_Cursor_getArgument(call, static_cast<unsigned>(pos));
    if (::clang_Cursor_isNull(arg)) {
      continue;
    }
    const ValueSource vs = classify_value_source(arg);
    if (vs.src_kind == "literal") {
      continue;
    }
    CallArg ca;
    ca.edge_id = edge_id;
    ca.file_id = ctx->file_id;
    ca.line = static_cast<int64_t>(line);
    ca.col = static_cast<int64_t>(col);
    ca.position = static_cast<int64_t>(pos);
    ca.src_kind = vs.src_kind;
    if (!vs.type_usr.empty()) {
      ca.type_usr = vs.type_usr;
    }
    if (!vs.decl_usr.empty()) {
      ca.decl_usr = vs.decl_usr;
    }
    if (!vs.callee_usr.empty()) {
      ca.callee_usr = vs.callee_usr;
    }
    // Phase 3a: compute type_is_value for value-eligible arg kinds,
    // including "local" (to distinguish value-typed locals from param re-passing).
    // Use the declared type of the underlying decl (not the use-site expr type
    // which auto-derefs lvalue-references in libclang).
    if ((vs.src_kind == "member" || vs.src_kind == "global" ||
         vs.src_kind == "call_result" || vs.src_kind == "local") && !vs.type_usr.empty()) {
      const CXCursor peeled_arg = peel_expr(arg);
      ca.type_is_value = type_is_value(decl_type_for_expr(peeled_arg),
                                       vs.type_usr) ? 1 : 0;
    }
    ctx->db->add_call_arg(ca);
  }
}

// --- dependent-call recovery ------------------------------------------------
// A call to a dependent/overloaded name inside a template body (e.g.
// `combine(a, b)` in Stack<T>::summary) has a null getCursorReferenced, even
// though the call IS present in the AST. The callee sub-expression still carries
// an OverloadedDeclRef listing the candidate declarations. When that set names
// exactly ONE declaration we recover the callee; ambiguous sets (stdlib
// to_string, etc.) are left unresolved so we never guess a wrong target.
// Note: FirstChildCtx + first_child_visitor defined earlier (above Phase 2 helpers).
struct OverloadRefCtx {
  CXCursor out;
  bool found = false;
};
CXChildVisitResult overload_ref_visitor(CXCursor c, CXCursor /*parent*/,
                                        CXClientData d) noexcept {
  auto *x = static_cast<OverloadRefCtx *>(d);
  if (::clang_getCursorKind(c) == CXCursor_OverloadedDeclRef) {
    x->out = c;
    x->found = true;
    return CXChildVisit_Break;
  }
  return CXChildVisit_Recurse;
}

// All candidate declarations of a dependent/overloaded callee, or empty when
// the CALL_EXPR's first child carries no OverloadedDeclRef. Only the FIRST child
// (the callee position) is searched, so an argument that is itself an overloaded
// name is not mistaken for the callee. Mirror of Python
// _overload_set_candidates().
std::vector<CXCursor> overload_set_candidates(CXCursor call) {
  FirstChildCtx fc{};
  ::clang_visitChildren(call, &first_child_visitor, &fc);
  if (!fc.found) {
    return {};
  }
  CXCursor odr = ::clang_getNullCursor();
  if (::clang_getCursorKind(fc.out) == CXCursor_OverloadedDeclRef) {
    odr = fc.out;
  } else {
    OverloadRefCtx oc{{}, false};
    ::clang_visitChildren(fc.out, &overload_ref_visitor, &oc);
    if (oc.found) {
      odr = oc.out;
    }
  }
  if (::clang_Cursor_isNull(odr)) {
    return {};
  }
  const unsigned n = ::clang_getNumOverloadedDecls(odr);
  std::vector<CXCursor> out;
  out.reserve(n);
  for (unsigned i = 0; i < n; ++i) {
    out.push_back(::clang_getOverloadedDecl(odr, i));
  }
  return out;
}

// Returns the unique overloaded declaration, or a null cursor when the callee
// cannot be unambiguously recovered (ambiguous sets are handled by
// emit_overloaded_calls instead). Mirror of Python _recover_overloaded_callee().
CXCursor recover_overloaded_callee(CXCursor call) {
  const auto cands = overload_set_candidates(call);
  return cands.size() == 1 ? cands[0] : ::clang_getNullCursor();
}

// Emit `calls` edges for a dependent call whose overload set has MORE THAN one
// candidate (e.g. an overloaded member function template `cache.set(...)`
// invoked inside another template body). libclang cannot say which overload is
// selected, so the site is linked to every overload of that name -- a sound
// over-approximation for find-references / call-graph navigation. Each candidate
// USR is TU-invariant by contract, so a candidate not yet in the DB is given a
// USR-keyed stub (backfilled when its defining TU is indexed later), making the
// call order-independent. True system/stdlib candidates (ADL overload sets never
// separately indexed) are skipped so they do not become permanent unresolved
// externals. No receiver/argument provenance is recorded: that feeds
// virtual-dispatch devirt, and a function-template call is never a virtual
// dispatch. Mirror of Python _emit_overloaded_calls().
void emit_overloaded_calls(BodyDescentCtx *ctx, CXCursor call) {
  const auto cands = overload_set_candidates(call);
  if (cands.size() < 2) {
    return;
  }
  std::set<int64_t> dst_ids; // ordered + deduped
  for (const CXCursor &cand : cands) {
    const std::string usr = CxString(::clang_getCursorUSR(cand)).str();
    if (usr.empty()) {
      continue;
    }
    if (const auto s = ctx->db->lookup_symbol(usr)) {
      dst_ids.insert(s->id);
      continue;
    }
    // Not yet indexed: mint a USR-keyed stub so a later index backfills it,
    // but skip true system/stdlib overloads.
    if (::clang_Location_isInSystemHeader(
            ::clang_getCursorLocation(cand)) != 0) {
      continue;
    }
    const RefDeclLoc dl = ref_decl_loc(*ctx->db, cand);
    dst_ids.insert(ctx->db->mint_symbol_id(
        usr, CxString(::clang_getCursorSpelling(cand)).str(),
        qualified_name(cand),
        CxString(::clang_getCursorDisplayName(cand)).str(),
        stub_kind(cand), dl.file_id, dl.line, dl.col, dl.path));
  }
  if (dst_ids.empty()) {
    // Nothing resolved or minted (e.g. all candidates system/USR-less): fall
    // back to the shared qualified name + kind over indexed symbols.
    const CXCursor first = cands[0];
    const std::string qn = qualified_name(first);
    if (!qn.empty()) {
      const std::string sk = stub_kind(first);
      for (const auto &s : ctx->db->lookup_symbols_by_qual_name(qn, sk)) {
        dst_ids.insert(s.id);
      }
    }
  }
  if (dst_ids.empty()) {
    return;
  }
  unsigned line = 0;
  unsigned col = 0;
  unsigned offset = 0;
  CXFile file_handle = nullptr;
  ::clang_getExpansionLocation(::clang_getCursorLocation(call), &file_handle,
                                 &line, &col, &offset);
  for (const int64_t dst_id : dst_ids) {
    Edge e;
    e.src_id = ctx->src_id;
    e.dst_id = dst_id;
    e.kind = 1; // calls
    e.count = 1;
    const int64_t edge_id = ctx->db->add_edge(e);
    EdgeSite site;
    site.edge_id = edge_id;
    site.file_id = ctx->file_id;
    site.line = static_cast<int64_t>(line);
    site.col = static_cast<int64_t>(col);
    site.conditional = ctx->cond_depth > 0 ? 1 : 0;
    ctx->db->add_edge_site(site);
  }
}

// Link a callable specialization and, when applicable, mint its owner type.
// Mirror of Python _mint_instantiation_nodes().
//
// Always emits callable-specialization -> primary-template. For a class-template
// member instantiation (X<int>::method), also mints X<int>, attaches method_of to
// it, links X<int> -> X, and stores TYPE args on X<int>. For a method-template
// specialization on a non-template class (Context::register<MyType>), attaches
// method_of to the existing owner class without marking that class as an
// instantiation. Free-function specializations have no method_of edge.
void mint_instantiation_nodes(Storage &db,
                              const CXCursor &ref,
                              int64_t member_id,
                              int64_t prim_member_id) {
  // Callable specialization -> primary function/method template.
  Edge inst_b;
  inst_b.src_id = member_id;
  inst_b.dst_id = prim_member_id;
  inst_b.kind = 5; // instantiates
  inst_b.count = 1;
  db.add_edge(inst_b);

  const CXCursorKind ref_kind = ::clang_getCursorKind(ref);
  if (ref_kind != CXCursor_CXXMethod &&
      ref_kind != CXCursor_Constructor &&
      ref_kind != CXCursor_Destructor &&
      ref_kind != CXCursor_ConversionFunction) {
    return;
  }

  const CXCursor parent = ::clang_getCursorSemanticParent(ref);
  if (::clang_Cursor_isNull(parent) ||
      is_invalid_kind(::clang_getCursorKind(parent))) {
    return;
  }
  const CXCursorKind parent_cursor_kind = ::clang_getCursorKind(parent);
  if (parent_cursor_kind != CXCursor_ClassDecl &&
      parent_cursor_kind != CXCursor_StructDecl &&
      parent_cursor_kind != CXCursor_UnionDecl &&
      parent_cursor_kind != CXCursor_ClassTemplate &&
      parent_cursor_kind != CXCursor_ClassTemplatePartialSpecialization) {
    return;
  }
  const std::string type_usr =
      CxString(::clang_getCursorUSR(parent)).str();
  if (type_usr.empty()) {
    return;
  }

  const CXCursor class_primary =
      ::clang_getSpecializedCursorTemplate(parent);
  if (::clang_Cursor_isNull(class_primary) ||
      is_invalid_kind(::clang_getCursorKind(class_primary))) {
    if (const auto owner_sym = db.lookup_symbol(type_usr)) {
      Edge mo;
      mo.src_id = member_id;
      mo.dst_id = owner_sym->id;
      mo.kind = 9; // method_of
      mo.count = 1;
      db.add_edge(mo);
    }
    return;
  }
  const std::string class_prim_usr =
      CxString(::clang_getCursorUSR(class_primary)).str();
  if (class_prim_usr.empty() || class_prim_usr == type_usr) {
    if (const auto owner_sym = db.lookup_symbol(type_usr)) {
      Edge mo;
      mo.src_id = member_id;
      mo.dst_id = owner_sym->id;
      mo.kind = 9; // method_of
      mo.count = 1;
      db.add_edge(mo);
    }
    return;
  }

  const std::string parent_spelling =
      CxString(::clang_getCursorSpelling(parent)).str();
  const std::string parent_qual = qualified_name(parent);
  const std::string parent_display =
      CxString(::clang_getCursorDisplayName(parent)).str();
  const std::string parent_kind = stub_kind(parent);
  const RefDeclLoc tdl = ref_decl_loc(db, parent);
  const int64_t type_id = db.mint_symbol_id(
      type_usr, parent_spelling, parent_qual, parent_display,
      parent_kind, tdl.file_id, tdl.line, tdl.col, tdl.path,
      /*is_instantiation=*/true);

  // (d) method_of(9): member_id -> type_id
  Edge mo;
  mo.src_id = member_id;
  mo.dst_id = type_id;
  mo.kind = 9; // method_of
  mo.count = 1;
  db.add_edge(mo);

  // (e) instantiates(5): type_id -> class primary, guarded by primary indexed
  const auto class_prim_sym = db.lookup_symbol(class_prim_usr);
  if (class_prim_sym) {
    Edge inst_e;
    inst_e.src_id = type_id;
    inst_e.dst_id = class_prim_sym->id;
    inst_e.kind = 5; // instantiates
    inst_e.count = 1;
    db.add_edge(inst_e);
  }

  // (f) template_arg rows on TYPE node via clang_Type_getTemplateArgumentAsType
  // (TYPE args only -- same as VAR_DECL B3 pattern at ast.cpp:1280).
  // For a method template on a non-template owner we returned above; its explicit
  // args are stored on the callable specialization itself.
  const CXType parent_type = ::clang_getCursorType(parent);
  const int nargs = ::clang_Type_getNumTemplateArguments(parent_type);
  if (nargs <= 0) {
    return;
  }
  for (int ai = 0; ai < nargs; ++ai) {
    const CXType arg_type = ::clang_Type_getTemplateArgumentAsType(
        parent_type, static_cast<unsigned>(ai));
    TemplateArg ta;
    ta.owner_id = type_id;
    ta.position = static_cast<int64_t>(ai);
    ta.arg_kind = 1; // TYPE (only kind available via type API)
    const std::string spelling =
        CxString(::clang_getTypeSpelling(arg_type)).str();
    if (!spelling.empty()) {
      ta.literal = spelling;
    }
    // Try to resolve the arg type to an indexed symbol (ref_id FK).
    const CXCursor arg_decl = ::clang_getTypeDeclaration(arg_type);
    if (!::clang_Cursor_isNull(arg_decl) &&
        !is_invalid_kind(::clang_getCursorKind(arg_decl))) {
      const std::string ref_usr =
          CxString(::clang_getCursorUSR(arg_decl)).str();
      if (!ref_usr.empty()) {
        if (const auto rsym = db.lookup_symbol(ref_usr)) {
          ta.ref_id = rsym->id;
        }
      }
    }
    if (!ta.ref_id) {
      ta.ref_id = resolve_template_arg_ref_id(db, ta.literal, parent);
    }
    db.add_template_arg(ta);
  }
}

// Non-recursive entry point: visits all children via clang_visitChildren,
// capturing CALL_EXPR (calls) + DECL_REF_EXPR / MEMBER_REF_EXPR (uses)
// nodes and recursing depth-first.
CXChildVisitResult body_descent_visitor(CXCursor cursor, CXCursor parent,
                                        CXClientData data) noexcept {
  auto *ctx = static_cast<BodyDescentCtx *>(data);
  try {
    const CXCursorKind kind = ::clang_getCursorKind(cursor);

    if (kind == CXCursor_CallExpr) {
      // Get callee USR via getCursorReferenced. Mint-stub if not yet indexed.
      CXCursor ref = ::clang_getCursorReferenced(cursor);
      bool recovered = false;
      if (::clang_Cursor_isNull(ref)) {
        // Dependent/overloaded callee inside a template body (e.g.
        // `combine(a, b)` in Stack<T>::summary): recover it from the callee's
        // single-overload OverloadedDeclRef. See recover_overloaded_callee.
        ref = recover_overloaded_callee(cursor);
        recovered = !::clang_Cursor_isNull(ref);
        if (::clang_Cursor_isNull(ref)) {
          // Multi-candidate dependent overload set (e.g. an overloaded member
          // function template `cache.set(...)` called from another template
          // body): the single-overload recovery above declines it. Link the
          // site to every indexed overload. See emit_overloaded_calls.
          emit_overloaded_calls(ctx, cursor);
        }
      }
      if (!::clang_Cursor_isNull(ref)) {
        const std::string callee_usr =
            CxString(::clang_getCursorUSR(ref)).str();
        if (!callee_usr.empty()) {
          // Resolved calls mint a stub for an unindexed target. A RECOVERED
          // (dependent) call does the same -- a USR is TU-invariant by
          // contract, so a USR-keyed stub is backfilled when its defining TU is
          // indexed later, making the call order-independent. (An earlier
          // belief that libclang emits an inconsistent USR for dependent member
          // templates was a parse artifact: a fatal builtin-header miss
          // truncated `std::string` to a fallback type. With a complete parse
          // the call-site USR matches the declaration.)
          int64_t dst_id = -1;
          if (recovered) {
            if (const auto dst = ctx->db->lookup_symbol(callee_usr)) {
              dst_id = dst->id;
            }
            // USR not yet indexed: try the stable qualified name + kind first
            // (links to an already-present symbol when unambiguous), else mint a
            // USR-keyed stub for a later index to backfill.
            if (dst_id < 0) {
              const std::string qn = qualified_name(ref);
              if (!qn.empty()) {
                const std::string sk = stub_kind(ref);
                const auto cands =
                    ctx->db->lookup_symbols_by_qual_name(qn, sk);
                if (cands.size() == 1) {
                  dst_id = cands[0].id;
                }
              }
            }
            if (dst_id < 0 && ::clang_Location_isInSystemHeader(
                                  ::clang_getCursorLocation(ref)) == 0) {
              // Skip true system/stdlib targets (e.g. one-arg std::move) -- they
              // are never separately indexed, so a stub would be a permanent
              // unresolved external.
              const RefDeclLoc dl = ref_decl_loc(*ctx->db, ref);
              dst_id = ctx->db->mint_symbol_id(
                  callee_usr,
                  CxString(::clang_getCursorSpelling(ref)).str(),
                  qualified_name(ref),
                  CxString(::clang_getCursorDisplayName(ref)).str(),
                  stub_kind(ref), dl.file_id, dl.line, dl.col, dl.path);
            }
          } else {
            const RefDeclLoc dl = ref_decl_loc(*ctx->db, ref);
            // Pre-check: is this an instantiation member? Set is_instantiation=1
            // on the mint if the callee has a specialized parent.
            const CXCursor pre_primary =
                ::clang_getSpecializedCursorTemplate(ref);
            const bool is_inst_member =
                !::clang_Cursor_isNull(pre_primary) &&
                !is_invalid_kind(::clang_getCursorKind(pre_primary)) &&
                [&]() {
                  const std::string pp_usr =
                      CxString(::clang_getCursorUSR(pre_primary)).str();
                  return !pp_usr.empty() && pp_usr != callee_usr;
                }();
            dst_id = ctx->db->mint_symbol_id(
                callee_usr,
                CxString(::clang_getCursorSpelling(ref)).str(),
                qualified_name(ref),
                CxString(::clang_getCursorDisplayName(ref)).str(),
                stub_kind(ref), dl.file_id, dl.line, dl.col, dl.path,
                /*is_instantiation=*/is_inst_member);
            // Function/method template specialization: capture the concrete
              // template arguments. Free-function specs expose them via the cursor
              // API (incl. non-type + nested args); METHOD specs return -1 there
              // (libclang gap), so fall back to the explicit `<...>` call tokens,
              // stored as-written with best-effort type ref_id linking.
            if (is_inst_member && dst_id >= 0) {
              const int wrote =
                  index_cursor_template_args(*ctx->db, dst_id, ref);
              if (wrote == 0 &&
                  ::clang_getCursorKind(ref) == CXCursor_CXXMethod) {
                index_method_template_args_from_tokens(
                    *ctx->db, dst_id, cursor,
                    CxString(::clang_getCursorSpelling(ref)).str());
              }
            }
          }
          if (dst_id >= 0) {
            // Phase 2: compute receiver provenance for member calls
            std::string recv_src_kind;
            std::string recv_type_usr;
            std::string recv_decl_usr;
            std::optional<int64_t> recv_param_pos;
            const CXCursor recv_expr = receiver_subexpr(cursor);
            if (!::clang_Cursor_isNull(recv_expr)) {
              const ValueSource rv = classify_value_source(recv_expr);
              recv_src_kind = rv.src_kind;
              recv_type_usr = rv.type_usr;
              recv_decl_usr = rv.decl_usr;
              // If receiver is a PARM_DECL, record its 0-based parameter
              // position so the Gamma engine can do position-indexed binding.
              if (recv_src_kind == "local" && !recv_decl_usr.empty()) {
                // Peel to the DECL_REF_EXPR to get the referenced ParmDecl.
                CXCursor peeled_recv = ::clang_getCursorReferenced(recv_expr);
                // Handle nested peeling (implicit casts etc.)
                while (!::clang_Cursor_isNull(peeled_recv) &&
                       ::clang_getCursorKind(peeled_recv) != CXCursor_ParmDecl &&
                       ::clang_getCursorKind(peeled_recv) != CXCursor_VarDecl) {
                  peeled_recv = ::clang_getCursorReferenced(peeled_recv);
                }
                if (!::clang_Cursor_isNull(peeled_recv) &&
                    ::clang_getCursorKind(peeled_recv) == CXCursor_ParmDecl) {
                  // Find position by iterating parent function's parameters.
                  const CXCursor fn_parent =
                      ::clang_getCursorSemanticParent(peeled_recv);
                  if (!::clang_Cursor_isNull(fn_parent)) {
                    const int nparams =
                        ::clang_Cursor_getNumArguments(fn_parent);
                    const std::string parm_usr = recv_decl_usr; // already set
                    for (int pi = 0; pi < nparams; ++pi) {
                      const CXCursor p =
                          ::clang_Cursor_getArgument(fn_parent,
                                                       static_cast<unsigned>(pi));
                      const std::string p_usr =
                          CxString(::clang_getCursorUSR(p)).str();
                      if (p_usr == parm_usr) {
                        recv_param_pos = static_cast<int64_t>(pi);
                        break;
                      }
                    }
                  }
                }
              }
            } else {
              // Implicit this (no explicit receiver child)
              const CXCursorKind ref_kind = ::clang_getCursorKind(ref);
              if (ref_kind == CXCursor_CXXMethod ||
                  ref_kind == CXCursor_Constructor ||
                  ref_kind == CXCursor_Destructor) {
                const CXCursor owner = ::clang_getCursorSemanticParent(ref);
                if (!::clang_Cursor_isNull(owner)) {
                  const std::string owner_usr =
                      CxString(::clang_getCursorUSR(owner)).str();
                  recv_src_kind = "this";
                  recv_type_usr = owner_usr;
                  recv_decl_usr = owner_usr;
                }
              }
            }
            // Phase 3a: compute recv_type_is_value for value-eligible src_kinds.
            std::optional<int64_t> recv_type_is_value_opt;
            if ((recv_src_kind == "member" || recv_src_kind == "global" ||
                 recv_src_kind == "call_result") &&
                !::clang_Cursor_isNull(recv_expr)) {
              // dispatch_usr = USR of the class owning the virtual method.
              std::string dispatch_usr;
              const CXCursorKind ref_kind2 = ::clang_getCursorKind(ref);
              if (ref_kind2 == CXCursor_CXXMethod ||
                  ref_kind2 == CXCursor_Constructor ||
                  ref_kind2 == CXCursor_Destructor ||
                  ref_kind2 == CXCursor_ConversionFunction) {
                const CXCursor owner2 = ::clang_getCursorSemanticParent(ref);
                if (!::clang_Cursor_isNull(owner2)) {
                  dispatch_usr =
                      CxString(::clang_getCursorUSR(owner2)).str();
                }
              }
              // Use the DECLARED type of the underlying decl (not the use-site
              // expression type, which auto-derefs references in libclang).
              const CXCursor peeled_recv = peel_expr(recv_expr);
              recv_type_is_value_opt =
                  type_is_value(decl_type_for_expr(peeled_recv),
                                dispatch_usr) ? 1 : 0;
            }
            unsigned call_line = 0, call_col = 0, call_off = 0;
            CXFile call_fh = nullptr;
            ::clang_getExpansionLocation(::clang_getCursorLocation(cursor),
                                           &call_fh, &call_line, &call_col, &call_off);
            const int64_t edge_id = emit_call_edge(
                ctx, cursor, dst_id,
                recv_src_kind, recv_type_usr, recv_decl_usr, recv_param_pos,
                recv_type_is_value_opt);
            emit_call_args(ctx, cursor, edge_id, call_line, call_col);

            // B3 instantiates (kind=5): when the callee is a template
            // specialization, emit an edge to the primary template symbol.
            // clang_getSpecializedCursorTemplate returns the primary (or a
            // partial specialization) for both function and class templates.
            // For a recovered primary template this is a no-op (no parent).
            const CXCursor primary =
                ::clang_getSpecializedCursorTemplate(ref);
            if (!::clang_Cursor_isNull(primary) &&
                !is_invalid_kind(::clang_getCursorKind(primary))) {
              const std::string prim_usr =
                  CxString(::clang_getCursorUSR(primary)).str();
              if (!prim_usr.empty() && prim_usr != callee_usr) {
                // Only emit when primary is already indexed (no stubs for
                // stdlib templates — prevents inflating the stub count for
                // std::vector, std::move, etc.).
                const auto prim_sym = ctx->db->lookup_symbol(prim_usr);
                if (prim_sym) {
                  Edge inst;
                  inst.src_id = ctx->src_id;
                  inst.dst_id = prim_sym->id;
                  inst.kind = 5; // instantiates
                  inst.count = 1;
                  ctx->db->add_edge(inst);
                  // ADR-004 instantiation-member promotion block.
                  // Runs alongside the existing caller->primary edge above.
                  mint_instantiation_nodes(*ctx->db, ref, dst_id,
                                           prim_sym->id);
                }
              }
            }
          }
        }
      }
      // PR1 Layer-0: emit construction form edges (10/11/13/14) and
      // factory-construct (15) when the callee is a known constructor or a
      // make_unique / make_shared factory. Lookup-only: only when B is indexed.
      // parent-kind context: `parent` (the cursor arg) is the parent of cursor.
      if (!::clang_Cursor_isNull(ref)) {
        const CXCursorKind ref_kind = ::clang_getCursorKind(ref);
        if (ref_kind == CXCursor_Constructor) {
          // Skip if the immediate parent is CXXNewExpr (134): the construct-heap
          // branch below handles that case so we avoid emitting both 12 and 10.
          const CXCursorKind parent_kind = ::clang_getCursorKind(parent);
          if (parent_kind != (CXCursorKind)134 /* CXXNewExpr */) {
            const std::string type_usr =
                record_usr_of_type(::clang_getCursorType(cursor));
            if (!type_usr.empty()) {
              if (const auto dst_sym = ctx->db->lookup_symbol(type_usr)) {
                int form;
                if (parent_kind == CXCursor_VarDecl) {
                  form = ctor_form_kind(ref);
                } else {
                  // Standalone temporary (Widget{} / Widget(x) not in a var).
                  int sig = ctor_form_kind(ref);
                  form = (sig == 13 || sig == 14) ? sig : 11; // construct-temp
                }
                Edge fe;
                fe.src_id = ctx->src_id;
                fe.dst_id = dst_sym->id;
                fe.kind = form;
                fe.count = 1;
                ctx->db->add_edge(fe);
              }
            }
          }
        } else if (ref_kind == CXCursor_FunctionDecl) {
          // Factory: make_unique<B> / make_shared<B> from system headers.
          const std::string callee_sp =
              CxString(::clang_getCursorSpelling(ref)).str();
          if ((callee_sp == "make_unique" || callee_sp == "make_shared") &&
              ::clang_Location_isInSystemHeader(
                  ::clang_getCursorLocation(ref)) != 0) {
            const CXType result_canonical =
                ::clang_getCanonicalType(::clang_getCursorType(cursor));
            const int nargs =
                ::clang_Type_getNumTemplateArguments(result_canonical);
            if (nargs > 0) {
              const CXType arg0 =
                  ::clang_Type_getTemplateArgumentAsType(result_canonical, 0);
              const std::string fact_usr = record_usr_of_type(arg0);
              if (!fact_usr.empty()) {
                if (const auto fact_sym = ctx->db->lookup_symbol(fact_usr)) {
                  Edge fe;
                  fe.src_id = ctx->src_id;
                  fe.dst_id = fact_sym->id;
                  fe.kind = 15; // factory-construct
                  fe.count = 1;
                  ctx->db->add_edge(fe);
                }
              }
            }
          }
        }
      }
    } else if (kind == (CXCursorKind)134 /* CXXNewExpr */) {
      // PR1 Layer-0: construct-heap (12). The new expression type is the
      // pointer to the allocated record (e.g. Widget*).
      const std::string heap_usr =
          record_usr_of_type(::clang_getCursorType(cursor));
      if (!heap_usr.empty()) {
        if (const auto heap_sym = ctx->db->lookup_symbol(heap_usr)) {
          Edge fe;
          fe.src_id = ctx->src_id;
          fe.dst_id = heap_sym->id;
          fe.kind = 12; // construct-heap
          fe.count = 1;
          ctx->db->add_edge(fe);
        }
      }
    } else if (kind == (CXCursorKind)135 /* CXXDeleteExpr */) {
      // PR1 Layer-0: destroy (16). First child's type pointee names the
      // destroyed record.
      struct FirstDelCtx {
        std::string usr;
      } fdc;
      ::clang_visitChildren(
          cursor,
          [](CXCursor c, CXCursor /*p*/, CXClientData data) {
            auto *ctx2 = static_cast<FirstDelCtx *>(data);
            const CXType ct = ::clang_getCursorType(c);
            ctx2->usr = record_usr_of_type(ct);
            return CXChildVisit_Break; // first child only
          },
          &fdc);
      if (!fdc.usr.empty()) {
        if (const auto del_sym = ctx->db->lookup_symbol(fdc.usr)) {
          Edge fe;
          fe.src_id = ctx->src_id;
          fe.dst_id = del_sym->id;
          fe.kind = 16; // destroy
          fe.count = 1;
          ctx->db->add_edge(fe);
        }
      }
    } else if (kind == CXCursor_DeclRefExpr || kind == CXCursor_MemberRefExpr) {
      // B2 uses: DECL_REF_EXPR references a non-function indexed symbol
      // (variable, field, enum-constant, etc.).  Only emit for symbols
      // already in the DB (lookup, no stub) — prevents creating stubs for
      // every standard-library constant touched in the body.
      const CXCursor ref = ::clang_getCursorReferenced(cursor);
      if (!::clang_Cursor_isNull(ref)) {
        const CXCursorKind ref_kind = ::clang_getCursorKind(ref);
        // Exclude function-like (those produce calls edges above); include
        // member fields, variables, enum-constants, etc.
        if (!is_function_like(ref_kind) && ref_kind != CXCursor_CXXMethod &&
            ref_kind != CXCursor_Constructor &&
            ref_kind != CXCursor_Destructor) {
          const std::string ref_usr =
              CxString(::clang_getCursorUSR(ref)).str();
          if (!ref_usr.empty()) {
            const auto dst_sym = ctx->db->lookup_symbol(ref_usr);
            if (dst_sym) {
              emit_body_edge(ctx, cursor, dst_sym->id, 7 /* uses */);
            }
          }
        }
      }
    } else if (kind == CXCursor_TypeRef || kind == CXCursor_TemplateRef) {
      // B2 uses: a bare type NAME in expression/statement position
      // (Color::Red, MyClass::instance(), sizeof(T), static_cast<T>, new T).
      // PARENT-KIND GUARD: `parent` is the enclosing cursor. Signature / field
      // / var-decl / typedef type-refs are already emitted by the declaration
      // paths (emit_type_use, template_arg rows) and have a *declaration*
      // parent, so skip those. Only type-names under expression/statement
      // nodes survive. Mirrors ast.py:_body_descent TYPE_REF branch.
      const CXCursorKind pk = ::clang_getCursorKind(parent);
      const bool parent_is_decl =
          pk == CXCursor_VarDecl || pk == CXCursor_ParmDecl ||
          pk == CXCursor_FieldDecl || pk == CXCursor_FunctionDecl ||
          pk == CXCursor_CXXMethod || pk == CXCursor_Constructor ||
          pk == CXCursor_Destructor || pk == CXCursor_FunctionTemplate ||
          pk == CXCursor_TypedefDecl || pk == CXCursor_TypeAliasDecl;
      if (!parent_is_decl) {
        const CXCursor ref = ::clang_getCursorReferenced(cursor);
        if (!::clang_Cursor_isNull(ref)) {
          const std::string usr =
              CxString(::clang_getCursorUSR(ref)).str();
          // Lookup-only, NO stubs; skip self-edge and the enclosing method's
          // own owning record (redundant with method_of).
          if (!usr.empty() && usr != ctx->owner_usr) {
            const auto dst = ctx->db->lookup_symbol(usr);
            if (dst && dst->id != ctx->src_id) {
              emit_body_edge(ctx, cursor, dst->id, 7 /* uses */);
            }
          }
        }
      }
    } else if (kind == CXCursor_VarDecl) {
      // B2 uses: a LOCAL variable's declared type names a record/enum/typedef
      // -> uses edge (src=enclosing fn). `Conf local;` counts as the function
      // using Conf even when no method is called on it.
      emit_type_use(*ctx->db, ctx->src_id, ::clang_getCursorType(cursor),
                    ctx->file_id, cursor, ctx->cond_depth > 0 ? 1 : 0);
      // Stage 3: a LOCAL `X<B> v;` mints the X<B> instance entity (its own
      // composes/aggregates/associates via T->B). The file-cursor walk in
      // index_edges does not descend bodies, so locals are minted here;
      // file-scope vars + members are minted there. (Order matches the Python
      // body-descent arm: emit_type_use THEN mint -- locals have no owning
      // record, so the FIELD_DECL/VAR_DECL ordering fix is not needed here.)
      mint_instance_from_type(*ctx->db, ::clang_getCursorType(cursor));
      // B3 class-template instantiates (kind=5): when a variable's type is a
      // class-template instantiation, emit instantiates (src=enclosing fn,
      // dst=primary template) + template_arg rows.
      // Only emit when the primary is already indexed (no stubs for stdlib
      // types — prevents inflating stub count for std::string, std::vector).
      const CXType var_type = ::clang_getCursorType(cursor);
      const int nargs = ::clang_Type_getNumTemplateArguments(var_type);
      if (nargs > 0) {
        const CXCursor type_decl = ::clang_getTypeDeclaration(var_type);
        if (!::clang_Cursor_isNull(type_decl) &&
            !is_invalid_kind(::clang_getCursorKind(type_decl))) {
          const CXCursor primary =
              ::clang_getSpecializedCursorTemplate(type_decl);
          if (!::clang_Cursor_isNull(primary) &&
              !is_invalid_kind(::clang_getCursorKind(primary))) {
            const std::string prim_usr =
                CxString(::clang_getCursorUSR(primary)).str();
            if (!prim_usr.empty()) {
              const auto prim_sym = ctx->db->lookup_symbol(prim_usr);
              if (prim_sym) {
                // instantiates edge: fn -> primary template
                Edge inst;
                inst.src_id = ctx->src_id;
                inst.dst_id = prim_sym->id;
                inst.kind = 5; // instantiates
                inst.count = 1;
                ctx->db->add_edge(inst);

                // template_arg rows: owner_id = src_id (the using function),
                // recording which types this instantiation uses.
                for (int ai = 0; ai < nargs; ++ai) {
                  const CXType arg_type =
                      ::clang_Type_getTemplateArgumentAsType(
                          var_type, static_cast<unsigned>(ai));
                  TemplateArg ta;
                  ta.owner_id = ctx->src_id;
                  ta.position = static_cast<int64_t>(ai);
                  ta.arg_kind = 1; // TYPE
                  // Always store the type spelling so the binding is
                  // distinguishable even for builtins with no declaration.
                  const std::string spelling =
                      CxString(::clang_getTypeSpelling(arg_type)).str();
                  if (!spelling.empty()) {
                    ta.literal = spelling;
                  }
                  // Try to resolve the arg type to an indexed symbol.
                  const CXCursor arg_decl =
                      ::clang_getTypeDeclaration(arg_type);
                  if (!::clang_Cursor_isNull(arg_decl) &&
                      !is_invalid_kind(::clang_getCursorKind(arg_decl))) {
                    const std::string ref_usr =
                        CxString(::clang_getCursorUSR(arg_decl)).str();
                    if (!ref_usr.empty()) {
                      if (const auto rsym = ctx->db->lookup_symbol(ref_usr)) {
                        ta.ref_id = rsym->id;
                      }
                    }
                  }
                  if (!ta.ref_id) {
                    ta.ref_id = resolve_template_arg_ref_id(
                        *ctx->db, ta.literal, cursor);
                  }
                  ctx->db->add_template_arg(ta);
                }
              }
            }
          }
        }
      }
    }

    // Recurse into children, tracking cond_depth.
    const bool is_cond = is_cond_cursor(kind);
    if (is_cond) {
      ++ctx->cond_depth;
    }
    ::clang_visitChildren(cursor, &body_descent_visitor, ctx);
    if (is_cond) {
      --ctx->cond_depth;
    }
  } catch (...) {
    ctx->error = std::current_exception();
    return CXChildVisit_Break;
  }
  return CXChildVisit_Continue; // children already visited recursively above
}

// v27: walk a static member variable's INITIALIZER, recording each call it
// makes as a def_edge off this backend's definition. Mirrors
// ast.py:_emit_static_init_def_edges. A variable has no body descent, so
// `int C::x = seed();` would otherwise drop the `seed` dependency.
struct StaticInitCtx {
  Storage *db;
  int64_t def_id;
};

CXChildVisitResult static_init_visitor(CXCursor cursor, CXCursor /*parent*/,
                                       CXClientData data) noexcept {
  auto *ctx = static_cast<StaticInitCtx *>(data);
  try {
    if (::clang_getCursorKind(cursor) == CXCursor_CallExpr) {
      const CXCursor ref = ::clang_getCursorReferenced(cursor);
      if (!::clang_Cursor_isNull(ref)) {
        const std::string usr =
            CxString(::clang_getCursorUSR(ref)).str();
        if (!usr.empty()) {
          const auto sym = ctx->db->lookup_symbol(usr);
          if (sym) {
            // A variable does not *call*; its initializer USES (kind 7) the
            // functions it references. Mirrors _emit_static_init_def_edges.
            ctx->db->add_def_edge(ctx->def_id, sym->id, 7); // uses (not a call)
          }
        }
      }
    }
  } catch (...) {
    // Swallow: initializer def_edges are best-effort, like the Python walk.
  }
  return CXChildVisit_Recurse;
}

void emit_static_init_def_edges(Storage &db, CXCursor var_cursor,
                                int64_t def_id) {
  StaticInitCtx ctx{&db, def_id};
  ::clang_visitChildren(var_cursor, &static_init_visitor, &ctx);
}

// v28: the initializer source text of a variable definition, per backend:
// `int C::x = seed_a();` -> "seed_a()", `= 5` -> "5". Reads the cursor's own
// source extent from the file and returns the text after the first '=', stripped
// with a trailing ';' removed (no '=' -> nullopt). Exact source slice so Python
// and C++ agree byte-for-byte. Mirrors _static_var_init_text.
std::optional<std::string> static_var_init_text(CXCursor cursor) {
  const CXSourceRange ext = ::clang_getCursorExtent(cursor);
  CXFile sfile = nullptr;
  unsigned soff = 0, u = 0;
  ::clang_getExpansionLocation(::clang_getRangeStart(ext), &sfile, &u, &u,
                                 &soff);
  CXFile efile = nullptr;
  unsigned eoff = 0;
  ::clang_getExpansionLocation(::clang_getRangeEnd(ext), &efile, &u, &u,
                                 &eoff);
  if (sfile == nullptr || eoff <= soff) {
    return std::nullopt;
  }
  const std::string path = CxString(::clang_getFileName(sfile)).str();
  if (path.empty()) {
    return std::nullopt;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  in.seekg(static_cast<std::streamoff>(soff));
  std::string raw(eoff - soff, '\0');
  in.read(raw.data(), static_cast<std::streamsize>(eoff - soff));
  raw.resize(static_cast<std::size_t>(in.gcount()));
  const auto eq = raw.find('=');
  if (eq == std::string::npos) {
    return std::nullopt;
  }
  const auto strip = [](const std::string &s) {
    const char *ws = " \t\r\n\f\v";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) {
      return std::string();
    }
    const auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
  };
  std::string val = strip(raw.substr(eq + 1));
  while (!val.empty() && val.back() == ';') {
    val.pop_back();
  }
  val = strip(val);
  if (val.empty()) {
    return std::nullopt;
  }
  return val;
}


} // namespace cidx::ast_detail

namespace cidx {
using namespace ast_detail;

void AstIndexer::body_descent(CXCursor fn_cursor, int64_t src_id,
                              int64_t file_id) {
  BodyDescentCtx ctx;
  ctx.db = &db_;
  ctx.src_id = src_id;
  ctx.file_id = file_id;
  ctx.cond_depth = 0;
  // Owner USR for the self-owner skip in the TYPE_REF branch: when fn_cursor is
  // a method, its semantic parent is the owning record; record that USR so a
  // method naming its own class does not emit a redundant uses edge.
  const CXCursor owner = ::clang_getCursorSemanticParent(fn_cursor);
  if (!::clang_Cursor_isNull(owner)) {
    const CXCursorKind ok = ::clang_getCursorKind(owner);
    if (ok == CXCursor_ClassDecl || ok == CXCursor_StructDecl ||
        ok == CXCursor_UnionDecl || ok == CXCursor_ClassTemplate ||
        ok == CXCursor_ClassTemplatePartialSpecialization) {
      ctx.owner_usr = CxString(::clang_getCursorUSR(owner)).str();
    }
  }
  ::clang_visitChildren(fn_cursor, &body_descent_visitor, &ctx);
  if (ctx.error) {
    std::rethrow_exception(ctx.error);
  }
}

} // namespace cidx
