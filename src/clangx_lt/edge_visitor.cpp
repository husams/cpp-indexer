#include "clangx_lt/edge_visitor.hpp"

#include "clangx_lt/edge_sink.hpp"
#include "clangx_lt/kind_map.hpp"
#include "clangx_lt/location.hpp"
#include "clangx_lt/type_use.hpp"
#include "clangx_lt/usr.hpp"

#include "clang/Lex/Lexer.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclFriend.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/Basic/SourceManager.h"

namespace cidx::lt {

namespace {

// The templated pattern is represented by its template decl (one libclang
// cursor); handlers must not fire twice.
bool is_template_pattern(const clang::Decl *decl) {
  if (const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(decl))
    return fn->getDescribedFunctionTemplate() != nullptr;
  if (const auto *rec = llvm::dyn_cast<clang::CXXRecordDecl>(decl))
    return rec->getDescribedClassTemplate() != nullptr;
  return false;
}

// libclang cursor kind of the lexical parent, reduced to what the contains
// handler needs. A class-template PATTERN record reads as "class-template"
// (libclang's walk parent is the CLASS_TEMPLATE cursor).
enum class ParentKind { Other, Namespace, Record, ClassTemplate };

ParentKind parent_kind_of(const clang::Decl *parent) {
  if (llvm::isa<clang::NamespaceDecl>(parent))
    return ParentKind::Namespace;
  if (const auto *rec = llvm::dyn_cast<clang::CXXRecordDecl>(parent)) {
    if (rec->getDescribedClassTemplate() != nullptr)
      return ParentKind::ClassTemplate;
    if (llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(rec))
      return ParentKind::Other; // not in the contains parent set
    return ParentKind::Record;  // class/struct/union (incl. full spec)
  }
  if (const auto *rec = llvm::dyn_cast<clang::RecordDecl>(parent))
    return rec->isUnion() ? ParentKind::Record : ParentKind::Record;
  return ParentKind::Other;
}

// contains: nested-type child kinds under a record parent
// (child_is_nested_type, ast_edges.cpp:44-48).
bool is_nested_type_child(const clang::Decl *decl) {
  switch (decl->getKind()) {
  case clang::Decl::Record:
  case clang::Decl::CXXRecord:
  case clang::Decl::ClassTemplateSpecialization:
  case clang::Decl::Enum:
  case clang::Decl::Typedef:
  case clang::Decl::TypeAlias:
  case clang::Decl::ClassTemplate:
  case clang::Decl::FunctionTemplate:
    return true;
  default:
    return false;
  }
}

// USR of a decl, treating a template PATTERN as its template (same string —
// generateUSRForDecl looks through, but be explicit for clarity).
std::string usr_of(const clang::Decl *decl) {
  if (const auto *nd = llvm::dyn_cast<clang::NamedDecl>(decl))
    return usr_for_decl(nd);
  return {};
}

} // namespace

EdgeVisitor::EdgeVisitor(clang::ASTContext &context, EdgeSink &sink,
                         std::string target_file, int64_t file_id)
    : context_(context), source_manager_(context.getSourceManager()),
      sink_(sink), mint_(context, sink), arg_resolver_(context, sink),
      minter_(context, sink, mint_, arg_resolver_),
      target_file_(std::move(target_file)), file_id_(file_id) {}

// Signature-level uses(7): return + parameter types (emit_type_use in the
// function-like B1 branch). Constructors/destructors record no return type.
void EdgeVisitor::emit_signature_uses(const clang::FunctionDecl *fn) {
  const clang::NamedDecl *keyed = fn;
  if (const clang::FunctionTemplateDecl *ft =
          fn->getDescribedFunctionTemplate())
    keyed = ft;
  const std::string usr = usr_for_decl(keyed);
  if (usr.empty())
    return;
  const auto fn_sym = sink_.lookup_symbol_id(usr);
  if (!fn_sym)
    return;
  if (!llvm::isa<clang::CXXConstructorDecl>(fn) &&
      !llvm::isa<clang::CXXDestructorDecl>(fn)) {
    emit_type_use(sink_, *fn_sym, fn->getReturnType(), file_id_,
                  expansion_loc(context_, fn->getLocation()), 0);
  }
  for (const clang::ParmVarDecl *p : fn->parameters())
    emit_type_use(sink_, *fn_sym, p->getType(), file_id_,
                  expansion_loc(context_, p->getLocation()), 0);
}

bool EdgeVisitor::in_walk(const clang::Decl *decl) const {
  // for_file_cursors_p: expansion location in the target file...
  const ExpansionLoc loc =
      expansion_loc(context_, decl->getLocation());
  if (loc.file != target_file_)
    return false;
  // ...and the walk stops at function bodies (body edges belong to the
  // body-descent phase, which emits calls/uses only — no decl edges).
  if (decl->getParentFunctionOrMethod() != nullptr)
    return false;
  return true;
}

// -- contains (kind=3) + signature-level uses ---------------------------------
bool EdgeVisitor::VisitNamedDecl(clang::NamedDecl *decl) {
  if (!in_walk(decl) || is_template_pattern(decl))
    return true;
  // Function templates get NO signature uses: the cursor API's result-type
  // and argument accessors return invalid/-1 on FUNCTION_TEMPLATE cursors.
  if (const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(decl))
    emit_signature_uses(fn);
  const clang::DeclContext *ldc = decl->getLexicalDeclContext();
  const auto *parent = ldc != nullptr ? llvm::dyn_cast<clang::Decl>(ldc) : nullptr;
  if (parent == nullptr)
    return true;
  const ParentKind pk = parent_kind_of(parent);
  const bool emit =
      pk == ParentKind::Namespace ||
      ((pk == ParentKind::Record || pk == ParentKind::ClassTemplate) &&
       is_nested_type_child(decl));
  if (!emit)
    return true;
  const std::string child_usr = usr_of(decl);
  const std::string parent_usr = usr_of(parent);
  if (child_usr.empty() || parent_usr.empty())
    return true;
  const auto child_id = sink_.lookup_symbol_id(child_usr);
  const auto parent_id = sink_.lookup_symbol_id(parent_usr);
  if (!child_id || !parent_id)
    return true;
  EdgeRecord e;
  e.src_id = *parent_id;
  e.dst_id = *child_id;
  e.kind = 3;
  sink_.add_edge(e);
  return true;
}

// -- libclang duplicate-cursor quirk for `typedef struct X {...} Y;` ----------
// libclang shows an inline-defined tag BOTH as its own top-level cursor AND as
// a child of the TYPEDEF_DECL, so every member edge under it is emitted twice
// (field_of count=2 on the corpus C structs). RAV visits the tag once; re-walk
// its subtree from the typedef to mirror the doubled emissions.
bool EdgeVisitor::TraverseTypedefDecl(clang::TypedefDecl *decl) {
  if (!RecursiveASTVisitor::TraverseTypedefDecl(decl))
    return false;
  if (!in_walk(decl))
    return true;
  const clang::Type *type = decl->getUnderlyingType().getTypePtrOrNull();
  if (type == nullptr)
    return true;
  if (const clang::TagDecl *tag = type->getAsTagDecl()) {
    if (tag->isThisDeclarationADefinition() && tag->isEmbeddedInDeclarator())
      RecursiveASTVisitor::TraverseDecl(
          const_cast<clang::TagDecl *>(tag));
  }
  return true;
}

// -- inherits (kind=2) + CRTP instantiates (kind=5) ---------------------------
bool EdgeVisitor::VisitCXXRecordDecl(clang::CXXRecordDecl *decl) {
  // Base specifiers hang off the DEFINITION of the derived record; libclang
  // emits them as children of the record (or its class template / partial
  // specialization). The pattern record stands in for the template cursor.
  if (!in_walk(decl) || !decl->isThisDeclarationADefinition())
    return true;
  const clang::NamedDecl *derived = decl;
  if (const clang::ClassTemplateDecl *ct = decl->getDescribedClassTemplate())
    derived = ct;
  const std::string derived_usr = usr_for_decl(derived);
  if (derived_usr.empty())
    return true;

  for (const clang::CXXBaseSpecifier &base : decl->bases()) {
    // clang_getCursorReferenced(base) -> the base record decl; dependent
    // bases (Base<T> in a template) resolve to their template pattern via
    // the type's declaration when available.
    const clang::CXXRecordDecl *base_rec =
        base.getType()->getAsCXXRecordDecl();
    if (base_rec == nullptr)
      continue;
    const std::string base_usr = usr_for_decl(base_rec);
    if (base_usr.empty())
      continue;

    // src: lookup, else mint (partial specs are not indexed as symbols).
    int64_t src_id = 0;
    if (const auto sid = sink_.lookup_symbol_id(derived_usr)) {
      src_id = *sid;
    } else if (auto req = mint_.build(llvm::cast<clang::NamedDecl>(
                   const_cast<clang::NamedDecl *>(derived)))) {
      // kind: mapped kinds cover every accepted parent except the partial
      // spec, which defaults to "class-template" (ast_edges.cpp comment).
      if (cidx_symbol_kind_name(derived) == nullptr)
        req->kind_name = "class-template";
      src_id = sink_.mint_symbol(*req);
    } else {
      continue;
    }

    int64_t dst_id = 0;
    if (auto req = mint_.build(base_rec)) {
      dst_id = sink_.mint_symbol(*req);
    } else {
      continue;
    }

    EdgeRecord e;
    e.src_id = src_id;
    e.dst_id = dst_id;
    e.kind = 2;
    switch (base.getAccessSpecifier()) {
    case clang::AS_public:    e.base_access = 1; break;
    case clang::AS_protected: e.base_access = 2; break;
    case clang::AS_private:   e.base_access = 3; break;
    case clang::AS_none:      break;
    }
    e.is_virtual = base.isVirtual() ? 1 : 0;
    sink_.add_edge(e);

    // CRTP/template base: specialization instance -> primary template.
    if (const auto *spec =
            llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(base_rec)) {
      const clang::ClassTemplateDecl *primary = spec->getSpecializedTemplate();
      if (primary != nullptr) {
        const std::string prim_usr = usr_for_decl(primary);
        if (!prim_usr.empty() && prim_usr != base_usr) {
          if (const auto prim_id = sink_.lookup_symbol_id(prim_usr)) {
            EdgeRecord inst;
            inst.src_id = dst_id;
            inst.dst_id = *prim_id;
            inst.kind = 5;
            sink_.add_edge(inst);
          }
        }
      }
    }
  }
  return true;
}

// -- field_of (kind=8) + field type use/mint -----------------------------------
bool EdgeVisitor::VisitFieldDecl(clang::FieldDecl *decl) {
  if (!in_walk(decl))
    return true;
  const std::string member_usr = usr_for_decl(decl);
  if (member_usr.empty())
    return true;
  // FIELD_DECL branch: mint the X<B> instance FIRST, then the structural
  // uses(7) field -> its declared type.
  minter_.mint_instance_from_type(decl->getType());
  if (const auto self = sink_.lookup_symbol_id(member_usr))
    emit_type_use(sink_, *self, decl->getType(), file_id_,
                  expansion_loc(context_, decl->getLocation()), 0);
  const std::string owner_usr = usr_of(decl->getParent());
  if (owner_usr.empty())
    return true;
  const auto src = sink_.lookup_symbol_id(member_usr);
  const auto dst = sink_.lookup_symbol_id(owner_usr);
  if (!src || !dst)
    return true;
  EdgeRecord e;
  e.src_id = *src;
  e.dst_id = *dst;
  e.kind = 8;
  sink_.add_edge(e);
  return true;
}

// -- VAR_DECL: type use/mint + out-of-line static member definitions ----------
bool EdgeVisitor::VisitVarDecl(clang::VarDecl *decl) {
  if (!in_walk(decl) || llvm::isa<clang::ParmVarDecl>(decl))
    return true;
  const std::string usr = usr_for_decl(decl);
  if (usr.empty())
    return true;
  minter_.mint_instance_from_type(decl->getType());
  const auto self = sink_.lookup_symbol_id(usr);
  if (!self)
    return true;
  emit_type_use(sink_, *self, decl->getType(), file_id_,
                expansion_loc(context_, decl->getLocation()), 0);
  // v27: out-of-line static DATA MEMBER definition — a per-backend body.
  if (decl->isThisDeclarationADefinition() == clang::VarDecl::Definition &&
      decl->isStaticDataMember()) {
    const clang::SourceRange range = decl->getSourceRange();
    const ExpansionLoc start = extent_start(context_, range);
    const ExpansionLoc end = extent_end(context_, range);
    // Initializer source text after '=' (static_var_init_text): exact slice.
    std::optional<std::string> init_text;
    {
      const clang::SourceManager &sm = context_.getSourceManager();
      const clang::SourceLocation b = sm.getExpansionLoc(range.getBegin());
      const clang::SourceLocation e = clang::Lexer::getLocForEndOfToken(
          sm.getExpansionLoc(range.getEnd()), 0, sm, context_.getLangOpts());
      bool invalid = false;
      const char *bp = sm.getCharacterData(b, &invalid);
      const char *ep = invalid ? nullptr : sm.getCharacterData(e, &invalid);
      if (!invalid && bp != nullptr && ep != nullptr && ep > bp) {
        std::string raw(bp, static_cast<size_t>(ep - bp));
        const auto eq = raw.find('=');
        if (eq != std::string::npos) {
          const auto strip = [](std::string s) {
            const char *ws = " \t\r\n\f\v";
            const auto b2 = s.find_first_not_of(ws);
            if (b2 == std::string::npos)
              return std::string();
            const auto e2 = s.find_last_not_of(ws);
            return s.substr(b2, e2 - b2 + 1);
          };
          std::string val = strip(raw.substr(eq + 1));
          while (!val.empty() && val.back() == ';')
            val.pop_back();
          val = strip(val);
          if (!val.empty())
            init_text = val;
        }
      }
    }
    const int64_t def_id = sink_.get_or_create_definition(
        *self, file_id_, start.line, start.col, end.line, end.col, init_text);
    // Initializer calls become def_edge USES (emit_static_init_def_edges).
    if (const clang::Expr *init = decl->getInit()) {
      std::vector<const clang::Stmt *> stack{init};
      while (!stack.empty()) {
        const clang::Stmt *s = stack.back();
        stack.pop_back();
        if (s == nullptr)
          continue;
        if (const auto *call = llvm::dyn_cast<clang::CallExpr>(s)) {
          if (const auto *fd = llvm::dyn_cast_or_null<clang::FunctionDecl>(
                  call->getCalleeDecl())) {
            const std::string cu = usr_for_decl(fd);
            if (!cu.empty())
              if (const auto cid = sink_.lookup_symbol_id(cu))
                sink_.add_def_edge(def_id, *cid, 7);
          }
        }
        for (const clang::Stmt *c : s->children())
          stack.push_back(c);
      }
    }
  }
  return true;
}

// -- TYPEDEF/TYPE_ALIAS: named-instance mint + underlying type use ------------
bool EdgeVisitor::VisitTypedefNameDecl(clang::TypedefNameDecl *decl) {
  if (!in_walk(decl))
    return true;
  if (const auto *alias = llvm::dyn_cast<clang::TypeAliasDecl>(decl))
    if (alias->getDescribedAliasTemplate() != nullptr)
      return true; // pattern of an alias template — not a symbol
  const std::string usr = usr_for_decl(decl);
  if (usr.empty())
    return true;
  minter_.mint_named_instance(decl); // minted FIRST (order-dependent)
  if (const auto self = sink_.lookup_symbol_id(usr))
    emit_type_use(sink_, *self, decl->getUnderlyingType(), file_id_,
                  expansion_loc(context_, decl->getLocation()), 0);
  return true;
}

// -- method_of (kind=9) + overrides (kind=6) ----------------------------------
bool EdgeVisitor::VisitCXXMethodDecl(clang::CXXMethodDecl *decl) {
  if (!in_walk(decl) || is_template_pattern(decl))
    return true;
  const std::string method_usr = usr_for_decl(decl);
  if (method_usr.empty())
    return true;
  const std::string owner_usr = usr_of(decl->getParent());
  if (owner_usr.empty())
    return true;
  const auto src = sink_.lookup_symbol_id(method_usr);
  const auto dst = sink_.lookup_symbol_id(owner_usr);
  if (!src || !dst)
    return true;
  EdgeRecord e;
  e.src_id = *src;
  e.dst_id = *dst;
  e.kind = 9;
  sink_.add_edge(e);

  // overrides: plain CXX_METHOD cursors only (not ctors/dtors).
  if (!llvm::isa<clang::CXXConstructorDecl>(decl) &&
      !llvm::isa<clang::CXXDestructorDecl>(decl)) {
    for (const clang::CXXMethodDecl *overridden : decl->overridden_methods()) {
      if (auto req = mint_.build(overridden)) {
        const int64_t dst_ov = sink_.mint_symbol(*req);
        EdgeRecord oe;
        oe.src_id = *src;
        oe.dst_id = dst_ov;
        oe.kind = 6;
        sink_.add_edge(oe);
      }
    }
  }
  return true;
}

// -- friend (kind=17) ---------------------------------------------------------
bool EdgeVisitor::VisitFriendDecl(clang::FriendDecl *decl) {
  if (!in_walk(decl))
    return true;
  const auto *owner =
      llvm::dyn_cast<clang::CXXRecordDecl>(decl->getDeclContext());
  if (owner == nullptr || owner->isUnion())
    return true; // record-friends of class/struct owners only
  const std::string owner_usr = usr_of(owner);
  if (owner_usr.empty())
    return true;
  const auto src = sink_.lookup_symbol_id(owner_usr);
  if (!src)
    return true;
  // libclang collects the FriendDecl's direct TYPE_REF children. For
  // `friend class B;` that is B itself; for `friend class Singleton<Cache>;`
  // the TEMPLATE_REF (Singleton) is NOT a TYPE_REF, so the collected refs are
  // the template ARGUMENT records (yielding the quirky self-friend edge the
  // reference emits). Mirror both shapes.
  const clang::TypeSourceInfo *tsi = decl->getFriendType();
  if (tsi == nullptr)
    return true; // friend functions are not recorded
  std::vector<const clang::NamedDecl *> refs;
  const clang::Type *type = tsi->getType().getTypePtrOrNull();
  if (type != nullptr) {
    if (const auto *tst = type->getAs<clang::TemplateSpecializationType>()) {
      for (const clang::TemplateArgument &arg : tst->template_arguments()) {
        if (arg.getKind() != clang::TemplateArgument::Type)
          continue;
        if (const clang::TagDecl *td = arg.getAsType()->getAsTagDecl())
          refs.push_back(td);
      }
    } else if (const clang::CXXRecordDecl *rec = type->getAsCXXRecordDecl()) {
      const clang::NamedDecl *target = rec;
      if (const clang::ClassTemplateDecl *ct =
              rec->getDescribedClassTemplate())
        target = ct;
      refs.push_back(target);
    }
  }
  for (const clang::NamedDecl *target : refs) {
    const std::string friend_usr = usr_for_decl(target);
    if (friend_usr.empty())
      continue;
    const auto dst = sink_.lookup_symbol_id(friend_usr);
    if (!dst)
      continue; // lookup-only (no stub)
    EdgeRecord e;
    e.src_id = *src;
    e.dst_id = *dst;
    e.kind = 17;
    sink_.add_edge(e);
  }
  return true;
}

// -- template_param rows (class/function templates) ----------------------------
void EdgeVisitor::emit_template_params(const clang::TemplateDecl *tmpl,
                                       int64_t owner_id) {
  int64_t pos = 0;
  for (const clang::NamedDecl *p : *tmpl->getTemplateParameters()) {
    TemplateParamRecord rec;
    rec.owner_id = owner_id;
    rec.position = pos++;
    if (llvm::isa<clang::TemplateTypeParmDecl>(p))
      rec.param_kind = 1;
    else if (llvm::isa<clang::NonTypeTemplateParmDecl>(p))
      rec.param_kind = 2;
    else if (llvm::isa<clang::TemplateTemplateParmDecl>(p))
      rec.param_kind = 3;
    else
      continue;
    const std::string name = p->getNameAsString();
    if (!name.empty())
      rec.name = name;
    sink_.add_template_param(rec);
  }
}

bool EdgeVisitor::VisitClassTemplateDecl(clang::ClassTemplateDecl *decl) {
  if (!in_walk(decl))
    return true;
  const std::string usr = usr_for_decl(decl);
  if (usr.empty())
    return true;
  const auto id = sink_.lookup_symbol_id(usr);
  if (!id)
    return true;
  emit_template_params(decl, *id);
  return true;
}

bool EdgeVisitor::VisitFunctionTemplateDecl(
    clang::FunctionTemplateDecl *decl) {
  if (!in_walk(decl))
    return true;
  const std::string usr = usr_for_decl(decl);
  if (usr.empty())
    return true;
  const auto id = sink_.lookup_symbol_id(usr);
  if (!id)
    return true;
  // A member function template is a method too, but the CXX_METHOD handler
  // never sees it (cursor kind FUNCTION_TEMPLATE) — emit method_of here.
  const clang::DeclContext *dc = decl->getDeclContext();
  if (const auto *owner_rec =
          llvm::dyn_cast_or_null<clang::CXXRecordDecl>(
              dc != nullptr ? llvm::dyn_cast<clang::Decl>(dc) : nullptr)) {
    const std::string owner_usr = usr_of(owner_rec);
    if (!owner_usr.empty()) {
      if (const auto owner_id = sink_.lookup_symbol_id(owner_usr)) {
        EdgeRecord mo;
        mo.src_id = *id;
        mo.dst_id = *owner_id;
        mo.kind = 9;
        sink_.add_edge(mo);
      }
    }
  }
  emit_template_params(decl, *id);
  return true;
}

// -- specializes (4) / explicit-instantiation instantiates (5) + template_arg --
bool EdgeVisitor::VisitClassTemplateSpecializationDecl(
    clang::ClassTemplateSpecializationDecl *decl) {
  // Mirrors the STRUCT/CLASS_DECL specializes handler: definitions only;
  // partial specializations are a different cursor kind and skipped.
  if (llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(decl))
    return true;
  if (!in_walk(decl) || !decl->isThisDeclarationADefinition())
    return true;
  const clang::ClassTemplateDecl *primary = decl->getSpecializedTemplate();
  if (primary == nullptr)
    return true;
  const std::string spec_usr = usr_for_decl(decl);
  const std::string prim_usr = usr_for_decl(primary);
  if (spec_usr.empty() || prim_usr.empty() || spec_usr == prim_usr)
    return true;
  const auto spec_id = sink_.lookup_symbol_id(spec_usr);
  if (!spec_id)
    return true;
  int64_t prim_id = 0;
  if (auto req = mint_.build(primary)) {
    prim_id = sink_.mint_symbol(*req);
  } else {
    return true;
  }

  // `template class Foo<int>;` (explicit instantiation) is an INSTANCE of the
  // template (kind 5); `template <> class Foo<bool> {...}` stays a
  // specialization (kind 4). The C++ API answers directly what the libclang
  // pass had to recover from source tokens (is_explicit_instantiation).
  const clang::TemplateSpecializationKind tsk = decl->getSpecializationKind();
  const bool explicit_inst =
      tsk == clang::TSK_ExplicitInstantiationDeclaration ||
      tsk == clang::TSK_ExplicitInstantiationDefinition;
  EdgeRecord e;
  e.src_id = *spec_id;
  e.dst_id = prim_id;
  e.kind = explicit_inst ? 5 : 4;
  sink_.add_edge(e);

  // template_arg rows. TYPE args always carry the type spelling in `literal`.
  const clang::TemplateArgumentList &args = decl->getTemplateArgs();
  const clang::PrintingPolicy &policy = context_.getPrintingPolicy();
  for (unsigned ai = 0; ai < args.size(); ++ai) {
    const clang::TemplateArgument &arg = args[ai];
    TemplateArgRecord ta;
    ta.owner_id = *spec_id;
    ta.position = static_cast<int64_t>(ai);
    switch (arg.getKind()) {
    case clang::TemplateArgument::Type: {
      ta.arg_kind = 1;
      const clang::QualType type = arg.getAsType();
      const std::string spelling = type.getAsString(policy);
      if (!spelling.empty())
        ta.literal = spelling;
      if (const clang::TagDecl *td = type->getAsTagDecl()) {
        const std::string ref_usr = usr_for_decl(td);
        if (!ref_usr.empty())
          ta.ref_id = sink_.lookup_symbol_id(ref_usr);
      }
      if (!ta.ref_id)
        ta.ref_id = arg_resolver_.resolve(ta.literal, decl);
      break;
    }
    case clang::TemplateArgument::Integral:
      ta.arg_kind = 2;
      ta.literal = llvm::toString(arg.getAsIntegral(), 10);
      break;
    default:
      // Raw CXTemplateArgumentKind values (Null=0, Declaration=2, NullPtr=3,
      // Template=5, TemplateExpansion=6, Expression=7, Pack=8).
      switch (arg.getKind()) {
      case clang::TemplateArgument::Null:       ta.arg_kind = 0; break;
      case clang::TemplateArgument::Declaration: ta.arg_kind = 2; break;
      case clang::TemplateArgument::NullPtr:    ta.arg_kind = 3; break;
      case clang::TemplateArgument::Template:   ta.arg_kind = 5; break;
      case clang::TemplateArgument::TemplateExpansion: ta.arg_kind = 6; break;
      case clang::TemplateArgument::Expression: ta.arg_kind = 7; break;
      case clang::TemplateArgument::Pack:       ta.arg_kind = 8; break;
      default:                                  ta.arg_kind = 0; break;
      }
      break;
    }
    sink_.add_template_arg(ta);
  }
  return true;
}

} // namespace cidx::lt
