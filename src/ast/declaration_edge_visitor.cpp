#include "ast/declaration_edge_visitor.hpp"

#include "ast/edge_sink.hpp"
#include "ast/instantiation_edges.hpp"
#include "ast/kind_map.hpp"
#include "ast/location.hpp"
#include "ast/type_use.hpp"
#include "ast/clang_compat.hpp"
#include "ast/usr.hpp"

#include "clang/Lex/Lexer.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclFriend.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/Basic/SourceManager.h"

namespace cidx::ast {

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
  case clang::Decl::ClassTemplatePartialSpecialization:
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

DeclarationEdgeVisitor::DeclarationEdgeVisitor(clang::ASTContext &context, EdgeSink &sink,
                         std::string target_file, int64_t file_id)
    : context_(context), source_manager_(context.getSourceManager()),
      sink_(sink), mint_(context, sink), targ_encoder_(context, sink),
      minter_(context, sink, mint_, targ_encoder_),
      target_file_(std::move(target_file)), file_id_(file_id) {}

// Signature-level uses(7): return + parameter types (emit_type_use in the
// function-like B1 branch). Constructors/destructors record no return type.
void DeclarationEdgeVisitor::emit_signature_uses(const clang::FunctionDecl *fn) {
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

bool DeclarationEdgeVisitor::in_walk(const clang::Decl *decl) const {
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

// Lookup-only edge between two already-indexed symbols (no stubs minted).
// Returns the src symbol id when the edge was emitted.
std::optional<int64_t>
DeclarationEdgeVisitor::emit_lookup_edge(const std::string &src_usr,
                                         const std::string &dst_usr,
                                         int kind) {
  if (src_usr.empty() || dst_usr.empty())
    return std::nullopt;
  const auto src = sink_.lookup_symbol_id(src_usr);
  const auto dst = sink_.lookup_symbol_id(dst_usr);
  if (!src || !dst)
    return std::nullopt;
  EdgeRecord e;
  e.src_id = *src;
  e.dst_id = *dst;
  e.kind = kind;
  sink_.add_edge(e);
  return src;
}

// -- contains (kind=3) + signature-level uses ---------------------------------
bool DeclarationEdgeVisitor::VisitNamedDecl(clang::NamedDecl *decl) {
  if (!in_walk(decl) || is_template_pattern(decl))
    return true;
  // Function templates get NO signature uses: the retired reference's
  // result-type and argument accessors were undefined on templates.
  if (const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(decl))
    emit_signature_uses(fn);
  emit_contains_edge(decl);
  return true;
}

// contains(3): lexical namespace -> any indexed child; record/class-template
// -> nested type children only.
void DeclarationEdgeVisitor::emit_contains_edge(const clang::NamedDecl *decl) {
  const clang::DeclContext *ldc = decl->getLexicalDeclContext();
  const auto *parent = ldc != nullptr ? llvm::dyn_cast<clang::Decl>(ldc) : nullptr;
  if (parent == nullptr)
    return;
  const ParentKind pk = parent_kind_of(parent);
  const bool emit =
      pk == ParentKind::Namespace ||
      ((pk == ParentKind::Record || pk == ParentKind::ClassTemplate) &&
       is_nested_type_child(decl));
  if (emit)
    emit_lookup_edge(usr_of(parent), usr_of(decl), 3);
}

// -- inherits (kind=2) + CRTP instantiates (kind=5) ---------------------------
bool DeclarationEdgeVisitor::VisitCXXRecordDecl(clang::CXXRecordDecl *decl) {
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

  for (const clang::CXXBaseSpecifier &base : decl->bases())
    emit_base_specifier(derived, derived_usr, base);
  return true;
}

// One inherits(2) edge for a base specifier, plus the CRTP instantiates(5)
// edge when the base is a class-template specialization.
void DeclarationEdgeVisitor::emit_base_specifier(const clang::NamedDecl *derived,
                                      const std::string &derived_usr,
                                      const clang::CXXBaseSpecifier &base) {
  // The base record decl; dependent bases (Base<T> in a template) resolve to
  // their template pattern via the type's declaration when available.
  const clang::CXXRecordDecl *base_rec = base.getType()->getAsCXXRecordDecl();
  if (base_rec == nullptr)
    return;
  const std::string base_usr = usr_for_decl(base_rec);
  if (base_usr.empty())
    return;
  const int64_t src_id = inherits_src_id(derived, derived_usr);
  if (src_id < 0)
    return;
  auto req = mint_.build(base_rec);
  if (!req)
    return;
  const int64_t dst_id = sink_.mint_symbol(*req);

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
  emit_crtp_instantiates(base_rec, base_usr, dst_id);
}

// src of an inherits edge: lookup, else mint (unmapped kinds default to
// "class-template").
int64_t DeclarationEdgeVisitor::inherits_src_id(const clang::NamedDecl *derived,
                                     const std::string &derived_usr) {
  if (const auto sid = sink_.lookup_symbol_id(derived_usr))
    return *sid;
  auto req = mint_.build(derived);
  if (!req)
    return -1;
  if (cidx_symbol_kind_name(derived) == nullptr)
    req->kind_name = "class-template";
  return sink_.mint_symbol(*req);
}

// CRTP/template base: specialization instance -> primary template.
void DeclarationEdgeVisitor::emit_crtp_instantiates(const clang::CXXRecordDecl *base_rec,
                                         const std::string &base_usr,
                                         int64_t dst_id) {
  const auto *spec =
      llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(base_rec);
  if (spec == nullptr)
    return;
  const clang::ClassTemplateDecl *primary = spec->getSpecializedTemplate();
  if (primary == nullptr)
    return;
  const std::string prim_usr = usr_for_decl(primary);
  if (prim_usr.empty() || prim_usr == base_usr)
    return;
  if (const auto prim_id = sink_.lookup_symbol_id(prim_usr)) {
    EdgeRecord inst;
    inst.src_id = dst_id;
    inst.dst_id = *prim_id;
    inst.kind = 5;
    sink_.add_edge(inst);
  }
}

// -- field_of (kind=8) + field type use/mint -----------------------------------
bool DeclarationEdgeVisitor::VisitFieldDecl(clang::FieldDecl *decl) {
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
  emit_lookup_edge(member_usr, usr_of(decl->getParent()), 8);
  return true;
}

// -- VAR_DECL: type use/mint + out-of-line static member definitions ----------
bool DeclarationEdgeVisitor::VisitVarDecl(clang::VarDecl *decl) {
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
      decl->isStaticDataMember())
    emit_static_member_definition(decl, *self);
  return true;
}

void DeclarationEdgeVisitor::emit_static_member_definition(const clang::VarDecl *decl,
                                                int64_t symbol_id) {
  const clang::SourceRange range = decl->getSourceRange();
  const ExpansionLoc start = extent_start(context_, range);
  const ExpansionLoc end = extent_end(context_, range);
  const int64_t def_id = sink_.get_or_create_definition(
      symbol_id, file_id_, start.line, start.col, end.line, end.col,
      static_var_init_text(range));
  // Initializer calls become def_edge USES (emit_static_init_def_edges).
  if (const clang::Expr *init = decl->getInit())
    emit_static_init_def_edges(def_id, init);
}

// Initializer source text after '=' (static_var_init_text): exact slice.
std::optional<std::string>
DeclarationEdgeVisitor::static_var_init_text(clang::SourceRange range) const {
  const clang::SourceManager &sm = context_.getSourceManager();
  const clang::SourceLocation b = sm.getExpansionLoc(range.getBegin());
  const clang::SourceLocation e = clang::Lexer::getLocForEndOfToken(
      sm.getExpansionLoc(range.getEnd()), 0, sm, context_.getLangOpts());
  bool invalid = false;
  // Both ends must sit in the same file buffer: a range whose begin and end
  // expand into different buffers (macro spellings, PCH prefix buffers)
  // yields pointers into unrelated allocations, and ep - bp is garbage.
  const bool same_buffer =
      b.isValid() && e.isValid() && sm.getFileID(b) == sm.getFileID(e);
  const char *bp = same_buffer ? sm.getCharacterData(b, &invalid) : nullptr;
  const char *ep =
      (same_buffer && !invalid) ? sm.getCharacterData(e, &invalid) : nullptr;
  if (invalid || bp == nullptr || ep == nullptr || ep <= bp)
    return std::nullopt;
  const std::string raw(bp, static_cast<size_t>(ep - bp));
  const auto eq = raw.find('=');
  if (eq == std::string::npos)
    return std::nullopt;
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
  if (val.empty())
    return std::nullopt;
  return val;
}

// Call targets anywhere in the initializer become def_edge USES rows.
void DeclarationEdgeVisitor::emit_static_init_def_edges(int64_t def_id,
                                             const clang::Expr *init) {
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

// -- TYPEDEF/TYPE_ALIAS: named-instance mint + underlying type use ------------
bool DeclarationEdgeVisitor::VisitTypedefNameDecl(clang::TypedefNameDecl *decl) {
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
bool DeclarationEdgeVisitor::VisitCXXMethodDecl(clang::CXXMethodDecl *decl) {
  if (!in_walk(decl) || is_template_pattern(decl))
    return true;
  // Method specializations get method_of from the shared identity helper
  // (idempotent); a second counted emission here would duplicate it.
  std::optional<int64_t> src;
  if (callable_template_info(decl))
    src = sink_.lookup_symbol_id(usr_for_decl(decl));
  else
    src = emit_lookup_edge(usr_for_decl(decl), usr_of(decl->getParent()), 9);
  if (!src)
    return true;
  // overrides: plain methods only (not ctors/dtors).
  if (!llvm::isa<clang::CXXConstructorDecl>(decl) &&
      !llvm::isa<clang::CXXDestructorDecl>(decl))
    emit_override_edges(decl, *src);
  return true;
}

// overrides(6): method -> each directly overridden method (minted).
void DeclarationEdgeVisitor::emit_override_edges(
    const clang::CXXMethodDecl *decl, int64_t src_id) {
  for (const clang::CXXMethodDecl *overridden : decl->overridden_methods()) {
    if (auto req = mint_.build(overridden)) {
      const int64_t dst_ov = sink_.mint_symbol(*req);
      EdgeRecord oe;
      oe.src_id = src_id;
      oe.dst_id = dst_ov;
      oe.kind = 6;
      sink_.add_edge(oe);
    }
  }
}


// The record decls a `friend` type declaration references. For
// `friend class B;` that is B itself; for `friend class Singleton<Cache>;`
// the named records are the template ARGUMENT types (yielding the quirky
// self-friend edge the retired reference emitted). Mirror both shapes.
std::vector<const clang::NamedDecl *>
DeclarationEdgeVisitor::friend_targets(const clang::TypeSourceInfo *tsi) const {
  std::vector<const clang::NamedDecl *> refs;
  const clang::Type *type = tsi->getType().getTypePtrOrNull();
  if (type == nullptr)
    return refs;
  if (const auto *tst = type->getAs<clang::TemplateSpecializationType>()) {
    for (const clang::TemplateArgument &arg : tst->template_arguments()) {
      if (arg.getKind() != clang::TemplateArgument::Type)
        continue;
      if (const clang::TagDecl *td = arg.getAsType()->getAsTagDecl())
        refs.push_back(td);
    }
  } else if (const clang::CXXRecordDecl *rec = type->getAsCXXRecordDecl()) {
    const clang::NamedDecl *target = rec;
    if (const clang::ClassTemplateDecl *ct = rec->getDescribedClassTemplate())
      target = ct;
    refs.push_back(target);
  }
  return refs;
}

// -- friend (kind=17) ---------------------------------------------------------
bool DeclarationEdgeVisitor::VisitFriendDecl(clang::FriendDecl *decl) {
  if (!in_walk(decl))
    return true;
  const auto *owner =
      llvm::dyn_cast<clang::CXXRecordDecl>(decl->getDeclContext());
  if (owner == nullptr || owner->isUnion())
    return true; // record-friends of class/struct owners only
  const clang::TypeSourceInfo *tsi = decl->getFriendType();
  if (tsi == nullptr)
    return true; // friend functions are not recorded
  const std::string owner_usr = usr_of(owner);
  for (const clang::NamedDecl *target : friend_targets(tsi))
    emit_lookup_edge(owner_usr, usr_for_decl(target), 17);
  return true;
}

// -- template_param rows (templates + class partial specializations) ----------
void DeclarationEdgeVisitor::emit_template_params(
    const clang::TemplateParameterList *params, int64_t owner_id) {
  int64_t pos = 0;
  for (const clang::NamedDecl *p : *params) {
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

bool DeclarationEdgeVisitor::VisitClassTemplateDecl(clang::ClassTemplateDecl *decl) {
  if (!in_walk(decl))
    return true;
  const std::string usr = usr_for_decl(decl);
  if (usr.empty())
    return true;
  const auto id = sink_.lookup_symbol_id(usr);
  if (!id)
    return true;
  emit_template_params(decl->getTemplateParameters(), *id);
  return true;
}

bool DeclarationEdgeVisitor::VisitFunctionTemplateDecl(
    clang::FunctionTemplateDecl *decl) {
  if (!in_walk(decl))
    return true;
  // Explicit instantiations of this template are reachable only through its
  // specializations() list — handle them here, call sites or none.
  emit_explicit_instantiations(decl);
  const std::string usr = usr_for_decl(decl);
  if (usr.empty())
    return true;
  const auto id = sink_.lookup_symbol_id(usr);
  if (!id)
    return true;
  // A member function template is a method too, but the CXXMethodDecl
  // callback never sees it — emit method_of here.
  const clang::DeclContext *dc = decl->getDeclContext();
  const auto *owner_rec = llvm::dyn_cast_or_null<clang::CXXRecordDecl>(
      dc != nullptr ? llvm::dyn_cast<clang::Decl>(dc) : nullptr);
  if (owner_rec != nullptr)
    emit_lookup_edge(usr, usr_of(owner_rec), 9);
  emit_template_params(decl->getTemplateParameters(), *id);
  return true;
}

// -- callable explicit specializations / instantiations -----------------------
// One shared helper (emit_callable_template_identity) covers flags, arguments,
// display names, and structural edges for BOTH this declaration pass and the
// call path, so the two cannot diverge; every emission in it is idempotent.
void DeclarationEdgeVisitor::emit_callable_identity(
    const clang::FunctionDecl *fd) {
  const auto info = callable_template_info(fd);
  if (!info)
    return;
  auto req = mint_.build(fd);
  if (!req)
    return;
  req->is_instantiation = info->is_instantiation;
  const int64_t dst_id = sink_.mint_symbol(*req);
  emit_callable_template_identity(sink_, mint_, targ_encoder_, dst_id, fd,
                                  *info, {});
}

// Lexically written explicit specializations (free and member) land here.
bool DeclarationEdgeVisitor::VisitFunctionDecl(clang::FunctionDecl *decl) {
  if (!in_walk(decl) || is_template_pattern(decl))
    return true;
  emit_callable_identity(decl);
  return true;
}

void DeclarationEdgeVisitor::emit_explicit_instantiations(
    const clang::FunctionTemplateDecl *tmpl) {
  for (const clang::FunctionDecl *fd : tmpl->specializations()) {
    const clang::TemplateSpecializationKind tsk =
        fd->getTemplateSpecializationKind();
    if (tsk != clang::TSK_ExplicitInstantiationDeclaration &&
        tsk != clang::TSK_ExplicitInstantiationDefinition)
      continue;
    emit_callable_identity(fd);
  }
}

// The indexed symbol id of a class-template specialization whose identity is
// distinct from its primary (nullopt when either USR is missing or equal).
std::optional<int64_t> DeclarationEdgeVisitor::specialization_symbol_id(
    const clang::ClassTemplateSpecializationDecl *decl,
    const clang::ClassTemplateDecl *primary) {
  const std::string spec_usr = usr_for_decl(decl);
  const std::string prim_usr = usr_for_decl(primary);
  if (spec_usr.empty() || prim_usr.empty() || spec_usr == prim_usr)
    return std::nullopt;
  return sink_.lookup_symbol_id(spec_usr);
}

// -- specializes (4) / explicit-instantiation instantiates (5) + template_arg --
bool DeclarationEdgeVisitor::VisitClassTemplateSpecializationDecl(
    clang::ClassTemplateSpecializationDecl *decl) {
  // Partial specializations have their own callback below.
  if (llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(decl))
    return true;
  // Definitions — plus explicit-instantiation DECLARATIONS (`extern template
  // struct Box<long>;`), whose relationship and flag hold without a body.
  const clang::TemplateSpecializationKind tsk = decl->getSpecializationKind();
  const bool explicit_inst =
      tsk == clang::TSK_ExplicitInstantiationDeclaration ||
      tsk == clang::TSK_ExplicitInstantiationDefinition;
  if (!in_walk(decl) ||
      (!decl->isThisDeclarationADefinition() && !explicit_inst))
    return true;
  const clang::ClassTemplateDecl *primary = decl->getSpecializedTemplate();
  if (primary == nullptr)
    return true;
  const auto spec_id = specialization_symbol_id(decl, primary);
  if (!spec_id)
    return true;
  if (!emit_specializes_edge(decl, primary, *spec_id))
    return true;
  // template_arg rows through the one canonical encoder
  // (docs/improvements/template-arg-contract.md).
  const clang::TemplateArgumentList &args = decl->getTemplateArgs();
  for (unsigned ai = 0; ai < args.size(); ++ai)
    targ_encoder_.emit(*spec_id, static_cast<int64_t>(ai), args[ai]);
  return true;
}

// specializes(4) — or instantiates(5) for `template class Foo<int>;` explicit
// instantiations, which the specialization-kind API answers directly. Mints
// the primary's stub. Idempotent: a later concrete use of the same
// specialization must not re-count the relationship.
bool DeclarationEdgeVisitor::emit_specializes_edge(
    const clang::ClassTemplateSpecializationDecl *decl,
    const clang::ClassTemplateDecl *primary, int64_t spec_id) {
  auto req = mint_.build(primary);
  if (!req)
    return false;
  const int64_t prim_id = sink_.mint_symbol(*req);
  const clang::TemplateSpecializationKind tsk = decl->getSpecializationKind();
  const bool explicit_inst =
      tsk == clang::TSK_ExplicitInstantiationDeclaration ||
      tsk == clang::TSK_ExplicitInstantiationDefinition;
  EdgeRecord e;
  e.src_id = spec_id;
  e.dst_id = prim_id;
  e.kind = explicit_inst ? 5 : 4;
  sink_.ensure_edge(e);
  return true;
}

// -- partial specialization: first-class template symbol ----------------------
// Indexed with its own USR, its own parameter list (canonical path), its
// PATTERN arguments (`Box<T*>`'s `T*`), and specializes(4) -> primary.
bool DeclarationEdgeVisitor::VisitClassTemplatePartialSpecializationDecl(
    clang::ClassTemplatePartialSpecializationDecl *decl) {
  if (!in_walk(decl) || !decl->isThisDeclarationADefinition())
    return true;
  const clang::ClassTemplateDecl *primary = decl->getSpecializedTemplate();
  if (primary == nullptr)
    return true;
  const auto spec_id = specialization_symbol_id(decl, primary);
  if (!spec_id)
    return true;
  auto req = mint_.build(primary);
  if (!req)
    return true;
  const int64_t prim_id = sink_.mint_symbol(*req);
  EdgeRecord e;
  e.src_id = *spec_id;
  e.dst_id = prim_id;
  e.kind = 4; // specializes: a partial specialization is authored code
  sink_.ensure_edge(e);
  emit_template_params(decl->getTemplateParameters(), *spec_id);
  // Pattern arguments: the canonical list encodes, the as-written list keeps
  // the authored spelling (`T *`, not `type-parameter-0-0 *`).
  const clang::TemplateArgumentList &args = decl->getTemplateArgs();
  const clang::ASTTemplateArgumentListInfo *written =
      decl->getTemplateArgsAsWritten();
  for (unsigned ai = 0; ai < args.size(); ++ai) {
    clang::QualType w;
    if (written != nullptr && ai < written->NumTemplateArgs) {
      const clang::TemplateArgumentLoc &tal = (*written)[ai];
      if (tal.getArgument().getKind() == clang::TemplateArgument::Type)
        if (const clang::TypeSourceInfo *tsi = tal.getTypeSourceInfo())
          w = tsi->getType();
    }
    targ_encoder_.emit(*spec_id, static_cast<int64_t>(ai), args[ai], w);
  }
  return true;
}

} // namespace cidx::ast
