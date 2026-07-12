#include "clangx_lt/body_walker.hpp"

#include "clangx_lt/call_template_args.hpp"
#include "clangx_lt/edge_sink.hpp"
#include "clangx_lt/instantiation_edges.hpp"
#include "clangx_lt/kind_map.hpp"
#include "clangx_lt/location.hpp"
#include "clangx_lt/names.hpp"
#include "clangx_lt/receiver_provenance.hpp"
#include "clangx_lt/type_use.hpp"
#include "clangx_lt/llvm_compat.hpp"
#include "clangx_lt/usr.hpp"
#include "clangx_lt/value_source.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/NestedNameSpecifier.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/Config/llvm-config.h"

#include <algorithm>
#include <set>

namespace cidx::lt {

namespace {

// is_cond_cursor (ast_templates.cpp:17): If/For/While/Do/Switch/Case/?: —
// range-for is NOT in the set.
bool is_cond_stmt(const clang::Stmt *s) {
  return llvm::isa<clang::IfStmt>(s) || llvm::isa<clang::ForStmt>(s) ||
         llvm::isa<clang::WhileStmt>(s) || llvm::isa<clang::DoStmt>(s) ||
         llvm::isa<clang::SwitchStmt>(s) || llvm::isa<clang::CaseStmt>(s) ||
         llvm::isa<clang::ConditionalOperator>(s);
}

// libclang function-like reference kinds.
bool is_function_like_decl(const clang::Decl *d) {
  return llvm::isa<clang::FunctionDecl>(d);
}

// libclang locates TYPE_REF cursors at the type NAME token (after any
// qualifier); TEMPLATE_REF at the template name.
clang::SourceLocation type_name_loc(clang::TypeLoc tl) {
#if LLVM_VERSION_MAJOR < 22
  // Pre-22 an elaborated (qualified) type wraps the tag/typedef loc; peel to
  // the named type so getNameLoc lands on the type NAME, not the qualifier.
  if (auto etl = tl.getAs<clang::ElaboratedTypeLoc>())
    tl = etl.getNamedTypeLoc();
#endif
  tl = tl.getUnqualifiedLoc();
  if (auto ts = tl.getAs<clang::TemplateSpecializationTypeLoc>())
    return ts.getTemplateNameLoc();
  // LLVM 22: tag/typedef TypeLocs embed their qualifier and carry NameLoc.
  if (auto tt = tl.getAs<clang::TagTypeLoc>())
    return tt.getNameLoc();
  if (auto td = tl.getAs<clang::TypedefTypeLoc>())
    return td.getNameLoc();
  if (auto spec = tl.getAs<clang::TypeSpecTypeLoc>())
    return spec.getNameLoc();
  return tl.getBeginLoc();
}

// The callee decl the way clang_getCursorReferenced resolves a CALL_EXPR.
const clang::FunctionDecl *callee_decl(const clang::CallExpr *call) {
  return llvm::dyn_cast_or_null<clang::FunctionDecl>(call->getCalleeDecl());
}

// Candidate declarations of a dependent/overloaded callee
// (overload_set_candidates): the OverloadExpr in callee position.
const clang::OverloadExpr *callee_overload_expr(const clang::CallExpr *call) {
  const clang::Expr *callee = call->getCallee();
  if (callee == nullptr)
    return nullptr;
  callee = peel_expr(callee);
  return llvm::dyn_cast_or_null<clang::OverloadExpr>(callee);
}

} // namespace

BodyWalker::BodyWalker(clang::ASTContext &context, EdgeSink &sink,
                       int64_t src_id, int64_t file_id)
    : context_(context), sink_(sink), mint_(context, sink),
      resolver_(context, sink), minter_(context, sink, mint_, resolver_),
      src_id_(src_id), file_id_(file_id) {}

void BodyWalker::walk(const clang::FunctionDecl *fn) {
  // Owner USR for the self-owner skip in the type-name branch.
  if (const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(fn)) {
    if (const clang::CXXRecordDecl *owner = method->getParent()) {
      const clang::NamedDecl *o = owner;
      if (const clang::ClassTemplateDecl *ct =
              owner->getDescribedClassTemplate())
        o = ct;
      owner_usr_ = usr_for_decl(o);
    }
  }
  // Constructor member initializers carry calls/uses too (libclang walks them
  // as children of the ctor cursor).
  if (const auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(fn)) {
    for (const clang::CXXCtorInitializer *init : ctor->inits()) {
      if (init->isWritten() && init->getInit() != nullptr)
        descend(init->getInit(), nullptr);
    }
  }
  if (const clang::Stmt *body = fn->getBody())
    descend(body, nullptr);
}

void BodyWalker::descend(const clang::Stmt *stmt, const clang::Stmt *parent) {
  if (stmt == nullptr)
    return;
  handle_stmt(stmt, parent);
  const bool cond = is_cond_stmt(stmt);
  if (cond)
    ++cond_depth_;
  // Range-for: libclang visits only the RANGE INIT, the loop VARIABLE, and
  // the BODY — the desugared begin/end/cond/inc statements never appear as
  // cursors, so their operator calls must not be emitted.
  if (const auto *frs = llvm::dyn_cast<clang::CXXForRangeStmt>(stmt)) {
    descend(frs->getRangeInit(), stmt);
    if (const clang::Stmt *lv = frs->getLoopVarStmt())
      descend(lv, stmt);
    descend(frs->getBody(), stmt);
    return; // range-for is not a cond stmt; no depth to unwind
  }
  // Local declarations run BEFORE their initializer children (libclang
  // preorder: the VAR_DECL cursor precedes its ctor-call child, so a spec
  // instance minted by the ctor call is NOT yet visible to the var branch).
  if (const auto *ds = llvm::dyn_cast<clang::DeclStmt>(stmt)) {
    for (const clang::Decl *d : ds->decls())
      if (const auto *var = llvm::dyn_cast<clang::VarDecl>(d))
        handle_var_decl(var);
  }
  for (const clang::Stmt *child : stmt->children())
    descend(child, stmt);
  // Local tag types' method bodies (libclang recursion reaches them from the
  // same walk, attributed to the OUTER function).
  if (const auto *ds = llvm::dyn_cast<clang::DeclStmt>(stmt)) {
    for (const clang::Decl *d : ds->decls()) {
      if (const auto *tag = llvm::dyn_cast<clang::CXXRecordDecl>(d)) {
        for (const clang::CXXMethodDecl *m : tag->methods())
          if (m->doesThisDeclarationHaveABody())
            descend(m->getBody(), stmt);
      }
    }
  }
  if (cond)
    --cond_depth_;
}

void BodyWalker::handle_stmt(const clang::Stmt *stmt,
                             const clang::Stmt *parent) {
  if (const auto *ctor = llvm::dyn_cast<clang::CXXConstructExpr>(stmt)) {
    handle_construct(ctor, parent);
    return;
  }
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(stmt)) {
    handle_call(call, parent);
    return;
  }
  if (const auto *ne = llvm::dyn_cast<clang::CXXNewExpr>(stmt)) {
    handle_new(ne);
    return;
  }
  if (const auto *de = llvm::dyn_cast<clang::CXXDeleteExpr>(stmt)) {
    handle_delete(de);
    return;
  }
  if (const auto *dre = llvm::dyn_cast<clang::DeclRefExpr>(stmt)) {
    // uses(7): non-function references, lookup-only.
    const clang::ValueDecl *ref = dre->getDecl();
    if (ref != nullptr && !is_function_like_decl(ref)) {
      const std::string usr = usr_for_decl(ref);
      if (!usr.empty()) {
        if (const auto dst = sink_.lookup_symbol_id(usr))
          // libclang locates a DECL_REF_EXPR at the NAME (after qualifiers).
          emit_site_edge_at(dre->getLocation(), *dst, 7);
      }
    }
    // Bare type name in the qualifier (Color::Red): TYPE_REF under an
    // expression parent. NNS is a value type with a Kind enum in LLVM 22, a
    // pointer with a SpecifierKind in LLVM 21.
    {
      const clang::Type *t = nullptr;
#if LLVM_VERSION_MAJOR >= 22
      const clang::NestedNameSpecifier nns = dre->getQualifier();
      if (nns.getKind() == clang::NestedNameSpecifier::Kind::Type)
        t = nns.getAsType();
#else
      if (const clang::NestedNameSpecifier *nns = dre->getQualifier())
        t = nns->getAsType();
#endif
      if (t != nullptr) {
        if (const clang::NamedDecl *td =
                named_type_decl(clang::QualType(t, 0))) {
          const std::string usr = usr_for_decl(td);
          if (!usr.empty() && usr != owner_usr_) {
            if (const auto dst = sink_.lookup_symbol_id(usr))
              if (*dst != src_id_)
                emit_site_edge(dre, *dst, 7);
          }
        }
      }
    }
    // Explicit template arguments (make_unique<Widget>(...)): each named
    // TYPE argument is a TYPE_REF under the expression — emit uses.
    if (dre->hasExplicitTemplateArgs()) {
      for (const clang::TemplateArgumentLoc &tal : dre->template_arguments()) {
        if (tal.getArgument().getKind() != clang::TemplateArgument::Type)
          continue;
        const clang::TypeSourceInfo *tsi = tal.getTypeSourceInfo();
        if (tsi == nullptr)
          continue;
        const clang::NamedDecl *td = named_type_decl(tsi->getType());
        if (td == nullptr)
          continue;
        const std::string usr = usr_for_decl(td);
        if (usr.empty() || usr == owner_usr_)
          continue;
        if (const auto dst = sink_.lookup_symbol_id(usr))
          if (*dst != src_id_)
            emit_site_edge_at(type_name_loc(tsi->getTypeLoc()), *dst, 7);
      }
    }
    return;
  }
  if (const auto *me = llvm::dyn_cast<clang::MemberExpr>(stmt)) {
    const clang::ValueDecl *ref = me->getMemberDecl();
    if (ref != nullptr && !is_function_like_decl(ref)) {
      const std::string usr = usr_for_decl(ref);
      if (!usr.empty()) {
        if (const auto dst = sink_.lookup_symbol_id(usr))
          // libclang locates a MEMBER_REF_EXPR at the member NAME token.
          emit_site_edge_at(me->getMemberLoc(), *dst, 7);
      }
    }
    return;
  }
  // Bare type names in expression position (sizeof(T), static_cast<T>,
  // T{}/T() temporaries) — libclang TYPE_REF/TEMPLATE_REF cursors whose
  // parent is an expression (declaration parents are excluded by guard).
  const auto emit_type_name = [&](const clang::TypeSourceInfo *tsi) {
    if (tsi == nullptr)
      return;
    const clang::NamedDecl *named = named_type_decl(tsi->getType());
    if (named == nullptr)
      return;
    if (const auto *rec = llvm::dyn_cast<clang::CXXRecordDecl>(named))
      if (const clang::ClassTemplateDecl *ct = rec->getDescribedClassTemplate())
        named = ct;
    const std::string usr = usr_for_decl(named);
    if (usr.empty() || usr == owner_usr_)
      return;
    if (const auto dst = sink_.lookup_symbol_id(usr))
      if (*dst != src_id_)
        emit_site_edge_at(type_name_loc(tsi->getTypeLoc()), *dst, 7);
  };
  if (const auto *uett = llvm::dyn_cast<clang::UnaryExprOrTypeTraitExpr>(stmt)) {
    if (uett->isArgumentType())
      emit_type_name(uett->getArgumentTypeInfo());
    return;
  }
  if (const auto *cast = llvm::dyn_cast<clang::ExplicitCastExpr>(stmt)) {
    emit_type_name(cast->getTypeInfoAsWritten());
    return;
  }
}

void BodyWalker::handle_call(const clang::CallExpr *call,
                             const clang::Stmt *parent) {
  const clang::FunctionDecl *ref = callee_decl(call);
  bool recovered = false;
  if (ref == nullptr) {
    // Dependent/overloaded callee: single-candidate recovery, else link every
    // indexed overload (emit_overloaded_calls).
    if (const clang::OverloadExpr *ovl = callee_overload_expr(call)) {
      std::vector<const clang::NamedDecl *> cands(ovl->decls_begin(),
                                                  ovl->decls_end());
      if (cands.size() == 1) {
        const clang::NamedDecl *cand = cands[0];
        const clang::NamedDecl *mint_as = nullptr;
        if (const auto *ftd = llvm::dyn_cast<clang::FunctionTemplateDecl>(cand)) {
          ref = ftd->getTemplatedDecl();
          mint_as = ftd;
        } else {
          ref = llvm::dyn_cast<clang::FunctionDecl>(cand);
        }
        if (ref != nullptr) {
          emit_resolved_call(call, ref, /*recovered=*/true, mint_as);
          return;
        }
      } else if (cands.size() >= 2) {
        std::set<int64_t> dst_ids;
        for (const clang::NamedDecl *cand : cands) {
          const std::string usr = usr_for_decl(cand);
          if (usr.empty())
            continue;
          if (const auto s = sink_.lookup_symbol_id(usr)) {
            dst_ids.insert(*s);
            continue;
          }
          if (context_.getSourceManager().isInSystemHeader(
                  cand->getLocation()))
            continue;
          if (auto req = mint_.build(cand))
            dst_ids.insert(sink_.mint_symbol(*req));
        }
        if (dst_ids.empty() && !cands.empty()) {
          const std::string qn = qualified_name(context_, cands[0]);
          if (!qn.empty())
            for (const int64_t id : sink_.symbol_ids_by_qual_name_kind(
                     qn, cidx_stub_kind_name(cands[0])))
              dst_ids.insert(id);
        }
        for (const int64_t dst_id : dst_ids)
          emit_site_edge(call, dst_id, 1);
      }
    }
  }
  if (ref != nullptr)
    emit_resolved_call(call, ref, recovered);

  // Factory: make_unique<B> / make_shared<B> from system headers.
  if (ref != nullptr && !llvm::isa<clang::CXXMethodDecl>(ref)) {
    const std::string sp = spelling(ref);
    if ((sp == "make_unique" || sp == "make_shared") &&
        context_.getSourceManager().isInSystemHeader(ref->getLocation())) {
      const clang::QualType result = call->getType().getCanonicalType();
      if (const auto *spec = llvm::dyn_cast_or_null<
              clang::ClassTemplateSpecializationDecl>(
              result->getAsCXXRecordDecl())) {
        const clang::TemplateArgumentList &args = spec->getTemplateArgs();
        if (args.size() > 0 &&
            args[0].getKind() == clang::TemplateArgument::Type) {
          const std::string fact_usr =
              record_usr_of_type(args[0].getAsType());
          if (!fact_usr.empty()) {
            if (const auto fact = sink_.lookup_symbol_id(fact_usr)) {
              EdgeRecord fe;
              fe.src_id = src_id_;
              fe.dst_id = *fact;
              fe.kind = 15; // factory-construct
              sink_.add_edge(fe);
            }
          }
        }
      }
    }
  }
}

void BodyWalker::handle_construct(const clang::CXXConstructExpr *ctor,
                                  const clang::Stmt *parent) {
  const clang::CXXConstructorDecl *ref = ctor->getConstructor();
  if (ref == nullptr)
    return;
  // libclang maps EVERY CXXConstructExpr to a CALL_EXPR cursor — including
  // an implicit default ctor for `B b;` (which mints a B() stub).
  emit_resolved_call(ctor, ref, /*recovered=*/false);
  // Temporary-object syntax (Widget{...} / Widget(x)) names the type — a
  // TYPE_REF under the call-expression parent -> uses.
  if (const auto *tmp = llvm::dyn_cast<clang::CXXTemporaryObjectExpr>(ctor)) {
    if (const clang::TypeSourceInfo *tsi = tmp->getTypeSourceInfo()) {
      if (const clang::NamedDecl *td = named_type_decl(tsi->getType())) {
        const std::string usr = usr_for_decl(td);
        if (!usr.empty() && usr != owner_usr_) {
          if (const auto dst = sink_.lookup_symbol_id(usr))
            if (*dst != src_id_)
              emit_site_edge_at(type_name_loc(tsi->getTypeLoc()), *dst, 7);
        }
      }
    }
  }

  // Construction form (10/11/13/14). A ctor under a CXXNewExpr parent emits
  // no form — construct-heap(12) covers it (reference parent-kind guard).
  if (llvm::isa_and_nonnull<clang::CXXNewExpr>(parent))
    return;
  // Parent VarDecl <=> the init of a local variable — our walker enters inits
  // from handle_var_decl / DeclStmt, so a null/DeclStmt parent marks the
  // var-init position.
  const std::string type_usr = record_usr_of_type(ctor->getType());
  if (type_usr.empty())
    return;
  const auto dst = sink_.lookup_symbol_id(type_usr);
  if (!dst)
    return;
  const auto ctor_form = [&]() -> int {
    if (ref->getNumParams() == 1) {
      const std::string pt =
          ref->getParamDecl(0)->getType().getAsString(
              context_.getPrintingPolicy());
      if (pt.find("&&") != std::string::npos)
        return 14; // construct-move
      if (pt.find('&') != std::string::npos)
        return 13; // construct-copy
    }
    return 10; // construct-value
  };
  const bool var_init =
      parent == nullptr || llvm::isa_and_nonnull<clang::DeclStmt>(parent);
  int form;
  if (var_init) {
    form = ctor_form();
  } else {
    const int sig = ctor_form();
    form = (sig == 13 || sig == 14) ? sig : 11; // construct-temp
  }
  EdgeRecord fe;
  fe.src_id = src_id_;
  fe.dst_id = *dst;
  fe.kind = form;
  sink_.add_edge(fe);
}

void BodyWalker::handle_new(const clang::CXXNewExpr *expr) {
  const std::string heap_usr = record_usr_of_type(expr->getType());
  if (!heap_usr.empty()) {
    if (const auto dst = sink_.lookup_symbol_id(heap_usr)) {
      EdgeRecord fe;
      fe.src_id = src_id_;
      fe.dst_id = *dst;
      fe.kind = 12; // construct-heap
      sink_.add_edge(fe);
    }
  }
  // The allocated type name is a TYPE_REF under the new-expr (expression
  // parent) -> uses.
  if (const clang::TypeSourceInfo *tsi = expr->getAllocatedTypeSourceInfo()) {
    if (const clang::NamedDecl *td = named_type_decl(tsi->getType())) {
      const std::string usr = usr_for_decl(td);
      if (!usr.empty() && usr != owner_usr_) {
        if (const auto dst = sink_.lookup_symbol_id(usr))
          if (*dst != src_id_)
            emit_site_edge_at(type_name_loc(tsi->getTypeLoc()), *dst, 7);
      }
    }
  }
  // The constructor call inside `new T(...)` is handled by handle_construct
  // via recursion; construct-heap replaces its construct-value form (the
  // libclang branch skips form emission when the parent is CXXNewExpr, which
  // the var_init/temp logic reproduces since the parent is the new-expr).
}

void BodyWalker::handle_delete(const clang::CXXDeleteExpr *expr) {
  const clang::Expr *arg = expr->getArgument();
  if (arg == nullptr)
    return;
  const std::string usr = record_usr_of_type(arg->getType());
  if (usr.empty())
    return;
  if (const auto dst = sink_.lookup_symbol_id(usr)) {
    EdgeRecord fe;
    fe.src_id = src_id_;
    fe.dst_id = *dst;
    fe.kind = 16; // destroy
    sink_.add_edge(fe);
  }
}

void BodyWalker::handle_var_decl(const clang::VarDecl *var) {
  // Local variable's declared type -> uses edge + instance mint + class
  // template instantiates/template_arg rows (VarDecl branch).
  const ExpansionLoc loc = expansion_loc(context_, var->getLocation());
  emit_type_use(sink_, src_id_, var->getType(), file_id_, loc,
                cond_depth_ > 0 ? 1 : 0);
  minter_.mint_instance_from_type(var->getType());

  const auto *spec =
      llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
          var->getType()->getAsCXXRecordDecl());
  if (spec == nullptr)
    return;
  const clang::ClassTemplateDecl *primary = spec->getSpecializedTemplate();
  if (primary == nullptr)
    return;
  const std::string prim_usr = usr_for_decl(primary);
  if (prim_usr.empty())
    return;
  const auto prim = sink_.lookup_symbol_id(prim_usr);
  if (!prim)
    return; // lookup-only: no stubs for stdlib templates
  EdgeRecord inst;
  inst.src_id = src_id_;
  inst.dst_id = *prim;
  inst.kind = 5; // instantiates
  sink_.add_edge(inst);

  // Args print AS WRITTEN (`Box<Color> bc;` inside geo stores 'Color', not
  // 'geo::Color'): prefer the sugared args off the declared type.
  llvm::SmallVector<clang::QualType, 4> arg_types;
  if (const auto *tst =
          var->getType()->getAs<clang::TemplateSpecializationType>()) {
    for (const clang::TemplateArgument &a : tst->template_arguments())
      arg_types.push_back(a.getKind() == clang::TemplateArgument::Type
                              ? a.getAsType()
                              : clang::QualType());
  } else {
    const clang::TemplateArgumentList &args = spec->getTemplateArgs();
    for (unsigned ai = 0; ai < args.size(); ++ai)
      arg_types.push_back(args[ai].getKind() == clang::TemplateArgument::Type
                              ? args[ai].getAsType()
                              : clang::QualType());
  }
  const clang::PrintingPolicy &policy = context_.getPrintingPolicy();
  for (unsigned ai = 0; ai < arg_types.size(); ++ai) {
    const clang::QualType at = arg_types[ai];
    if (at.isNull())
      continue;
    TemplateArgRecord ta;
    ta.owner_id = src_id_;
    ta.position = static_cast<int64_t>(ai);
    ta.arg_kind = 1;
    const std::string spelling = at.getAsString(policy);
    if (!spelling.empty())
      ta.literal = spelling;
    if (const clang::TagDecl *td = at->getAsTagDecl()) {
      const std::string ref_usr = usr_for_decl(td);
      if (!ref_usr.empty())
        ta.ref_id = sink_.lookup_symbol_id(ref_usr);
    }
    if (!ta.ref_id)
      ta.ref_id = resolver_.resolve(ta.literal, var);
    sink_.add_template_arg(ta);
  }
}

// Resolve a recovered (dependent/overloaded, single-candidate) callee to a
// destination symbol id: lookup by USR, else by (qual_name, kind), else mint a
// stub for a non-system decl. Returns -1 when unresolved.
int64_t
BodyWalker::resolve_recovered_target(const clang::NamedDecl *keyed,
                                     const std::string &callee_usr) {
  int64_t dst_id = -1;
  if (const auto dst = sink_.lookup_symbol_id(callee_usr))
    dst_id = *dst;
  if (dst_id < 0) {
    const std::string qn = qualified_name(context_, keyed);
    if (!qn.empty()) {
      const auto ids =
          sink_.symbol_ids_by_qual_name_kind(qn, cidx_stub_kind_name(keyed));
      if (ids.size() == 1)
        dst_id = ids[0];
    }
  }
  if (dst_id < 0 &&
      !context_.getSourceManager().isInSystemHeader(keyed->getLocation())) {
    if (auto req = mint_.build(keyed))
      dst_id = sink_.mint_symbol(*req);
  }
  return dst_id;
}

// Shared tail for a resolved/recovered callee. Orchestration only: resolve the
// destination id, then delegate each concern to its own translation unit —
// call_template_args (spec arg rows + display name), receiver_provenance (the
// edge_site recv_* fields), and instantiation_edges (the B3 family).
void BodyWalker::emit_resolved_call(const clang::Expr *site,
                                    const clang::FunctionDecl *callee,
                                    bool recovered,
                                    const clang::NamedDecl *mint_as) {
  const clang::NamedDecl *keyed =
      mint_as != nullptr ? mint_as : llvm::cast<clang::NamedDecl>(callee);
  const std::string callee_usr = usr_for_decl(keyed);
  if (callee_usr.empty())
    return;

  int64_t dst_id = -1;
  if (recovered) {
    dst_id = resolve_recovered_target(keyed, callee_usr);
  } else {
    // Non-recovered target: mint the callee (flagging instantiation members)
    // and emit its template-arg rows.
    const bool is_inst_member =
        callee->getPrimaryTemplate() != nullptr ||
        callee->getMemberSpecializationInfo() != nullptr;
    if (auto req = mint_.build(callee)) {
      req->is_instantiation = is_inst_member;
      dst_id = sink_.mint_symbol(*req);
      emit_callable_template_args(context_, sink_, resolver_, callee, site,
                                  dst_id);
    }
  }
  if (dst_id < 0)
    return;

  const ReceiverProvenance recv =
      classify_call_receiver(context_, site, callee);

  EdgeRecord e;
  e.src_id = src_id_;
  e.dst_id = dst_id;
  e.kind = 1;
  const int64_t edge_id = sink_.add_edge(e);
  const ExpansionLoc loc = expansion_loc(context_, site->getBeginLoc());
  EdgeSiteRecord siter;
  siter.edge_id = edge_id;
  siter.file_id = file_id_;
  siter.line = loc.line;
  siter.col = loc.col;
  siter.conditional = cond_depth_ > 0 ? 1 : 0;
  if (!recv.src_kind.empty())
    siter.recv_src_kind = recv.src_kind;
  if (!recv.type_usr.empty())
    siter.recv_type_usr = recv.type_usr;
  if (!recv.decl_usr.empty())
    siter.recv_decl_usr = recv.decl_usr;
  siter.recv_param_pos = recv.param_pos;
  siter.recv_type_is_value = recv.type_is_value;
  sink_.add_edge_site(siter);
  emit_call_args(site, llvm::dyn_cast<clang::CallExpr>(site),
                 llvm::dyn_cast<clang::CXXConstructExpr>(site), edge_id);

  // B3 instantiates edges for a non-recovered template specialization.
  if (!recovered)
    emit_instantiation_edges(context_, sink_, mint_, resolver_, src_id_, dst_id,
                             callee, callee_usr);
}

int64_t BodyWalker::emit_site_edge(const clang::Expr *site, int64_t dst_id,
                                   int kind) {
  return emit_site_edge_at(site->getBeginLoc(), dst_id, kind);
}

int64_t BodyWalker::emit_site_edge_at(clang::SourceLocation loc_in,
                                      int64_t dst_id, int kind) {
  EdgeRecord e;
  e.src_id = src_id_;
  e.dst_id = dst_id;
  e.kind = kind;
  const int64_t edge_id = sink_.add_edge(e);
  const ExpansionLoc loc = expansion_loc(context_, loc_in);
  EdgeSiteRecord siter;
  siter.edge_id = edge_id;
  siter.file_id = file_id_;
  siter.line = loc.line;
  siter.col = loc.col;
  siter.conditional = cond_depth_ > 0 ? 1 : 0;
  sink_.add_edge_site(siter);
  return edge_id;
}

void BodyWalker::emit_call_args(const clang::Expr *site,
                                const clang::CallExpr *call,
                                const clang::CXXConstructExpr *ctor,
                                int64_t edge_id) {
  const unsigned nargs = call != nullptr  ? call->getNumArgs()
                         : ctor != nullptr ? ctor->getNumArgs()
                                           : 0;
  const ExpansionLoc loc = expansion_loc(context_, site->getBeginLoc());
  for (unsigned pos = 0; pos < nargs; ++pos) {
    const clang::Expr *arg =
        call != nullptr ? call->getArg(pos) : ctor->getArg(pos);
    if (arg == nullptr)
      continue;
    // A defaulted argument (e.g. std::string's allocator on libstdc++) counts
    // in clang_getNumArguments and libclang emits a call_arg for it; peel_expr
    // does not descend into CXXDefaultArgExpr, so it classifies "unknown" —
    // matching the reference. (On libc++ the same call takes no such arg.)
    const ValueSource vs = classify_value_source(context_, arg);
    if (vs.src_kind == "literal")
      continue;
    CallArgRecord ca;
    ca.edge_id = edge_id;
    ca.file_id = file_id_;
    ca.line = loc.line;
    ca.col = loc.col;
    ca.position = static_cast<int64_t>(pos);
    ca.src_kind = vs.src_kind;
    if (!vs.type_usr.empty())
      ca.type_usr = vs.type_usr;
    if (!vs.decl_usr.empty())
      ca.decl_usr = vs.decl_usr;
    if (!vs.callee_usr.empty())
      ca.callee_usr = vs.callee_usr;
    if ((vs.src_kind == "member" || vs.src_kind == "global" ||
         vs.src_kind == "call_result" || vs.src_kind == "local") &&
        !vs.type_usr.empty()) {
      ca.type_is_value =
          type_is_value(decl_type_for_expr(peel_expr(arg)), vs.type_usr) ? 1
                                                                         : 0;
    }
    sink_.add_call_arg(ca);
  }
}

} // namespace cidx::lt
